//===-- Generic AFD socket helpers --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Address-family-agnostic AFD helpers. These are DRY wrappers for the
// completion pattern, endpoint creation, NTSTATUS→errno translation, and
// connect-completion reaping. No AF_UNIX-specific code lives here.
//
// AF-specific code (transport prime, SET/GET_CONTEXT, context image
// extraction) lives in af_unix_ops.h / af_unix_ops.cpp.
//
// All blocking/nonblocking AFD ioctls route IRP completions through the
// reactor IOCP; the caller parks on its own ThreadLocalWord. There is no
// per-socket NT Event. See afd_io_completion.h for the completion model.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AFD_CORE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AFD_CORE_H

#include "hdr/errno_macros.h"
#include "hdr/types/socklen_t.h"
#include "hdr/types/struct_sockaddr.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ipc/afd_io_completion.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_afd.h"
#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

//===----------------------------------------------------------------------===//
// NTSTATUS → errno for AFD socket operations
//===----------------------------------------------------------------------===//

LIBC_INLINE int ntstatus_to_errno_socket(NTSTATUS s) {
  switch (s) {
  case STATUS_SUCCESS:
    return 0;
  case STATUS_CONNECTION_RESET:
  case STATUS_CONNECTION_DISCONNECTED:
    return ECONNRESET;
  case STATUS_CONNECTION_REFUSED:
    return ECONNREFUSED;
  case STATUS_CONNECTION_ABORTED:
    return ECONNABORTED;
  case STATUS_PIPE_DISCONNECTED:
  case STATUS_PIPE_CLOSING:
  case STATUS_PIPE_BROKEN:
  case STATUS_LOCAL_DISCONNECT:
    return EPIPE;
  case STATUS_REMOTE_DISCONNECT:
  case STATUS_GRACEFUL_DISCONNECT:
    return ECONNRESET;
  case STATUS_ADDRESS_ALREADY_EXISTS:
    return EADDRINUSE;
  case STATUS_ADDRESS_CLOSED:
    return EADDRNOTAVAIL;
  case STATUS_DEVICE_NOT_READY:
    return EAGAIN;
  case STATUS_IO_TIMEOUT:
    return ETIMEDOUT;
  case STATUS_CANCELLED:
  case STATUS_ALERTED:
  case STATUS_USER_APC:
    return EINTR;
  case STATUS_NETWORK_UNREACHABLE:
    return ENETUNREACH;
  case STATUS_HOST_UNREACHABLE:
    return EHOSTUNREACH;
  case STATUS_PORT_UNREACHABLE:
    return ECONNREFUSED;
  case STATUS_CONNECTION_INVALID:
    return ENOTCONN;
  case STATUS_NOT_SUPPORTED:
    return EOPNOTSUPP;
  case STATUS_INVALID_PARAMETER:
    return EINVAL;
  case STATUS_SHARING_VIOLATION:
    return EADDRINUSE;
  case STATUS_ACCESS_DENIED:
    return EACCES;
  case STATUS_OBJECT_NAME_NOT_FOUND:
  case STATUS_OBJECT_PATH_NOT_FOUND:
    return ENOENT;
  case STATUS_INSUFFICIENT_RESOURCES:
  case STATUS_NO_MEMORY:
    return ENOMEM;
  default:
    return EIO;
  }
}

//===----------------------------------------------------------------------===//
// Synchronous / non-blocking AFD ioctls
//
// Both thin wrappers over afd_io:: primitives. The socket HANDLE must be
// bound to the reactor IOCP via afd_io::bind_handle() at socket() /
// accept() creation time.
//===----------------------------------------------------------------------===//

/// Issue an AFD ioctl and block until completion. Returns the final NTSTATUS.
LIBC_INLINE NTSTATUS afd_ioctl(HANDLE socket, ULONG ioctl_code, void *in_buf,
                               ULONG in_len, void *out_buf, ULONG out_len,
                               IO_STATUS_BLOCK *iosb) {
  return afd_io::ioctl_blocking(socket, ioctl_code, in_buf, in_len, out_buf,
                                out_len, iosb);
}

/// Non-blocking variant: returns STATUS_DEVICE_NOT_READY (→ EAGAIN) instead
/// of waiting when the operation would block.
LIBC_INLINE NTSTATUS afd_ioctl_nonblock(HANDLE socket, ULONG ioctl_code,
                                        void *in_buf, ULONG in_len,
                                        void *out_buf, ULONG out_len,
                                        IO_STATUS_BLOCK *iosb) {
  return afd_io::ioctl_nonblock(socket, ioctl_code, in_buf, in_len, out_buf,
                                out_len, iosb);
}

//===----------------------------------------------------------------------===//
// AFD endpoint creation
//===----------------------------------------------------------------------===//

/// Access mask for AFD endpoints: GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE.
inline constexpr ULONG AFD_ENDPOINT_ACCESS = 0xC0140000;

/// Open an AFD endpoint and bind it to the reactor IOCP for socket-io
/// completion dispatch. EndpointFlags: 0 for server/normal,
/// AFD_OPEN_FLAG_ACCEPT_TARGET for accept targets, AFD_OPEN_FLAG_RIO for RIO.
///
/// On success, *out holds a handle whose IRPs will post completions to
/// the reactor IOCP tagged with afd_io::socket_io_sentinel(). Callers
/// must use afd_ioctl / afd_ioctl_nonblock (or the afd_io:: async API
/// for persistent IRPs) — any raw NtDeviceIoControlFile call must supply
/// an IoWaiter * as ApcContext.
LIBC_INLINE NTSTATUS afd_open_endpoint(HANDLE *out, int domain, int type,
                                        int protocol,
                                        ULONG extra_endpoint_flags = 0) {
  AFD_OPEN_PACKET_FULL_EA ea = {};
  ea.NextEntryOffset = 0;
  ea.Flags = 0;
  ea.EaNameLength = sizeof(AfdOpenPacket) - 1;
  ea.EaValueLength = sizeof(AFD_OPEN_PACKET);
  __builtin_memcpy(ea.EaName, AfdOpenPacket, sizeof(AfdOpenPacket));
  ea.OpenPacket.__f.EndpointFlags = extra_endpoint_flags;
  ea.OpenPacket.GroupID = 0;
  ea.OpenPacket.AddressFamily = domain;
  ea.OpenPacket.SocketType = type;
  ea.OpenPacket.Protocol = protocol;
  ea.OpenPacket.TransportDeviceNameLength = 0;

  static const WCHAR path[] = u"\\Device\\Afd\\Endpoint";
  windows::nt_wstring_view device_name(path);

  auto oa = windows::named_internal_oa(&device_name);

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = ::NtCreateFile(out, AFD_ENDPOINT_ACCESS, &oa, &iosb, nullptr, 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                              0, &ea, sizeof(ea));
  if (!NT_SUCCESS(s))
    return s;

  // Bind to reactor IOCP so every subsequent IRP's completion routes to
  // the socket-io dispatcher. Failure here leaks the endpoint handle to
  // the caller's cleanup (ScopedNtHandle / explicit NtClose).
  NTSTATUS bind = afd_io::bind_handle(*out);
  if (!NT_SUCCESS(bind)) {
    ::NtClose(*out);
    *out = nullptr;
    return bind;
  }
  return s;
}

//===----------------------------------------------------------------------===//
// Generic transport ioctl wrapper
//===----------------------------------------------------------------------===//

LIBC_INLINE NTSTATUS afd_transport_ioctl(HANDLE socket,
                                         TL_IO_CONTROL_TYPE type,
                                         ULONG ioctl_code, void *input_buffer,
                                         ULONG input_length,
                                         void *output_buffer,
                                         ULONG output_length,
                                         IO_STATUS_BLOCK *iosb, ULONG level = 0,
                                         bool endpoint_ioctl = true) {
  AFD_TL_IO_CONTROL_INFO tl = {};
  tl.Type = type;
  tl.Level = level;
  tl.IoControlCode = ioctl_code;
  tl.EndpointIoctl = endpoint_ioctl ? 1 : 0;
  tl.InputBuffer = input_buffer;
  tl.InputBufferLength = input_length;
  return afd_ioctl(socket, IOCTL_AFD_TRANSPORT_IOCTL, &tl, sizeof(tl),
                   output_buffer, output_length, iosb);
}

//===----------------------------------------------------------------------===//
// Connect completion helpers
//===----------------------------------------------------------------------===//

/// Finalize a connect attempt. Called after the connect ioctl completes
/// (successfully or not). Updates socket_error and phase.
///
/// On success, transitions to CONNECTED. On failure, transitions back
/// to BOUND and stores the error for SO_ERROR.
///
/// The AF-specific post_connect context update (e.g., AF_UNIX SET_CONTEXT)
/// is handled by the caller (the AF ops do_connect function).
LIBC_INLINE NTSTATUS socket_finalize_connect(SocketState *state,
                                             NTSTATUS status) {
  if (NT_SUCCESS(status)) {
    state->socket_error.store(0, cpp::MemoryOrder::RELEASE);
    state->phase.store(SocketPhase::CONNECTED, cpp::MemoryOrder::RELEASE);
    return status;
  }

  state->socket_error.store(ntstatus_to_errno_socket(status),
                            cpp::MemoryOrder::RELEASE);
  state->phase.store(SocketPhase::BOUND, cpp::MemoryOrder::RELEASE);
  return status;
}

/// Non-blocking probe: check if a pending connect has completed without
/// blocking. Returns STATUS_SUCCESS if connected, STATUS_PENDING if still
/// in progress, or the connect error status.
LIBC_INLINE NTSTATUS socket_probe_connect_completion(SocketState *state) {
  if (state->phase.load(cpp::MemoryOrder::ACQUIRE) != SocketPhase::CONNECTING)
    return STATUS_SUCCESS;

  NTSTATUS s = afd_io::poll_waiter(&state->connect_waiter, &state->connect_iosb);
  if (s == STATUS_PENDING)
    return STATUS_PENDING;
  return socket_finalize_connect(state, s);
}

/// Reap a pending connect if needed, returning 0 on success or -errno.
/// Used by the data plane (send/recv) and query functions before proceeding.
LIBC_INLINE long socket_reap_connect_if_needed(SocketState *state,
                                               int pending_errno = EAGAIN) {
  NTSTATUS status = socket_probe_connect_completion(state);
  if (status == STATUS_PENDING)
    return -pending_errno;
  if (!NT_SUCCESS(status)) {
    int err = state->socket_error.load(cpp::MemoryOrder::ACQUIRE);
    if (err == 0)
      err = ntstatus_to_errno_socket(status);
    return -err;
  }
  return 0;
}

/// Atomically take and clear the sticky socket error (for SO_ERROR).
LIBC_INLINE int socket_take_error(SocketState *state) {
  return state->socket_error.exchange(0, cpp::MemoryOrder::ACQ_REL);
}

//===----------------------------------------------------------------------===//
// AF-agnostic sockaddr output helper
//
// POSIX semantics: copy at most *dstlen bytes into dst, then store the
// kernel-reported srclen into *dstlen regardless of truncation. Callers
// inspecting the returned length can detect truncation by comparing against
// their original buffer size.
//===----------------------------------------------------------------------===//

LIBC_INLINE void copy_out_sockaddr(struct sockaddr *dst, socklen_t *dstlen,
                                   const uint8_t *src, socklen_t srclen) {
  if (!dst || !dstlen)
    return;
  socklen_t copy = *dstlen;
  if (copy > srclen)
    copy = srclen;
  if (copy > 0)
    __builtin_memcpy(dst, src, copy);
  *dstlen = srclen;
}

//===----------------------------------------------------------------------===//
// Shared AFD bind / connect submission
//
// Both helpers assume payload_buf has a leading header (AFD_BIND_INFO_TL or
// AFD_CONNECT_JOIN_INFO_TL) immediately followed by raw sockaddr storage.
// The caller owns the allocation (typically the per-socket scratch region).
//===----------------------------------------------------------------------===//

/// Issue IOCTL_AFD_BIND synchronously. payload_buf must be at least
/// sizeof(AFD_BIND_INFO_TL) + addrlen bytes; this function rebuilds the
/// header (reading SO_REUSEADDR from state->socket_options), copies the
/// caller's sockaddr into place, and submits. On success the kernel
/// writes the actual bound sockaddr into the tail of payload_buf
/// (useful for AF_INET wildcard-port resolution).
LIBC_INLINE NTSTATUS afd_submit_bind(HANDLE sock, SocketState *state,
                                     uint8_t *payload_buf,
                                     size_t payload_capacity,
                                     const void *addr, size_t addrlen,
                                     IO_STATUS_BLOCK *iosb) {
  size_t need = sizeof(AFD_BIND_INFO_TL) + addrlen;
  if (payload_capacity < need)
    return STATUS_BUFFER_TOO_SMALL;

  auto *hdr = reinterpret_cast<AFD_BIND_INFO_TL *>(payload_buf);
  uint32_t opts = state->socket_options.load(cpp::MemoryOrder::ACQUIRE);
  hdr->ShareAccess = (opts & SocketState::OPT_REUSEADDR)
                         ? AfdBindReuseAddress
                         : AfdBindNormalAddressUse;
  __builtin_memcpy(payload_buf + sizeof(AFD_BIND_INFO_TL), addr, addrlen);

  __builtin_memset(iosb, 0, sizeof(IO_STATUS_BLOCK));
  // Output buffer overlaps the input tail so the kernel can write back
  // the resolved sockaddr without a second allocation.
  return afd_ioctl(sock, IOCTL_AFD_BIND, payload_buf,
                   static_cast<ULONG>(need),
                   payload_buf + sizeof(AFD_BIND_INFO_TL),
                   static_cast<ULONG>(addrlen), iosb);
}

/// Submit IOCTL_AFD_CONNECT and manage the shared phase-CAS + park pattern:
///   - Build AFD_CONNECT_JOIN_INFO_TL header + sockaddr into payload_buf
///   - Reset state->connect_iosb and state->connect_waiter
///   - Publish SocketPhase::CONNECTING BEFORE submission so release_aux
///     observes the intent to submit; caller's socket_finalize_connect()
///     overwrites phase on synchronous completion
///   - Call afd_io::ioctl_async
///   - On STATUS_PENDING:
///       * nonblock=true:  return STATUS_PENDING (caller maps to -EINPROGRESS)
///       * nonblock=false: stamp tlw, park via reap_waiter, unstamp, return
///         the reaped status
///   - Synchronous completion returns directly (phase left at CONNECTING
///     for socket_finalize_connect to transition)
///
/// Ordering invariant: while `phase == CONNECTING`, either the IRP is
/// in flight (done may be 0 or 1) or it completed synchronously
/// (done == 1) but socket_finalize_connect has not yet rewritten phase.
/// In both cases `&state->connect_waiter` is the valid ApcContext for
/// any pending kernel completion, so release_aux can reliably cancel +
/// reap off this signal alone.
///
/// The caller is responsible for socket_finalize_connect() and any
/// AF-specific post-connect context work (e.g. AF_UNIX SET_CONTEXT).
LIBC_INLINE NTSTATUS afd_submit_connect(HANDLE sock, SocketState *state,
                                        uint8_t *payload_buf,
                                        size_t payload_capacity,
                                        const void *addr, size_t addrlen,
                                        bool nonblock) {
  size_t need = sizeof(AFD_CONNECT_JOIN_INFO_TL) + addrlen;
  if (payload_capacity < need)
    return STATUS_BUFFER_TOO_SMALL;

  auto *hdr = reinterpret_cast<AFD_CONNECT_JOIN_INFO_TL *>(payload_buf);
  hdr->SanActive = 0;
  hdr->RootEndpoint = nullptr;
  hdr->ConnectEndpoint = nullptr;
  __builtin_memcpy(payload_buf + sizeof(AFD_CONNECT_JOIN_INFO_TL), addr,
                   addrlen);

  __builtin_memset(&state->connect_iosb, 0, sizeof(state->connect_iosb));
  // No park until we know the IRP is pending — the waiter lives in
  // SocketState for nonblocking probes, so tlw must stay null by default.
  state->connect_waiter.tlw = nullptr;

  // Publish CONNECTING before submission. This closes the window where a
  // concurrent release_aux would see stale phase and skip cancel+reap
  // despite the IRP being live in the kernel. Synchronous completion
  // paths rely on socket_finalize_connect() to rewrite phase.
  state->phase.store(SocketPhase::CONNECTING, cpp::MemoryOrder::RELEASE);

  NTSTATUS s = afd_io::ioctl_async(
      sock, IOCTL_AFD_CONNECT, payload_buf, static_cast<ULONG>(need), nullptr,
      0, &state->connect_waiter, &state->connect_iosb);

  if (s != STATUS_PENDING)
    return s;

  if (nonblock)
    return STATUS_PENDING;

  // Blocking path: stamp the current thread's notify_word for the duration
  // of the park and clear it before returning so a late cancellation can't
  // touch a freed TLW.
  auto *lc = get_current_lifecycle();
  state->connect_waiter.tlw = lc ? &lc->notify_word : nullptr;
  NTSTATUS reap =
      afd_io::reap_waiter(&state->connect_waiter, &state->connect_iosb);
  state->connect_waiter.tlw = nullptr;
  return reap;
}

//===----------------------------------------------------------------------===//
// Close-path drain of a pending connect IRP
//
// When the OFD's last reference is released while the socket is still in
// SocketPhase::CONNECTING, a kernel IRP with ApcContext = &state->connect_
// waiter may still be outstanding. Freeing SocketState before the router
// fires would UAF. This helper cancels the specific IRP (via connect_iosb)
// and parks on the waiter until the router publishes completion, so the
// caller can safely socket_state_free() afterwards.
//
// Idempotent: safe to call even if the IRP already completed synchronously
// — NtCancelIoFileEx returns STATUS_NOT_FOUND and reap_waiter observes
// done == 1 without parking.
//===----------------------------------------------------------------------===//

LIBC_INLINE void socket_drain_pending_connect(HANDLE sock, SocketState *state) {
  if (state->phase.load(cpp::MemoryOrder::ACQUIRE) != SocketPhase::CONNECTING)
    return;

  IO_STATUS_BLOCK cancel_iosb = {};
  ::NtCancelIoFileEx(sock, &state->connect_iosb, &cancel_iosb);

  // Stamp tlw so the router alerts us if it hasn't already published.
  // wait_for_addr's internal Dekker protocol closes the tlw-store vs
  // router-publish race on x86; a late router publish with tlw==null
  // is safe because reap_waiter observes done==1 via ACQUIRE and skips
  // the park.
  auto *lc = get_current_lifecycle();
  state->connect_waiter.tlw = lc ? &lc->notify_word : nullptr;
  if (state->connect_waiter.tlw) {
    afd_io::reap_waiter(&state->connect_waiter, &state->connect_iosb);
  } else {
    // Pre-Tier-B code paths have no lifecycle. Sockets cannot be created
    // before Tier B so this branch should be unreachable; kept for parity
    // with ioctl_blocking's fallback.
    while (state->connect_waiter.done.load(cpp::MemoryOrder::ACQUIRE) == 0)
      __asm__ __volatile__("pause" ::: "memory");
  }
  state->connect_waiter.tlw = nullptr;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AFD_CORE_H
