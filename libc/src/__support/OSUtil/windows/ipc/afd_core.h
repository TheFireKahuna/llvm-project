//===-- Generic AFD socket helpers --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Address-family-agnostic AFD helpers. These are DRY wrappers for the
// ioctl+wait pattern, endpoint creation, NTSTATUS→errno translation, and
// connect-completion reaping. No AF_UNIX-specific code lives here.
//
// AF-specific code (transport prime, SET/GET_CONTEXT, context image
// extraction) lives in af_unix_ops.h / af_unix_ops.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AFD_CORE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AFD_CORE_H

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_afd.h"
#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

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
// Synchronous AFD ioctl — NtDeviceIoControlFile + wait if pending
//===----------------------------------------------------------------------===//

/// Issue an AFD ioctl and block until completion. The event must be a
/// per-socket or per-thread event handle (not the thread ring's event).
LIBC_INLINE NTSTATUS afd_ioctl(HANDLE socket, HANDLE event, ULONG ioctl_code,
                                void *in_buf, ULONG in_len, void *out_buf,
                                ULONG out_len, IO_STATUS_BLOCK *iosb) {
  NTSTATUS s = ::NtDeviceIoControlFile(socket, event, nullptr, nullptr, iosb,
                                       ioctl_code, in_buf, in_len, out_buf,
                                       out_len);
  if (s == STATUS_PENDING) {
    s = ::NtWaitForSingleObject(event, /*Alertable=*/1, nullptr);
    if (s == STATUS_USER_APC || s == STATUS_ALERTED) {
      IO_STATUS_BLOCK cancel_iosb = {};
      ::NtCancelIoFileEx(socket, iosb, &cancel_iosb);
      ::NtWaitForSingleObject(event, 0, nullptr);
      return STATUS_CANCELLED;
    }
    if (NT_SUCCESS(s))
      s = iosb->Status;
  }
  return s;
}

/// Non-blocking variant: returns STATUS_DEVICE_NOT_READY (→ EAGAIN) instead
/// of waiting when the operation would block. Cancels the pending IRP cleanly.
LIBC_INLINE NTSTATUS afd_ioctl_nonblock(HANDLE socket, HANDLE event,
                                         ULONG ioctl_code, void *in_buf,
                                         ULONG in_len, void *out_buf,
                                         ULONG out_len, IO_STATUS_BLOCK *iosb) {
  NTSTATUS s = ::NtDeviceIoControlFile(socket, event, nullptr, nullptr, iosb,
                                       ioctl_code, in_buf, in_len, out_buf,
                                       out_len);
  if (s == STATUS_PENDING) {
    IO_STATUS_BLOCK cancel_iosb = {};
    ::NtCancelIoFileEx(socket, iosb, &cancel_iosb);
    ::NtWaitForSingleObject(event, 0, nullptr);
    return STATUS_DEVICE_NOT_READY;
  }
  return s;
}

//===----------------------------------------------------------------------===//
// AFD endpoint creation
//===----------------------------------------------------------------------===//

/// Access mask for AFD endpoints: GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE.
inline constexpr ULONG AFD_ENDPOINT_ACCESS = 0xC0140000;

/// Open an AFD endpoint. EndpointFlags: 0 for server/normal,
/// AFD_OPEN_FLAG_ACCEPT_TARGET for accept targets, AFD_OPEN_FLAG_RIO for RIO.
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

  UNICODE_STRING device_name;
  static const WCHAR path[] = u"\\Device\\Afd\\Endpoint";
  device_name.Length = sizeof(path) - sizeof(WCHAR);
  device_name.MaximumLength = sizeof(path);
  device_name.Buffer = const_cast<WCHAR *>(path);

  auto oa = windows::named_internal_oa(&device_name);

  IO_STATUS_BLOCK iosb = {};
  return ::NtCreateFile(out, AFD_ENDPOINT_ACCESS, &oa, &iosb, nullptr, 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF, 0,
                        &ea, sizeof(ea));
}

//===----------------------------------------------------------------------===//
// Generic transport ioctl wrapper
//===----------------------------------------------------------------------===//

LIBC_INLINE NTSTATUS afd_transport_ioctl(HANDLE socket, HANDLE event,
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
  return afd_ioctl(socket, event, IOCTL_AFD_TRANSPORT_IOCTL, &tl, sizeof(tl),
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

  LARGE_INTEGER zero = {};
  NTSTATUS wait =
      NtWaitForSingleObject(state->ioctl_event, /*Alertable=*/0, &zero);
  if (wait == STATUS_TIMEOUT)
    return STATUS_PENDING;
  if (!NT_SUCCESS(wait))
    return wait;
  return socket_finalize_connect(state, state->connect_iosb.Status);
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

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AFD_CORE_H
