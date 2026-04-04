//===-- Socket query engine (generic + ops dispatch) -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Generic socket query functions. SOL_SOCKET options are handled here
// (AF-agnostic). Protocol-level options and name queries dispatch to
// the ops table. All transient buffers live in scratch — no stack arrays.
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "hdr/types/socklen_t.h"
#include "hdr/types/struct_sockaddr.h"
#include "hdr/types/struct_sockaddr_un.h"
#include "hdr/types/struct_timeval.h"
#include "include/llvm-libc-macros/sys-socket-macros.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ipc/afd_core.h"
#include "src/__support/OSUtil/windows/ipc/af_ops.h"
#include "src/__support/OSUtil/windows/ipc/fifo_channel.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/nt/nt_afd.h"
#include "src/__support/OSUtil/windows/nt/nt_process.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

//===----------------------------------------------------------------------===//
// Scratch layout for SOL_SOCKET option ioctls
//===----------------------------------------------------------------------===//

struct SolSocketWork {
  AFD_INFORMATION info;
  IO_STATUS_BLOCK iosb;
};

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

static long store_int(void *optval, socklen_t *optlen, int val) {
  if (*optlen < sizeof(int)) {
    *optlen = sizeof(int);
    return -EINVAL;
  }
  __builtin_memcpy(optval, &val, sizeof(int));
  *optlen = sizeof(int);
  return 0;
}

/// Set or clear a boolean bit in SocketState::socket_options. One locked
/// RMW per call; no read-modify-store race with concurrent setters for
/// different bits.
static void set_option_bit(SocketState *state, uint32_t bit, bool on) {
  if (on)
    state->socket_options.fetch_or(bit, cpp::MemoryOrder::ACQ_REL);
  else
    state->socket_options.fetch_and(~bit, cpp::MemoryOrder::ACQ_REL);
}

//===----------------------------------------------------------------------===//
// getsockopt
//===----------------------------------------------------------------------===//

intptr_t getsockopt(int sockfd, int level, int optname, void *optval,
                    socklen_t *optlen) {
  auto *ofd = fd_table.get_ofd(sockfd);
  if (!ofd || !ofd->is_socket())
    return ofd ? -ENOTSOCK : -EBADF;

  if (!optval || !optlen)
    return -EINVAL;

  // Socketpair: return synthetic values.
  if (ofd->kind == FileKind::SocketPair) {
    switch (optname) {
    case SO_TYPE:
      return store_int(optval, optlen, SOCK_STREAM);
    case SO_ERROR:
      return store_int(optval, optlen, 0);
    case SO_ACCEPTCONN:
      return store_int(optval, optlen, 0);
    case SO_RCVBUF:
      return store_int(optval, optlen,
                       static_cast<int>(FIFO_RING_CAPACITY));
    case SO_SNDBUF:
      return store_int(optval, optlen,
                       static_cast<int>(FIFO_RING_CAPACITY));
    case SO_PEERCRED:
      return store_int(optval, optlen,
                       static_cast<int>(NtCurrentProcessId()));
    case SO_REUSEADDR:
    case SO_KEEPALIVE:
      return store_int(optval, optlen, 0);
    default:
      return -ENOPROTOOPT;
    }
  }

  HANDLE h = ofd->handle;

  // AFD socket.
  auto *state = ofd->afd_socket();
  auto phase = state->phase.load(cpp::MemoryOrder::ACQUIRE);
  if (phase == SocketPhase::CONNECTING) {
    NTSTATUS cs = socket_probe_connect_completion(state);
    if (optname == SO_ERROR && cs == STATUS_PENDING)
      return store_int(optval, optlen, 0);
    if (cs == STATUS_PENDING && optname != SO_TYPE &&
        optname != SO_ACCEPTCONN)
      return -EAGAIN;
    phase = state->phase.load(cpp::MemoryOrder::ACQUIRE);
  }

  // SOL_SOCKET options — handled generically.
  if (level == SOL_SOCKET) {
    switch (optname) {
    case SO_TYPE:
      return store_int(optval, optlen, state->type);

    case SO_ERROR:
      return store_int(optval, optlen, socket_take_error(state));

    case SO_ACCEPTCONN:
      return store_int(
          optval, optlen,
          phase == SocketPhase::LISTENING ? 1 : 0);

    case SO_RCVBUF: {
      auto *w =
          reinterpret_cast<SolSocketWork *>(socket_scratch(state));
      __builtin_memset(w, 0, sizeof(SolSocketWork));
      w->info.InformationType = AFD_RECEIVE_WINDOW_SIZE;
      NTSTATUS s =
          afd_ioctl(h, IOCTL_AFD_GET_INFORMATION,
                    &w->info, sizeof(w->info), &w->info, sizeof(w->info),
                    &w->iosb);
      if (!NT_SUCCESS(s))
        return -ntstatus_to_errno_socket(s);
      return store_int(optval, optlen,
                       static_cast<int>(w->info.Information.Ulong));
    }

    case SO_SNDBUF: {
      auto *w =
          reinterpret_cast<SolSocketWork *>(socket_scratch(state));
      __builtin_memset(w, 0, sizeof(SolSocketWork));
      w->info.InformationType = AFD_SEND_WINDOW_SIZE;
      NTSTATUS s =
          afd_ioctl(h, IOCTL_AFD_GET_INFORMATION,
                    &w->info, sizeof(w->info), &w->info, sizeof(w->info),
                    &w->iosb);
      if (!NT_SUCCESS(s))
        return -ntstatus_to_errno_socket(s);
      return store_int(optval, optlen,
                       static_cast<int>(w->info.Information.Ulong));
    }

    case SO_REUSEADDR: {
      uint32_t opts = state->socket_options.load(cpp::MemoryOrder::ACQUIRE);
      return store_int(optval, optlen,
                       (opts & SocketState::OPT_REUSEADDR) ? 1 : 0);
    }

    case SO_KEEPALIVE: {
      uint32_t opts = state->socket_options.load(cpp::MemoryOrder::ACQUIRE);
      return store_int(optval, optlen,
                       (opts & SocketState::OPT_KEEPALIVE) ? 1 : 0);
    }

    case SO_BROADCAST: {
      uint32_t opts = state->socket_options.load(cpp::MemoryOrder::ACQUIRE);
      return store_int(optval, optlen,
                       (opts & SocketState::OPT_BROADCAST) ? 1 : 0);
    }

    case SO_OOBINLINE: {
      uint32_t opts = state->socket_options.load(cpp::MemoryOrder::ACQUIRE);
      return store_int(optval, optlen,
                       (opts & SocketState::OPT_OOBINLINE) ? 1 : 0);
    }

    case SO_DONTROUTE: {
      uint32_t opts = state->socket_options.load(cpp::MemoryOrder::ACQUIRE);
      return store_int(optval, optlen,
                       (opts & SocketState::OPT_DONTROUTE) ? 1 : 0);
    }

    case SO_LINGER: {
      // POSIX `struct linger { int l_onoff; int l_linger; }`; writing
      // two 32-bit ints matches every in-tree ABI.
      if (*optlen < 2 * sizeof(int)) {
        *optlen = 2 * sizeof(int);
        return -EINVAL;
      }
      uint32_t opts = state->socket_options.load(cpp::MemoryOrder::ACQUIRE);
      int pair[2] = {
          (opts & SocketState::OPT_LINGER) ? 1 : 0,
          static_cast<int>(
              state->linger_seconds.load(cpp::MemoryOrder::ACQUIRE)),
      };
      __builtin_memcpy(optval, pair, sizeof(pair));
      *optlen = sizeof(pair);
      return 0;
    }

    case SO_RCVTIMEO:
    case SO_SNDTIMEO: {
      if (*optlen < sizeof(struct timeval)) {
        *optlen = sizeof(struct timeval);
        return -EINVAL;
      }
      int64_t us = (optname == SO_RCVTIMEO)
                       ? state->rcvtimeo_us.load(cpp::MemoryOrder::ACQUIRE)
                       : state->sndtimeo_us.load(cpp::MemoryOrder::ACQUIRE);
      struct timeval tv;
      tv.tv_sec = static_cast<decltype(tv.tv_sec)>(us / 1000000);
      tv.tv_usec = static_cast<decltype(tv.tv_usec)>(us % 1000000);
      __builtin_memcpy(optval, &tv, sizeof(tv));
      *optlen = sizeof(tv);
      return 0;
    }

    default:
      // Fall through to AF-specific handler for SOL_SOCKET options
      // that are AF-specific (e.g., SO_PEERCRED for AF_UNIX).
      break;
    }
  }

  // Dispatch to AF-specific option handler.
  return state->ops->do_getsockopt(h, state, level, optname, optval, optlen);
}

//===----------------------------------------------------------------------===//
// setsockopt
//===----------------------------------------------------------------------===//

intptr_t setsockopt(int sockfd, int level, int optname, const void *optval,
                    socklen_t optlen) {
  auto *ofd = fd_table.get_ofd(sockfd);
  if (!ofd || !ofd->is_socket())
    return ofd ? -ENOTSOCK : -EBADF;

  if (!optval || optlen < sizeof(int))
    return -EINVAL;

  int val;
  __builtin_memcpy(&val, optval, sizeof(int));

  // Socketpair: advisory only.
  if (ofd->kind == FileKind::SocketPair) {
    switch (optname) {
    case SO_RCVBUF:
    case SO_SNDBUF:
    case SO_KEEPALIVE:
    case SO_REUSEADDR:
      return 0;
    default:
      return -ENOPROTOOPT;
    }
  }

  HANDLE h = ofd->handle;
  auto *state = ofd->afd_socket();
  if (state->phase.load(cpp::MemoryOrder::ACQUIRE) ==
      SocketPhase::CONNECTING) {
    NTSTATUS cs = socket_probe_connect_completion(state);
    if (cs == STATUS_PENDING)
      return -EAGAIN;
  }

  // SOL_SOCKET options — handled generically.
  if (level == SOL_SOCKET) {
    switch (optname) {
    case SO_RCVBUF: {
      auto *w =
          reinterpret_cast<SolSocketWork *>(socket_scratch(state));
      __builtin_memset(w, 0, sizeof(SolSocketWork));
      w->info.InformationType = AFD_RECEIVE_WINDOW_SIZE;
      w->info.Information.Ulong = static_cast<ULONG>(val);
      NTSTATUS s =
          afd_ioctl(h, IOCTL_AFD_SET_INFORMATION,
                    &w->info, sizeof(w->info), nullptr, 0, &w->iosb);
      if (!NT_SUCCESS(s))
        return -ntstatus_to_errno_socket(s);
      return 0;
    }
    case SO_SNDBUF: {
      auto *w =
          reinterpret_cast<SolSocketWork *>(socket_scratch(state));
      __builtin_memset(w, 0, sizeof(SolSocketWork));
      w->info.InformationType = AFD_SEND_WINDOW_SIZE;
      w->info.Information.Ulong = static_cast<ULONG>(val);
      NTSTATUS s =
          afd_ioctl(h, IOCTL_AFD_SET_INFORMATION,
                    &w->info, sizeof(w->info), nullptr, 0, &w->iosb);
      if (!NT_SUCCESS(s))
        return -ntstatus_to_errno_socket(s);
      return 0;
    }
    case SO_REUSEADDR:
      set_option_bit(state, SocketState::OPT_REUSEADDR, val);
      return 0;

    case SO_KEEPALIVE:
      set_option_bit(state, SocketState::OPT_KEEPALIVE, val);
      // AF_INET forwards to the transport; AF_UNIX's do_setsockopt returns
      // -ENOPROTOOPT which we swallow.
      {
        intptr_t r = state->ops->do_setsockopt(h, state, level, optname,
                                               optval, optlen);
        if (r < 0 && r != -ENOPROTOOPT)
          return r;
      }
      return 0;

    case SO_BROADCAST:
      set_option_bit(state, SocketState::OPT_BROADCAST, val);
      // Forward so the AF_INET transport can enable its IP_BROADCAST path.
      {
        intptr_t r = state->ops->do_setsockopt(h, state, level, optname,
                                               optval, optlen);
        if (r < 0 && r != -ENOPROTOOPT)
          return r;
      }
      return 0;

    case SO_OOBINLINE:
      set_option_bit(state, SocketState::OPT_OOBINLINE, val);
      return 0;

    case SO_DONTROUTE:
      set_option_bit(state, SocketState::OPT_DONTROUTE, val);
      return 0;

    case SO_LINGER: {
      if (optlen < 2 * sizeof(int))
        return -EINVAL;
      int pair[2];
      __builtin_memcpy(pair, optval, sizeof(pair));
      int onoff = pair[0];
      int seconds = pair[1];
      if (seconds < 0)
        return -EINVAL;
      state->linger_seconds.store(static_cast<uint32_t>(seconds),
                                  cpp::MemoryOrder::RELEASE);
      set_option_bit(state, SocketState::OPT_LINGER, onoff != 0);
      return 0;
    }

    case SO_RCVTIMEO:
    case SO_SNDTIMEO: {
      if (optlen < sizeof(struct timeval))
        return -EINVAL;
      struct timeval tv;
      __builtin_memcpy(&tv, optval, sizeof(tv));
      if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000)
        return -EINVAL;
      int64_t us = static_cast<int64_t>(tv.tv_sec) * 1000000 +
                   static_cast<int64_t>(tv.tv_usec);
      if (optname == SO_RCVTIMEO)
        state->rcvtimeo_us.store(us, cpp::MemoryOrder::RELEASE);
      else
        state->sndtimeo_us.store(us, cpp::MemoryOrder::RELEASE);
      return 0;
    }

    default:
      break;
    }
  }

  // Dispatch to AF-specific option handler.
  return state->ops->do_setsockopt(h, state, level, optname, optval, optlen);
}

//===----------------------------------------------------------------------===//
// getsockname
//===----------------------------------------------------------------------===//

intptr_t getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
  auto *ofd = fd_table.get_ofd(sockfd);
  if (!ofd || !ofd->is_socket())
    return ofd ? -ENOTSOCK : -EBADF;

  // Socketpair: return unnamed AF_UNIX address directly to output.
  if (ofd->kind == FileKind::SocketPair) {
    socklen_t actual = sizeof(struct sockaddr_un);
    socklen_t copy = *addrlen;
    if (copy > actual)
      copy = actual;
    __builtin_memset(addr, 0, copy);
    if (copy >= 2) {
      uint16_t family = AF_UNIX;
      __builtin_memcpy(addr, &family, 2);
    }
    *addrlen = actual;
    return 0;
  }

  HANDLE h = ofd->handle;
  auto *state = ofd->afd_socket();
  return state->ops->do_getsockname(h, state, addr, addrlen);
}

//===----------------------------------------------------------------------===//
// getpeername
//===----------------------------------------------------------------------===//

intptr_t getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
  auto *ofd = fd_table.get_ofd(sockfd);
  if (!ofd || !ofd->is_socket())
    return ofd ? -ENOTSOCK : -EBADF;

  // Socketpair: peer is unnamed.
  if (ofd->kind == FileKind::SocketPair) {
    socklen_t actual = sizeof(struct sockaddr_un);
    socklen_t copy = *addrlen;
    if (copy > actual)
      copy = actual;
    __builtin_memset(addr, 0, copy);
    if (copy >= 2) {
      uint16_t family = AF_UNIX;
      __builtin_memcpy(addr, &family, 2);
    }
    *addrlen = actual;
    return 0;
  }

  HANDLE h = ofd->handle;
  auto *state = ofd->afd_socket();
  return state->ops->do_getpeername(h, state, addr, addrlen);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
