//===-- Socket data transfer engine (internal:: kernel functions) ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Engine versions of send / recv / sendmsg / recvmsg.
//
// Every function lives in namespace internal and returns ssize_t:
//   >= 0  byte count on success
//   < 0   -errno on failure (e.g. -EBADF, -EPIPE)
//
// No libc_errno access, no LLVM_LIBC_FUNCTION wrapper.
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/types/socklen_t.h"
#include "hdr/types/ssize_t.h"
#include "hdr/types/struct_msghdr.h"
#include "hdr/types/struct_sockaddr.h"
#include "include/llvm-libc-macros/sys-socket-macros.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "hdr/types/struct_sockaddr_un.h"
#include "src/__support/OSUtil/windows/ipc/afd_core.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ipc/fifo_channel.h"
#include "src/__support/OSUtil/windows/nt/nt_afd.h"
#include "src/__support/OSUtil/windows/ipc/sockpair_channel.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/macros/config.h"

// Forward declarations — these kernel functions are defined in
// __support/OSUtil/windows/io/read_write.cpp. Using them avoids
// re-entering the POSIX entry point layer.
namespace LIBC_NAMESPACE_DECL {
namespace internal {
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// ── helpers ─────────────────────────────────────────────────────────────

static void raise_sigpipe_if_needed() {
  signal_state::generate_standard_signal_for_current_thread(SIGPIPE);
}

static long reap_connect_for_io(internal::SocketState *state) {
  return internal::socket_reap_connect_if_needed(state);
}

static constexpr size_t MAX_AFD_ULONG = 0xFFFFFFFFu;
/// Manages an AFD_WSABUF array for iovec marshalling. Uses the per-socket
/// inline slots (SocketState::wsabuf_inline) for the common case (<=16
/// iovecs), falling back to page_alloc for exotic scatter/gather.
class AfdBufferArray {
public:
  explicit AfdBufferArray(internal::SocketState *state)
      : inline_(state->wsabuf_inline), buffers_(inline_) {}

  ~AfdBufferArray() {
    if (buffers_ != inline_)
      internal::page_free(buffers_);
  }

  long assign_iovecs(const struct iovec *iov, size_t iovcnt) {
    count_ = 0;
    if (iovcnt > 0 && iov == nullptr)
      return -EINVAL;
    if (iovcnt > MAX_AFD_ULONG)
      return -EINVAL;
    if (!ensure_capacity(iovcnt))
      return -ENOMEM;

    for (size_t i = 0; i < iovcnt; ++i) {
      if (iov[i].iov_len > MAX_AFD_ULONG)
        return -EINVAL;
      buffers_[i].Length = static_cast<ULONG>(iov[i].iov_len);
      buffers_[i].Buffer = iov[i].iov_base;
    }

    count_ = static_cast<ULONG>(iovcnt);
    return 0;
  }

  long assign_single(void *buffer, size_t length) {
    struct iovec iov = {buffer, length};
    return assign_iovecs(&iov, 1);
  }

  AFD_WSABUF *data() { return buffers_; }
  ULONG count() const { return count_; }

private:
  bool ensure_capacity(size_t iovcnt) {
    if (iovcnt <= internal::SocketState::WSABUF_INLINE_COUNT) {
      if (buffers_ != inline_)
        internal::page_free(buffers_);
      buffers_ = inline_;
      return true;
    }

    if (buffers_ != inline_)
      internal::page_free(buffers_);
    size_t bytes = iovcnt * sizeof(AFD_WSABUF);
    AFD_WSABUF *new_buffers =
        static_cast<AFD_WSABUF *>(internal::page_alloc(bytes));
    if (new_buffers == nullptr) {
      buffers_ = inline_;
      return false;
    }
    buffers_ = new_buffers;
    return true;
  }

  AFD_WSABUF *inline_;
  AFD_WSABUF *buffers_;
  ULONG count_ = 0;
};

static bool use_afd_message_path(const internal::SocketState *state,
                                 const struct msghdr *msg) {
  // Gated on the ops-table capability: only families whose AFD transport
  // honours IOCTL_AFD_SEND_MESSAGE / RECEIVE_MESSAGE may take this path.
  // AF_UNIX (afunix.sys) sets the bit false; AF_INET/INET6 set it true.
  return state->ops->supports_send_message &&
         (state->type == SOCK_DGRAM || msg->msg_control != nullptr ||
          (msg->msg_name != nullptr && msg->msg_namelen > 0));
}

static long afd_send_message(HANDLE h, internal::SocketState *state,
                             const struct msghdr *msg, int flags) {
  if (msg->msg_namelen > MAX_AFD_ULONG || msg->msg_controllen > MAX_AFD_ULONG)
    return -EINVAL;

  AfdBufferArray buffers(state);
  long marshal = buffers.assign_iovecs(msg->msg_iov, msg->msg_iovlen);
  if (marshal < 0)
    return marshal;

  ULONG tdi_flags = 0;
  if (flags & MSG_DONTWAIT)
    tdi_flags |= TDI_SEND_NON_BLOCKING;

  ULONG address_length = static_cast<ULONG>(msg->msg_namelen);
  ULONG *address_length_ptr =
      msg->msg_name != nullptr ? &address_length : nullptr;
  ULONG control_length = static_cast<ULONG>(msg->msg_controllen);
  ULONG *control_length_ptr =
      msg->msg_control != nullptr ? &control_length : nullptr;
  ULONG msg_flags = 0;

  AFD_MESSAGE_INFO message_info = {};
  message_info.dgi.BufferArray = buffers.data();
  message_info.dgi.BufferCount = buffers.count();
  message_info.dgi.AfdFlags = AFD_OVERLAPPED;
  message_info.dgi.TdiFlags = tdi_flags;
  message_info.dgi.Address = msg->msg_name;
  message_info.dgi.AddressLength = address_length_ptr;
  message_info.ControlBuffer = msg->msg_control;
  message_info.ControlLength = control_length_ptr;
  message_info.MsgFlags = &msg_flags;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = internal::afd_ioctl(h,IOCTL_AFD_SEND_MESSAGE,
                                   &message_info, sizeof(message_info), nullptr,
                                   0, &iosb);
  if (!NT_SUCCESS(s)) {
    int err = internal::ntstatus_to_errno_socket(s);
    if (err == EPIPE && !(flags & MSG_NOSIGNAL))
      raise_sigpipe_if_needed();
    return -err;
  }

  return static_cast<long>(iosb.Information);
}

static long afd_recv_message(internal::OpenFileDescription *ofd, HANDLE h,
                             internal::SocketState *state, struct msghdr *msg,
                             int flags) {
  if (msg->msg_controllen > MAX_AFD_ULONG)
    return -EINVAL;

  AfdBufferArray buffers(state);
  long marshal = buffers.assign_iovecs(msg->msg_iov, msg->msg_iovlen);
  if (marshal < 0)
    return marshal;

  ULONG tdi_flags = TDI_RECEIVE_NORMAL;
  if (flags & MSG_PEEK)
    tdi_flags |= TDI_RECEIVE_PEEK;
  if (flags & MSG_WAITALL)
    tdi_flags |= TDI_RECEIVE_ENTIRE_MESSAGE;

  // Stage the source address through a fixed MAX_SOCKADDR_STORAGE buffer so
  // AFD can never overrun a small caller-supplied msg_name; POSIX truncation
  // is applied on copy-out.
  uint8_t staging[internal::MAX_SOCKADDR_STORAGE];
  const bool want_name = msg->msg_name != nullptr && msg->msg_namelen > 0;
  ULONG address_length = static_cast<ULONG>(sizeof(staging));
  ULONG *address_length_ptr = want_name ? &address_length : nullptr;
  void *kernel_addr = want_name ? staging : nullptr;

  ULONG control_length = static_cast<ULONG>(msg->msg_controllen);
  ULONG *control_length_ptr =
      msg->msg_control != nullptr ? &control_length : nullptr;
  ULONG msg_flags = 0;

  AFD_MESSAGE_INFO message_info = {};
  message_info.dgi.BufferArray = buffers.data();
  message_info.dgi.BufferCount = buffers.count();
  message_info.dgi.AfdFlags = AFD_OVERLAPPED;
  message_info.dgi.TdiFlags = tdi_flags;
  message_info.dgi.Address = kernel_addr;
  message_info.dgi.AddressLength = address_length_ptr;
  message_info.ControlBuffer = msg->msg_control;
  message_info.ControlLength = control_length_ptr;
  message_info.MsgFlags = &msg_flags;

  bool nonblock = (flags & MSG_DONTWAIT) ||
      (ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE) & O_NONBLOCK);
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = nonblock
      ? internal::afd_ioctl_nonblock(h, IOCTL_AFD_RECEIVE_MESSAGE,
                                     &message_info, sizeof(message_info),
                                     nullptr, 0, &iosb)
      : internal::afd_ioctl(h, IOCTL_AFD_RECEIVE_MESSAGE,
                            &message_info, sizeof(message_info), nullptr, 0,
                            &iosb);
  if (!NT_SUCCESS(s))
    return -internal::ntstatus_to_errno_socket(s);

  if (want_name) {
    internal::copy_out_sockaddr(
        static_cast<struct sockaddr *>(msg->msg_name), &msg->msg_namelen,
        staging, static_cast<socklen_t>(address_length));
  } else if (msg->msg_name != nullptr) {
    msg->msg_namelen = 0;
  }
  msg->msg_controllen = static_cast<socklen_t>(control_length);
  msg->msg_flags = static_cast<int>(msg_flags);
  return static_cast<long>(iosb.Information);
}

// ── send ────────────────────────────────────────────────────────────────

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
  auto *ofd = internal::fd_table.get_ofd(sockfd);
  if (!ofd)
    return -EBADF;
  if (!ofd->is_socket())
    return -ENOTSOCK;

  // Socketpair path: write to the peer's read ring.
  if (ofd->kind == internal::FileKind::SocketPair) {
    auto *sp = ofd->socket_pair();
    uint8_t shut = sp->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
    if (shut & internal::SP_SHUT_WR) {
      raise_sigpipe_if_needed();
      return -EPIPE;
    }
    int open_flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
    if (flags & MSG_DONTWAIT)
      open_flags |= O_NONBLOCK;
    ssize_t r = internal::fifo_write(sp->write_ch, buf, len, open_flags);
    if (r >= 0)
      return r;
    if (r == -EPIPE)
      raise_sigpipe_if_needed();
    return r;
  }

  HANDLE h = ofd->handle;

  // AFD socket, no flags → delegate to write() for IoRing path.
  // MSG_DONTWAIT and/or MSG_NOSIGNAL also use write() (no TDI needed).
  auto *state = ofd->afd_socket();
  long connect_state = reap_connect_for_io(state);
  if (connect_state < 0)
    return connect_state;

  int non_tdi_flags = MSG_DONTWAIT | MSG_NOSIGNAL;
  if ((flags & ~non_tdi_flags) == 0) {
    if (flags & MSG_DONTWAIT) {
      int prev = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
      if (!(prev & O_NONBLOCK))
        ofd->status_flags.store(prev | O_NONBLOCK, cpp::MemoryOrder::RELEASE);
      ssize_t result = internal::write(sockfd, buf, len);
      if (!(prev & O_NONBLOCK))
        ofd->status_flags.store(prev, cpp::MemoryOrder::RELEASE);
      return result;
    }
    return internal::write(sockfd, buf, len);
  }

  // AFD socket with flags → IOCTL_AFD_SEND with TdiFlags.
  uint8_t shut = state->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
  if (shut & 2) { // SHUT_WR
    raise_sigpipe_if_needed();
    return -EPIPE;
  }

  // Map POSIX flags to TDI send flags.
  ULONG tdi_flags = 0;
  if (flags & MSG_DONTWAIT)
    tdi_flags |= TDI_SEND_NON_BLOCKING;
  // MSG_NOSIGNAL: handled at libc level, no TDI equivalent.

  AfdBufferArray buffers(state);
  long marshal = buffers.assign_single(const_cast<void *>(buf), len);
  if (marshal < 0)
    return marshal;

  AFD_SEND_INFO send_info = {};
  send_info.BufferArray = buffers.data();
  send_info.BufferCount = buffers.count();
  send_info.AfdFlags = AFD_OVERLAPPED;
  send_info.TdiFlags = tdi_flags;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = internal::afd_ioctl(h,IOCTL_AFD_SEND,
                                   &send_info, sizeof(send_info), nullptr, 0,
                                   &iosb);
  if (!NT_SUCCESS(s)) {
    int err = internal::ntstatus_to_errno_socket(s);
    if (err == EPIPE && !(flags & MSG_NOSIGNAL))
      raise_sigpipe_if_needed();
    return -err;
  }

  return static_cast<ssize_t>(iosb.Information);
}

// ── recv ────────────────────────────────────────────────────────────────

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
  auto *ofd = internal::fd_table.get_ofd(sockfd);
  if (!ofd)
    return -EBADF;
  if (!ofd->is_socket())
    return -ENOTSOCK;

  // Socketpair path: read from the local read ring.
  if (ofd->kind == internal::FileKind::SocketPair) {
    auto *sp = ofd->socket_pair();
    uint8_t shut = sp->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
    if (shut & internal::SP_SHUT_RD)
      return 0; // SHUT_RD → EOF

    int open_flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
    if (flags & MSG_DONTWAIT)
      open_flags |= O_NONBLOCK;

    // MSG_PEEK on socketpair: read without advancing position.
    if (flags & MSG_PEEK) {
      internal::FifoHeader *hdr = sp->read_ch->header();
      uint64_t rp = hdr->read_pos.load(cpp::MemoryOrder::ACQUIRE);
      uint64_t wp = hdr->write_pos.load(cpp::MemoryOrder::ACQUIRE);
      size_t avail = static_cast<size_t>(wp - rp);
      if (avail > internal::FIFO_RING_CAPACITY)
        avail = internal::FIFO_RING_CAPACITY; // Tamper guard.
      if (avail == 0) {
        if (open_flags & O_NONBLOCK)
          return -EAGAIN;
        // Blocking peek: fall through to fifo_read below, then
        // conceptually we'd need to not advance. For simplicity,
        // use the non-peek fifo_read and report back.
        // TODO: implement proper blocking peek with mutant hold.
      }
      if (avail > len)
        avail = len;
      if (avail > 0) {
        internal::ring_copy_out(buf, sp->read_ch->ring_data(), rp, avail);
        return static_cast<ssize_t>(avail);
      }
      return 0; // EOF
    }

    ssize_t r = internal::fifo_read(sp->read_ch, buf, len, open_flags);
    if (r >= 0)
      return r;
    return r;
  }

  HANDLE h = ofd->handle;

  // Check shutdown before any I/O.
  auto *state = ofd->afd_socket();
  long connect_state = reap_connect_for_io(state);
  if (connect_state < 0)
    return connect_state;
  uint8_t shut = state->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
  if (shut & 1) // SHUT_RD → EOF
    return 0;

  // Datagram: route through AFD_RECEIVE_MESSAGE so message boundaries and
  // msg_flags (MSG_TRUNC) reporting stay consistent with recvmsg(). IoRing
  // read on an AFD DGRAM handle silently truncates with no flag signal.
  if (state->type == SOCK_DGRAM) {
    struct iovec iov = {buf, len};
    struct msghdr m = {};
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    return afd_recv_message(ofd, h, state, &m, flags);
  }

  // AFD stream socket, no flags → delegate to read() for IoRing path.
  if (flags == 0)
    return internal::read(sockfd, buf, len);

  // AFD socket with flags → IOCTL_AFD_RECEIVE with TdiFlags.

  // Map POSIX flags to TDI receive flags.
  // Receive validation: (TdiFlags & 0x60) must be exactly one of 0x20 or 0x40.
  ULONG tdi_flags = TDI_RECEIVE_NORMAL;
  if (flags & MSG_PEEK)
    tdi_flags |= TDI_RECEIVE_PEEK;

  // MSG_WAITALL: userspace loop.
  if (flags & MSG_WAITALL) {
    size_t total = 0;
    auto *p = static_cast<uint8_t *>(buf);
    while (total < len) {
      AfdBufferArray buffers(state);
      long marshal = buffers.assign_single(p + total, len - total);
      if (marshal < 0) {
        if (total > 0)
          return static_cast<ssize_t>(total);
        return marshal;
      }

      AFD_RECV_INFO recv_info = {};
      recv_info.BufferArray = buffers.data();
      recv_info.BufferCount = buffers.count();
      recv_info.AfdFlags = AFD_OVERLAPPED;
      recv_info.TdiFlags = TDI_RECEIVE_NORMAL;

      IO_STATUS_BLOCK iosb = {};
      NTSTATUS s = internal::afd_ioctl(
          h, IOCTL_AFD_RECEIVE, &recv_info,
          sizeof(recv_info), nullptr, 0, &iosb);
      if (!NT_SUCCESS(s)) {
        if (total > 0)
          return static_cast<ssize_t>(total); // Partial read.
        return -internal::ntstatus_to_errno_socket(s);
      }
      size_t got = static_cast<size_t>(iosb.Information);
      if (got == 0)
        break; // EOF
      total += got;
    }
    return static_cast<ssize_t>(total);
  }

  bool nonblock = (flags & MSG_DONTWAIT) ||
      (ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE) & O_NONBLOCK);

  AfdBufferArray buffers(state);
  long marshal = buffers.assign_single(buf, len);
  if (marshal < 0)
    return marshal;

  AFD_RECV_INFO recv_info = {};
  recv_info.BufferArray = buffers.data();
  recv_info.BufferCount = buffers.count();
  recv_info.AfdFlags = AFD_OVERLAPPED;
  recv_info.TdiFlags = tdi_flags;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = nonblock
      ? internal::afd_ioctl_nonblock(h,IOCTL_AFD_RECEIVE,
                                     &recv_info, sizeof(recv_info), nullptr, 0,
                                     &iosb)
      : internal::afd_ioctl(h,IOCTL_AFD_RECEIVE,
                            &recv_info, sizeof(recv_info), nullptr, 0, &iosb);

  if (!NT_SUCCESS(s))
    return -internal::ntstatus_to_errno_socket(s);

  return static_cast<ssize_t>(iosb.Information);
}

// ── recvfrom ────────────────────────────────────────────────────────────
//
// Three paths:
//   * SocketPair:   unnamed AF_UNIX peer — synthesised from sa_family alone.
//   * AFD SOCK_DGRAM: per-datagram source via AFD_RECEIVE_MESSAGE.
//   * AFD SOCK_STREAM: delegate to recv() and copy the cached remote address.

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
  auto *ofd = internal::fd_table.get_ofd(sockfd);
  if (!ofd)
    return -EBADF;
  if (!ofd->is_socket())
    return -ENOTSOCK;

  // Socketpair: peer is unnamed AF_UNIX (just a family marker).
  if (ofd->kind == internal::FileKind::SocketPair) {
    ssize_t r = recv(sockfd, buf, len, flags);
    if (r >= 0 && src_addr && addrlen && *addrlen > 0) {
      uint8_t staging[sizeof(uint16_t)] = {};
      uint16_t family = AF_UNIX;
      __builtin_memcpy(staging, &family, sizeof(family));
      internal::copy_out_sockaddr(src_addr, addrlen, staging, sizeof(staging));
    }
    return r;
  }

  HANDLE h = ofd->handle;
  auto *state = ofd->afd_socket();

  // Datagram: bypass recv() so afd_recv_message can populate the source
  // address atomically with the payload.
  if (state->type == SOCK_DGRAM) {
    long connect_state = reap_connect_for_io(state);
    if (connect_state < 0)
      return connect_state;
    uint8_t shut = state->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
    if (shut & 1)
      return 0;

    struct iovec iov = {buf, len};
    struct msghdr m = {};
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    // Only request the source address if the caller reserved space for it.
    // afd_recv_message stages through MAX_SOCKADDR_STORAGE internally and
    // applies POSIX truncation before writing back to src_addr.
    if (src_addr && addrlen && *addrlen > 0) {
      m.msg_name = src_addr;
      m.msg_namelen = *addrlen;
    }
    ssize_t r = afd_recv_message(ofd, h, state, &m, flags);
    if (r >= 0 && src_addr && addrlen && *addrlen > 0)
      *addrlen = m.msg_namelen;
    return r;
  }

  // Stream: delegate to recv() and fill with cached remote address.
  ssize_t r = recv(sockfd, buf, len, flags);
  if (r >= 0 && src_addr && addrlen && *addrlen > 0)
    internal::copy_out_sockaddr(src_addr, addrlen, state->remote_addr,
                                state->remote_len);
  return r;
}

// Forward declaration — sendmsg is defined below sendto.
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);

// ── sendto ─────────────────────────────────────────────────────────────

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
  // No destination address → delegate to send().
  if (!dest_addr || addrlen == 0)
    return send(sockfd, buf, len, flags);

  // Build a msghdr and delegate to sendmsg for the message path.
  struct iovec iov;
  iov.iov_base = const_cast<void *>(buf);
  iov.iov_len = len;

  struct msghdr msg = {};
  msg.msg_name = const_cast<struct sockaddr *>(dest_addr);
  msg.msg_namelen = addrlen;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  return sendmsg(sockfd, &msg, flags);
}

// ── sendmsg ─────────────────────────────────────────────────────────────

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
  auto *ofd = internal::fd_table.get_ofd(sockfd);
  if (!ofd || !ofd->is_socket())
    return ofd ? -ENOTSOCK : -EBADF;

  // Socketpair: sequential writes per iovec.
  if (ofd->kind == internal::FileKind::SocketPair) {
    auto *sp = ofd->socket_pair();
    uint8_t shut = sp->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
    if (shut & internal::SP_SHUT_WR) {
      raise_sigpipe_if_needed();
      return -EPIPE;
    }
    int open_flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
    if (flags & MSG_DONTWAIT)
      open_flags |= O_NONBLOCK;

    size_t total = 0;
    for (size_t i = 0; i < static_cast<size_t>(msg->msg_iovlen); ++i) {
      if (msg->msg_iov[i].iov_len == 0)
        continue;
      ssize_t r = internal::fifo_write(sp->write_ch, msg->msg_iov[i].iov_base,
                                      msg->msg_iov[i].iov_len, open_flags);
      if (r < 0) {
        if (total > 0)
          return static_cast<ssize_t>(total);
        if (r == -EPIPE)
          raise_sigpipe_if_needed();
        return r;
      }
      total += static_cast<size_t>(r);
    }
    return static_cast<ssize_t>(total);
  }

  HANDLE h = ofd->handle;

  // AFD socket: scatter/gather via multi-entry BufferArray.
  auto *state = ofd->afd_socket();
  long connect_state = reap_connect_for_io(state);
  if (connect_state < 0)
    return connect_state;
  uint8_t shut = state->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
  if (shut & 2) {
    raise_sigpipe_if_needed();
    return -EPIPE;
  }

  // SCM_RIGHTS (future): if msg->msg_control contains SCM_RIGHTS cmsg,
  // extract fds, resolve to HANDLEs via fd_table, get peer PID via
  // AFD_TRANSPORT_IOCTL(0x58000100), NtOpenProcess(PROCESS_DUP_HANDLE),
  // NtDuplicateObject each handle into the peer, then frame the
  // duplicated HANDLE values as in-band metadata alongside the payload.
  // All NT primitives are proven — needs framing protocol design.

  if (use_afd_message_path(state, msg))
    return afd_send_message(h, state, msg, flags);

  AfdBufferArray buffers(state);
  long marshal = buffers.assign_iovecs(msg->msg_iov, msg->msg_iovlen);
  if (marshal < 0)
    return marshal;

  ULONG tdi_flags = 0;
  if (flags & MSG_DONTWAIT)
    tdi_flags |= TDI_SEND_NON_BLOCKING;

  AFD_SEND_INFO send_info = {};
  send_info.BufferArray = buffers.data();
  send_info.BufferCount = buffers.count();
  send_info.AfdFlags = AFD_OVERLAPPED;
  send_info.TdiFlags = tdi_flags;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = internal::afd_ioctl(h,IOCTL_AFD_SEND,
                                   &send_info, sizeof(send_info), nullptr, 0,
                                   &iosb);
  if (!NT_SUCCESS(s)) {
    int err = internal::ntstatus_to_errno_socket(s);
    if (err == EPIPE && !(flags & MSG_NOSIGNAL))
      raise_sigpipe_if_needed();
    return -err;
  }

  return static_cast<ssize_t>(iosb.Information);
}

// ── recvmsg ─────────────────────────────────────────────────────────────

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
  auto *ofd = internal::fd_table.get_ofd(sockfd);
  if (!ofd || !ofd->is_socket())
    return ofd ? -ENOTSOCK : -EBADF;

  // Socketpair: sequential reads per iovec.
  if (ofd->kind == internal::FileKind::SocketPair) {
    auto *sp = ofd->socket_pair();
    uint8_t shut = sp->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
    if (shut & internal::SP_SHUT_RD)
      return 0;

    int open_flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
    if (flags & MSG_DONTWAIT)
      open_flags |= O_NONBLOCK;

    size_t total = 0;
    for (size_t i = 0; i < static_cast<size_t>(msg->msg_iovlen); ++i) {
      if (msg->msg_iov[i].iov_len == 0)
        continue;
      ssize_t r = internal::fifo_read(sp->read_ch, msg->msg_iov[i].iov_base,
                                     msg->msg_iov[i].iov_len, open_flags);
      if (r < 0) {
        if (total > 0)
          return static_cast<ssize_t>(total);
        return r;
      }
      if (r == 0)
        break; // EOF
      total += static_cast<size_t>(r);
    }
    msg->msg_controllen = 0; // No ancillary data on socketpair.
    msg->msg_flags = 0;
    return static_cast<ssize_t>(total);
  }

  HANDLE h = ofd->handle;

  // AFD socket: gather receive via multi-entry BufferArray.
  auto *state = ofd->afd_socket();
  long connect_state = reap_connect_for_io(state);
  if (connect_state < 0)
    return connect_state;
  uint8_t shut = state->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
  if (shut & 1)
    return 0; // SHUT_RD → EOF

  if (use_afd_message_path(state, msg))
    return afd_recv_message(ofd, h, state, msg, flags);

  AfdBufferArray buffers(state);
  long marshal = buffers.assign_iovecs(msg->msg_iov, msg->msg_iovlen);
  if (marshal < 0)
    return marshal;

  // TdiFlags: NORMAL required, add PEEK if requested.
  ULONG tdi_flags = TDI_RECEIVE_NORMAL;
  if (flags & MSG_PEEK)
    tdi_flags |= TDI_RECEIVE_PEEK;

  AFD_RECV_INFO recv_info = {};
  recv_info.BufferArray = buffers.data();
  recv_info.BufferCount = buffers.count();
  recv_info.AfdFlags = AFD_OVERLAPPED;
  recv_info.TdiFlags = tdi_flags;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = internal::afd_ioctl(h,IOCTL_AFD_RECEIVE,
                                   &recv_info, sizeof(recv_info), nullptr, 0,
                                   &iosb);
  if (!NT_SUCCESS(s))
    return -internal::ntstatus_to_errno_socket(s);

  ssize_t received = static_cast<ssize_t>(iosb.Information);

  // Fill source address if requested (connected stream peer).
  if (msg->msg_name && msg->msg_namelen > 0)
    internal::copy_out_sockaddr(static_cast<struct sockaddr *>(msg->msg_name),
                                &msg->msg_namelen, state->remote_addr,
                                state->remote_len);

  // SCM_RIGHTS (future): detect in-band framed HANDLE values from the
  // sender's NtDuplicateObject side-channel. For each received HANDLE:
  // fd_table.alloc(handle, O_RDWR) → fd, then build SCM_RIGHTS cmsg
  // in msg_control with the allocated fds. No kernel cmsg transport
  // exists on AF_UNIX (0x1AFD endpoint gate blocks AFD_SEND_MESSAGE/
  // AFD_RECEIVE_MESSAGE). All NT primitives are proven — needs framing
  // protocol design to distinguish data from metadata in the byte stream.
  msg->msg_controllen = 0;
  msg->msg_flags = 0;

  return received;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
