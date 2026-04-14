//===-- Socket lifecycle engine (generic orchestrator) ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Generic socket lifecycle state machine. Delegates all address-family-specific
// logic to the ops table in SocketState::ops. All transient buffers live in
// the per-socket scratch region or in per-call page_alloc — no stack arrays.
//
// socket()     resolve ops, open AFD endpoint, allocate state
// bind()       phase check → ops->do_bind → phase transition
// listen()     phase check → IOCTL_AFD_START_LISTEN → ops->post_listen
// connect()    phase check → ops->do_connect → phase transition
// accept()     WAIT_FOR_LISTEN → ops->setup_accepted → fd alloc
// shutdown()   generic (AF-agnostic)
// socketpair() unchanged (ring buffer pair, not AFD)
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/types/socklen_t.h"
#include "hdr/types/struct_sockaddr.h"
#include "include/llvm-libc-macros/sys-socket-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ipc/afd_core.h"
#include "src/__support/OSUtil/windows/ipc/af_ops.h"
#include "src/__support/OSUtil/windows/ipc/fifo_channel.h"
#include "src/__support/OSUtil/windows/ipc/sockpair_channel.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_afd.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

//===----------------------------------------------------------------------===//
// Generic Work structs for AF-agnostic operations (overlaid on scratch)
//===----------------------------------------------------------------------===//

/// Scratch layout for the nonblocking mode setup in socket().
struct SocketInitWork {
  AFD_INFORMATION info;
  IO_STATUS_BLOCK iosb;
};

/// Scratch layout for IOCTL_AFD_START_LISTEN in listen().
struct GenericListenWork {
  AFD_LISTEN_INFO listen_info;
  IO_STATUS_BLOCK iosb;
};

/// Scratch layout for IOCTL_AFD_PARTIAL_DISCONNECT in shutdown().
struct GenericShutdownWork {
  AFD_PARTIAL_DISCONNECT_INFO disc;
  IO_STATUS_BLOCK iosb;
};

/// Scratch layout for the WAIT_FOR_LISTEN output in accept().
struct GenericAcceptWflWork {
  alignas(8) uint8_t wfl_buf[4 + MAX_SOCKADDR_STORAGE];
  IO_STATUS_BLOCK iosb;
};

// ---------------------------------------------------------------------------
// socket
// ---------------------------------------------------------------------------
intptr_t socket(int domain, int type, int protocol) {
  int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

  // Resolve address family ops.
  const AddressFamilyOps *ops = resolve_af_ops(domain);
  if (!ops)
    return -EAFNOSUPPORT;

  // AF-specific type/protocol validation.
  int err = ops->validate_create(base_type, protocol);
  if (err)
    return err;

  // Open AFD endpoint.
  HANDLE h = nullptr;
  NTSTATUS s = afd_open_endpoint(&h, domain, base_type, protocol);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);
  auto close_handle = cpp::make_scope_guard([&] { NtClose(h); });

  // Allocate 2-page state arena.
  auto *state = socket_state_alloc();
  if (!state)
    return -ENOMEM;
  auto free_state =
      cpp::make_scope_guard([&] { socket_state_free(state); });

  state->ops = ops;
  state->domain = domain;
  state->type = base_type;
  state->protocol = protocol;
  state->phase.store(SocketPhase::UNBOUND, cpp::MemoryOrder::RELAXED);

  // Create per-socket ioctl event.
  auto evt_oa = windows::internal_oa();
  NtCreateEvent(&state->ioctl_event, EVENT_MODIFY_STATE | SYNCHRONIZE, &evt_oa,
                SynchronizationEvent, FALSE);

  // Set non-blocking at AFD level — uses scratch for ioctl structs.
  if (state->ioctl_event) {
    auto *w = reinterpret_cast<SocketInitWork *>(socket_scratch(state));
    __builtin_memset(w, 0, sizeof(SocketInitWork));
    w->info.InformationType = AFD_NONBLOCKING_MODE;
    w->info.Information.Boolean = TRUE;
    afd_ioctl(h, state->ioctl_event, IOCTL_AFD_SET_INFORMATION, &w->info,
              sizeof(w->info), nullptr, 0, &w->iosb);
  }

  // Allocate fd.
  int open_flags = O_RDWR;
  if (flags & SOCK_NONBLOCK)
    open_flags |= O_NONBLOCK;

  auto result = fd_table.alloc(h, open_flags, 0, FileKind::AfdSocket);
  if (!result)
    return -EMFILE;

  // All resources committed — dismiss guards.
  free_state.dismiss();
  close_handle.dismiss();

  OpenFileDescription *ofd = fd_table.get_ofd(result.value());
  ofd->set_afd_socket(state);

  if (flags & SOCK_CLOEXEC)
    fd_table.set_fd_cloexec(result.value(), true);

  return static_cast<intptr_t>(result.value());
}

// ---------------------------------------------------------------------------
// bind
// ---------------------------------------------------------------------------
intptr_t bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  auto *ofd = fd_table.get_ofd(sockfd);
  if (!ofd || !ofd->is_socket())
    return ofd ? -ENOTSOCK : -EBADF;

  auto *state = ofd->afd_socket();

  // Must be UNBOUND.
  if (state->phase.load(cpp::MemoryOrder::ACQUIRE) != SocketPhase::UNBOUND)
    return -EINVAL;

  // Delegate to AF-specific bind.
  intptr_t result = state->ops->do_bind(ofd->handle, state, addr, addrlen);
  if (result < 0)
    return result;

  state->phase.store(SocketPhase::BOUND, cpp::MemoryOrder::RELEASE);
  return 0;
}

// ---------------------------------------------------------------------------
// listen
// ---------------------------------------------------------------------------
intptr_t listen(int sockfd, int backlog) {
  auto *ofd = fd_table.get_ofd(sockfd);
  if (!ofd || !ofd->is_socket())
    return ofd ? -ENOTSOCK : -EBADF;

  auto *state = ofd->afd_socket();
  HANDLE h = ofd->handle;

  if (state->phase.load(cpp::MemoryOrder::ACQUIRE) != SocketPhase::BOUND)
    return -EINVAL;

  if (backlog < 1)
    backlog = 1;
  if (backlog > 128)
    backlog = 128;

  // IOCTL_AFD_START_LISTEN — generic, same for all AFs.
  auto *w = reinterpret_cast<GenericListenWork *>(socket_scratch(state));
  __builtin_memset(w, 0, sizeof(GenericListenWork));
  w->listen_info.MaximumConnectionQueue = static_cast<ULONG>(backlog);

  NTSTATUS s = afd_ioctl(h, state->ioctl_event, IOCTL_AFD_START_LISTEN,
                         &w->listen_info, sizeof(w->listen_info), nullptr, 0,
                         &w->iosb);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);

  // AF-specific post-listen (e.g., SET_CONTEXT for AF_UNIX).
  intptr_t result = state->ops->post_listen(h, state);
  if (result < 0)
    return result;

  state->phase.store(SocketPhase::LISTENING, cpp::MemoryOrder::RELEASE);
  return 0;
}

// ---------------------------------------------------------------------------
// connect
// ---------------------------------------------------------------------------
intptr_t connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  auto *ofd = fd_table.get_ofd(sockfd);
  if (!ofd || !ofd->is_socket())
    return ofd ? -ENOTSOCK : -EBADF;

  auto *state = ofd->afd_socket();
  HANDLE h = ofd->handle;

  auto phase = state->phase.load(cpp::MemoryOrder::ACQUIRE);
  if (phase == SocketPhase::CONNECTING) {
    NTSTATUS pending = socket_probe_connect_completion(state);
    if (pending == STATUS_PENDING)
      return -EALREADY;
    phase = state->phase.load(cpp::MemoryOrder::ACQUIRE);
  }
  if (phase == SocketPhase::CONNECTED || phase == SocketPhase::LISTENING)
    return -EISCONN;

  bool nonblock =
      ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE) & O_NONBLOCK;

  // Delegate to AF-specific connect.
  return state->ops->do_connect(h, state, nonblock, addr, addrlen);
}

// ---------------------------------------------------------------------------
// accept
// ---------------------------------------------------------------------------
intptr_t accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
  auto *ofd = fd_table.get_ofd(sockfd);
  if (!ofd || !ofd->is_socket())
    return ofd ? -ENOTSOCK : -EBADF;

  auto *state = ofd->afd_socket();
  HANDLE listener = ofd->handle;

  if (state->phase.load(cpp::MemoryOrder::ACQUIRE) != SocketPhase::LISTENING)
    return -EINVAL;

  bool nonblock =
      ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE) & O_NONBLOCK;

  // WAIT_FOR_LISTEN — generic, uses listener's scratch.
  auto *w = reinterpret_cast<GenericAcceptWflWork *>(socket_scratch(state));
  __builtin_memset(w, 0, sizeof(GenericAcceptWflWork));

  NTSTATUS s = ::NtDeviceIoControlFile(
      listener, state->ioctl_event, nullptr, nullptr, &w->iosb,
      IOCTL_AFD_WAIT_FOR_LISTEN, nullptr, 0, w->wfl_buf, sizeof(w->wfl_buf));

  if (s == STATUS_PENDING) {
    if (nonblock) {
      IO_STATUS_BLOCK cancel_iosb = {};
      ::NtCancelIoFileEx(listener, &w->iosb, &cancel_iosb);
      ::NtWaitForSingleObject(state->ioctl_event, 0, nullptr);
      return -EAGAIN;
    }
    s = ::NtWaitForSingleObject(state->ioctl_event, /*Alertable=*/TRUE,
                                nullptr);
    if (s == STATUS_USER_APC || s == STATUS_ALERTED) {
      IO_STATUS_BLOCK cancel_iosb = {};
      ::NtCancelIoFileEx(listener, &w->iosb, &cancel_iosb);
      ::NtWaitForSingleObject(state->ioctl_event, 0, nullptr);
      return -EINTR;
    }
    if (NT_SUCCESS(s))
      s = w->iosb.Status;
  }

  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);

  // Extract WFL output: sequence (4 bytes) + remote address.
  int32_t sequence;
  __builtin_memcpy(&sequence, w->wfl_buf, 4);
  const uint8_t *wfl_remote = w->wfl_buf + 4;
  socklen_t wfl_remote_len =
      static_cast<socklen_t>(w->iosb.Information > 4 ? w->iosb.Information - 4
                                                     : 0);
  if (wfl_remote_len > MAX_SOCKADDR_STORAGE)
    wfl_remote_len = MAX_SOCKADDR_STORAGE;

  // Allocate state for accepted socket.
  auto *acc_state = socket_state_alloc();
  if (!acc_state)
    return -ENOMEM;
  auto free_acc_state =
      cpp::make_scope_guard([&] { socket_state_free(acc_state); });

  // Create ioctl event for accepted socket.
  auto aevt_oa = windows::internal_oa();
  NtCreateEvent(&acc_state->ioctl_event, EVENT_MODIFY_STATE | SYNCHRONIZE,
                &aevt_oa,
                SynchronizationEvent, FALSE);

  // AF-specific accept setup.
  HANDLE acc_handle = nullptr;
  intptr_t result = state->ops->setup_accepted(
      listener, state, sequence, wfl_remote, wfl_remote_len, &acc_handle,
      acc_state, addr, addrlen);
  if (result < 0)
    return result;

  auto close_acc = cpp::make_scope_guard([&] { NtClose(acc_handle); });

  // Allocate fd for accepted socket.
  auto fd_result = fd_table.alloc(acc_handle, O_RDWR, 0, FileKind::AfdSocket);
  if (!fd_result)
    return -EMFILE;

  // All committed.
  free_acc_state.dismiss();
  close_acc.dismiss();

  OpenFileDescription *acc_ofd = fd_table.get_ofd(fd_result.value());
  acc_ofd->set_afd_socket(acc_state);

  return static_cast<intptr_t>(fd_result.value());
}

// ---------------------------------------------------------------------------
// shutdown — entirely generic, AF-agnostic
// ---------------------------------------------------------------------------
intptr_t shutdown(int sockfd, int how) {
  if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
    return -EINVAL;

  auto *ofd = fd_table.get_ofd(sockfd);
  if (!ofd || !ofd->is_socket())
    return ofd ? -ENOTSOCK : -EBADF;

  // Socketpair path.
  if (ofd->kind == FileKind::SocketPair) {
    auto *sp = ofd->socket_pair();
    sp_shutdown(sp, how);
    return 0;
  }

  HANDLE h = ofd->handle;
  auto *state = ofd->afd_socket();
  long connect_state =
      socket_reap_connect_if_needed(state, /*pending_errno=*/ENOTCONN);
  if (connect_state < 0)
    return connect_state;

  // Map POSIX how to AFD disconnect type.
  ULONG afd_type = 0;
  uint8_t shut_bits = 0;
  switch (how) {
  case SHUT_RD:
    afd_type = 2;
    shut_bits = 1;
    break;
  case SHUT_WR:
    afd_type = 1;
    shut_bits = 2;
    break;
  case SHUT_RDWR:
    afd_type = 3;
    shut_bits = 3;
    break;
  }

  auto *w = reinterpret_cast<GenericShutdownWork *>(socket_scratch(state));
  __builtin_memset(w, 0, sizeof(GenericShutdownWork));
  w->disc.DisconnectMode = afd_type;
  w->disc.Timeout.QuadPart = -1;

  NTSTATUS s = afd_ioctl(h, state->ioctl_event, IOCTL_AFD_PARTIAL_DISCONNECT,
                         &w->disc, sizeof(w->disc), nullptr, 0, &w->iosb);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);

  state->shutdown_flags.fetch_or(shut_bits, cpp::MemoryOrder::ACQ_REL);
  return 0;
}

// ---------------------------------------------------------------------------
// socketpair — unchanged (ring buffer pair, not AFD)
// ---------------------------------------------------------------------------
intptr_t socketpair(int domain, int type, int protocol, int sv[2]) {
  int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

  if (domain != AF_UNIX)
    return -EAFNOSUPPORT;
  if (base_type != SOCK_STREAM)
    return -EPROTOTYPE;
  if (protocol != 0)
    return -EPROTONOSUPPORT;

  SocketPairChannel *ep_a = nullptr;
  SocketPairChannel *ep_b = nullptr;
  if (!sp_create_pair(&ep_a, &ep_b, domain, base_type))
    return -ENOMEM;

  int open_flags = O_RDWR;
  if (flags & SOCK_NONBLOCK)
    open_flags |= O_NONBLOCK;

  int fd_flags = 0;
  if (flags & SOCK_CLOEXEC)
    fd_flags = FD_CLOEXEC;

  auto result_a = fd_table.alloc(nullptr, open_flags, 0, FileKind::SocketPair);
  if (!result_a) {
    sp_close_channel(ep_a);
    sp_close_channel(ep_b);
    return -EMFILE;
  }

  auto result_b = fd_table.alloc(nullptr, open_flags, 0, FileKind::SocketPair);
  if (!result_b) {
    fd_table.release(result_a.value());
    sp_close_channel(ep_b);
    return -EMFILE;
  }

  OpenFileDescription *ofd_a = fd_table.get_ofd(result_a.value());
  OpenFileDescription *ofd_b = fd_table.get_ofd(result_b.value());

  ofd_a->set_socket_pair(ep_a);
  ofd_b->set_socket_pair(ep_b);

  if (fd_flags) {
    fd_table.set_fd_cloexec(result_a.value(), true);
    fd_table.set_fd_cloexec(result_b.value(), true);
  }

  sv[0] = result_a.value();
  sv[1] = result_b.value();
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
