//===-- AF_UNIX address family operations implementation -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// AF_UNIX ops table implementation. Every function uses the per-socket
// scratch region for all transient buffers — no stack arrays.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ipc/af_unix_ops.h"
#include "hdr/errno_macros.h"
#include "hdr/types/socklen_t.h"
#include "hdr/types/struct_sockaddr.h"
#include "hdr/types/struct_sockaddr_un.h"
#include "include/llvm-libc-macros/sys-socket-macros.h"
#include "src/__support/OSUtil/windows/ipc/afd_core.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_afd.h"
#include "src/__support/OSUtil/windows/nt/nt_path_convert.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// AFD_UNIX_* state constants from nt_afd.h (global scope).
using ::AFD_UNIX_CONTEXT_STATE_BOUND;
using ::AFD_UNIX_CONTEXT_STATE_CONNECTED;
using ::AFD_UNIX_CONTEXT_STATE_OPEN;
using ::AFD_UNIX_SHARED_INFO_FLAGS_BOUND;
using ::AFD_UNIX_SHARED_INFO_FLAGS_LISTENING;

//===----------------------------------------------------------------------===//
// Internal helpers — address construction
//===----------------------------------------------------------------------===//

static void build_wildcard_sockaddr(uint8_t out[SIZEOF_SOCKADDR_UN]) {
  __builtin_memset(out, 0, SIZEOF_SOCKADDR_UN);
  uint16_t family = UNIX_ADDRESS_FAMILY;
  __builtin_memcpy(out, &family, 2);
}

static int build_pathname_sockaddr(uint8_t out[SIZEOF_SOCKADDR_UN],
                                   const char *path, size_t path_len) {
  if (path_len > UNIX_SUN_PATH_LEN)
    return -ENAMETOOLONG;
  __builtin_memset(out, 0, SIZEOF_SOCKADDR_UN);
  uint16_t family = UNIX_ADDRESS_FAMILY;
  __builtin_memcpy(out, &family, 2);
  __builtin_memcpy(out + 2, path, path_len);
  return 0;
}

static uint8_t
sockaddr_un_effective_length(const uint8_t addr[SIZEOF_SOCKADDR_UN]) {
  if (addr[2] == '\0')
    return SIZEOF_SOCKADDR_UN;
  uint8_t path_len = 0;
  while (path_len < SIZEOF_SOCKADDR_UN - 2 && addr[2 + path_len] != '\0')
    ++path_len;
  return path_len == SIZEOF_SOCKADDR_UN - 2
             ? SIZEOF_SOCKADDR_UN
             : static_cast<uint8_t>(3 + path_len);
}

//===----------------------------------------------------------------------===//
// Internal helpers — SET_CONTEXT
//===----------------------------------------------------------------------===//

static NTSTATUS
afd_set_unix_context(HANDLE socket, HANDLE event,
                     AFD_UNIX_CONTEXT_IMAGE *ctx, IO_STATUS_BLOCK *iosb,
                     int32_t state_val, int32_t local_len, int32_t remote_len,
                     uint16_t socket_flags, const void *local_addr,
                     const void *remote_addr, bool include_selector,
                     bool is_open_state) {
  uint32_t selector_len = include_selector ? UNIX_SELECTOR_BLOB_LEN : 0;
  uint32_t total_len = UNIX_CONTEXT_HEADER_LEN +
                       UNIX_CONTEXT_ADDRESS_STORAGE_LEN +
                       UNIX_CONTEXT_ADDRESS_STORAGE_LEN + selector_len;

  __builtin_memset(ctx, 0, sizeof(AFD_UNIX_CONTEXT_IMAGE));
  ctx->Header.SharedInfo.State = static_cast<SOCKET_STATE>(state_val);
  ctx->Header.SharedInfo.AddressFamily = UNIX_ADDRESS_FAMILY;
  ctx->Header.SharedInfo.SocketType = UNIX_STREAM_SOCKET_TYPE;
  ctx->Header.SharedInfo.Protocol = 0;
  ctx->Header.SharedInfo.LocalAddressLength = local_len;
  ctx->Header.SharedInfo.RemoteAddressLength = remote_len;
  ctx->Header.SharedInfo.ReceiveBufferSize =
      AFD_UNIX_DEFAULT_RECEIVE_BUFFER_SIZE;
  ctx->Header.SharedInfo.SendBufferSize = AFD_UNIX_DEFAULT_SEND_BUFFER_SIZE;
  ctx->Header.SharedInfo.Flags = socket_flags;
  ctx->Header.SharedInfo.CreationFlags = AFD_UNIX_SHARED_INFO_CREATION_FLAGS;
  ctx->Header.SharedInfo.CatalogEntryId =
      AFD_UNIX_SHARED_INFO_OBSERVED_PROTOCOL_CATALOG_ENTRY_ID;
  ctx->Header.SharedInfo.ServiceFlags1 = AFD_UNIX_SHARED_INFO_SERVICE_FLAGS1;
  ctx->Header.SharedInfo.ProviderFlags = AFD_UNIX_SHARED_INFO_PROVIDER_FLAGS;
  __builtin_memcpy(ctx->Header.SharedInfo.ProviderId, AFD_UNIX_PROVIDER_GUID,
                   sizeof(AFD_UNIX_PROVIDER_GUID));
  ctx->Header.SelectorBlobLength = selector_len;
  ctx->Header.TailAlignmentPadding = AFD_UNIX_CONTEXT_TAIL_ALIGNMENT_PADDING;

  if (local_addr)
    __builtin_memcpy(ctx->LocalAddress, local_addr,
                     local_len < static_cast<int32_t>(SIZEOF_SOCKADDR_UN)
                         ? local_len
                         : SIZEOF_SOCKADDR_UN);
  if (remote_addr)
    __builtin_memcpy(ctx->RemoteAddress, remote_addr,
                     remote_len < static_cast<int32_t>(SIZEOF_SOCKADDR_UN)
                         ? remote_len
                         : SIZEOF_SOCKADDR_UN);

  if (include_selector) {
    AFD_UNIX_SELECTOR_BLOB selector = {};
    selector.AddressFamilyValue = UNIX_ADDRESS_FAMILY;
    __builtin_memcpy(ctx->SelectorBlob, &selector, sizeof(selector));
  }

  __builtin_memset(iosb, 0, sizeof(IO_STATUS_BLOCK));
  if (is_open_state)
    return afd_ioctl(socket, event, IOCTL_AFD_SET_CONTEXT, ctx, total_len,
                     ctx->RemoteAddress, UNIX_CONTEXT_ADDRESS_STORAGE_LEN,
                     iosb);
  return afd_ioctl(socket, event, IOCTL_AFD_SET_CONTEXT, ctx, total_len,
                   nullptr, 0, iosb);
}

//===----------------------------------------------------------------------===//
// Internal helpers — transport prime
//===----------------------------------------------------------------------===//

static NTSTATUS afd_transport_prime(HANDLE socket, HANDLE event,
                                    uint8_t *payload_buf,
                                    WCHAR *nt_path_buf,
                                    char *path_z,
                                    IO_STATUS_BLOCK *iosb,
                                    const char *posix_path,
                                    size_t path_len) {
  // Null-terminate the POSIX path (sun_path may not be terminated if
  // the caller filled all 108 bytes). path_z points into the caller's
  // Work struct on the per-socket scratch region (109 bytes).
  if (path_len > UNIX_SUN_PATH_LEN)
    return STATUS_NAME_TOO_LONG;
  __builtin_memcpy(path_z, posix_path, path_len);
  path_z[path_len] = '\0';

  // Resolve to a full NT path (\??\C:\cwd\path) using the scratch
  // buffer. to_nt_path handles relative paths via PEB CWD, absolute
  // DOS paths, and UNC paths.
  size_t nt_len =
      to_nt_path(path_z, nt_path_buf, UNIX_SOCKET_NT_PATH_WCHARS);
  if (nt_len == 0)
    return STATUS_OBJECT_NAME_INVALID;

  // Build the AFD_UNIX_SET_FILE_PATH payload in the scratch region.
  size_t nt_bytes = nt_len * sizeof(WCHAR);
  size_t payload_len = sizeof(HANDLE) + nt_bytes;
  size_t payload_capacity =
      sizeof(HANDLE) + sizeof(WCHAR) * UNIX_SOCKET_NT_PATH_WCHARS;
  if (payload_len > payload_capacity)
    return STATUS_BUFFER_TOO_SMALL;

  __builtin_memset(payload_buf, 0, payload_capacity);
  auto *set_path =
      reinterpret_cast<AFD_UNIX_SET_FILE_PATH_INPUT *>(payload_buf);
  set_path->FileHandle = nullptr;
  __builtin_memcpy(set_path->NtPath, nt_path_buf, nt_bytes);

  __builtin_memset(iosb, 0, sizeof(IO_STATUS_BLOCK));
  return afd_transport_ioctl(socket, event, TlSocketIoControlType,
                             AFD_AFINUX_SET_FILE_PATH, payload_buf,
                             static_cast<ULONG>(payload_len), nullptr, 0,
                             iosb);
}

//===----------------------------------------------------------------------===//
// Internal helpers — context query and address extraction
//===----------------------------------------------------------------------===//

static bool
context_is_plausible(const AFD_UNIX_CONTEXT_IMAGE *context,
                     ULONG_PTR returned_length) {
  if (!context || returned_length < sizeof(AFD_CONTEXT_HEADER))
    return false;
  const SOCK_SHARED_INFO &shared = context->Header.SharedInfo;
  if (shared.AddressFamily != UNIX_ADDRESS_FAMILY ||
      shared.SocketType != UNIX_STREAM_SOCKET_TYPE || shared.Protocol != 0)
    return false;
  if (shared.LocalAddressLength < 0 ||
      shared.LocalAddressLength > static_cast<LONG>(SIZEOF_SOCKADDR_UN) ||
      shared.RemoteAddressLength < 0 ||
      shared.RemoteAddressLength > static_cast<LONG>(SIZEOF_SOCKADDR_UN))
    return false;
  if (returned_length < UNIX_CONTEXT_SELECTOR_OFFSET_VAL)
    return false;
  ULONG selector_len = context->Header.SelectorBlobLength;
  if (selector_len != 0 &&
      selector_len < AFD_UNIX_CONTEXT_MIN_SELECTOR_LENGTH)
    return false;
  if (returned_length < static_cast<ULONG_PTR>(UNIX_CONTEXT_SELECTOR_OFFSET_VAL +
                                               selector_len))
    return false;
  if (selector_len != 0) {
    const auto *snapshot = reinterpret_cast<
        const WSHUNIX_SOCKET_CONTEXT_SNAPSHOT_HEADER *>(context->SelectorBlob);
    if (snapshot->PathState.AddressFamilyValue != UNIX_ADDRESS_FAMILY)
      return false;
    if ((snapshot->PathState.LocalPathByteCount & 1) != 0 ||
        (snapshot->PathState.RemotePathByteCount & 1) != 0)
      return false;
    ULONG required_selector_len =
        AFD_UNIX_CONTEXT_MIN_SELECTOR_LENGTH +
        snapshot->PathState.LocalPathByteCount +
        snapshot->PathState.RemotePathByteCount;
    if (selector_len < required_selector_len)
      return false;
  }
  return true;
}

static bool
context_extract_address(const AFD_UNIX_CONTEXT_IMAGE *context,
                        ULONG_PTR returned_length, bool remote,
                        uint8_t out[SIZEOF_SOCKADDR_UN], ULONG *out_len) {
  if (!context_is_plausible(context, returned_length))
    return false;
  LONG reported = remote ? context->Header.SharedInfo.RemoteAddressLength
                         : context->Header.SharedInfo.LocalAddressLength;
  if (reported <= 0)
    return false;
  ULONG clamped = static_cast<ULONG>(reported);
  if (clamped > SIZEOF_SOCKADDR_UN)
    clamped = SIZEOF_SOCKADDR_UN;
  ULONG offset = remote
                     ? static_cast<ULONG>(__builtin_offsetof(
                           AFD_UNIX_CONTEXT_IMAGE, RemoteAddress))
                     : static_cast<ULONG>(__builtin_offsetof(
                           AFD_UNIX_CONTEXT_IMAGE, LocalAddress));
  if (returned_length < static_cast<ULONG_PTR>(offset + clamped))
    return false;
  __builtin_memset(out, 0, SIZEOF_SOCKADDR_UN);
  __builtin_memcpy(out,
                   reinterpret_cast<const uint8_t *>(context) + offset,
                   clamped);
  if (out_len)
    *out_len = clamped;
  return true;
}

//===----------------------------------------------------------------------===//
// Internal helpers — copy utilities
//===----------------------------------------------------------------------===//

static void copy_sockaddr_to_user(const uint8_t *src, socklen_t src_len,
                                  struct sockaddr *addr, socklen_t *addrlen) {
  socklen_t copy = *addrlen;
  if (copy > src_len)
    copy = src_len;
  __builtin_memcpy(addr, src, copy);
  *addrlen = src_len;
}

static void cache_addr(uint8_t *dst, socklen_t *dst_len,
                       const uint8_t *src, ULONG src_len) {
  if (src_len > SIZEOF_SOCKADDR_UN)
    src_len = SIZEOF_SOCKADDR_UN;
  __builtin_memset(dst, 0, MAX_SOCKADDR_STORAGE);
  __builtin_memcpy(dst, src, src_len);
  *dst_len = static_cast<socklen_t>(src_len);
}

//===----------------------------------------------------------------------===//
// Ops: validate_create
//===----------------------------------------------------------------------===//

static int af_unix_validate_create(int type, int protocol) {
  if (type != SOCK_STREAM)
    return -EPROTOTYPE;
  if (protocol != 0)
    return -EPROTONOSUPPORT;
  return 0;
}

//===----------------------------------------------------------------------===//
// Ops: do_bind
//===----------------------------------------------------------------------===//

static intptr_t af_unix_do_bind(HANDLE socket, SocketState *state,
                                const struct sockaddr *addr,
                                socklen_t addrlen) {
  if (addr->sa_family != AF_UNIX || addrlen < 2)
    return -EINVAL;

  auto *sun = reinterpret_cast<const struct sockaddr_un *>(addr);
  size_t path_len = addrlen - offsetof(struct sockaddr_un, sun_path);

  if (path_len > 0 && sun->sun_path[0] == '\0')
    return -EADDRNOTAVAIL;
  if (path_len > UNIX_SUN_PATH_LEN)
    return -ENAMETOOLONG;

  HANDLE evt = state->ioctl_event;
  auto *w = reinterpret_cast<UnixBindWork *>(socket_scratch(state));
  __builtin_memset(w, 0, sizeof(UnixBindWork));

  // 1. Open-state context (required before first ioctl).
  NTSTATUS s = afd_set_unix_context(
      socket, evt, &w->open_ctx, &w->open_iosb,
      AFD_UNIX_CONTEXT_STATE_OPEN, SIZEOF_SOCKADDR_UN, SIZEOF_SOCKADDR_UN,
      AFD_UNIX_SHARED_INFO_FLAGS_BOUND, nullptr, nullptr,
      /*include_selector=*/false, /*is_open_state=*/true);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);

  bool is_wildcard = (path_len == 0);

  // 2. Transport prime (pathname sockets only).
  if (!is_wildcard) {
    s = afd_transport_prime(socket, evt, w->transport_payload, w->nt_path,
                            w->path_z, &w->prime_iosb, sun->sun_path,
                            path_len);
    if (!NT_SUCCESS(s))
      return -ntstatus_to_errno_socket(s);
  }

  // 3. Build bind address.
  if (is_wildcard) {
    build_wildcard_sockaddr(w->bind_addr);
  } else {
    int err = build_pathname_sockaddr(w->bind_addr, sun->sun_path, path_len);
    if (err)
      return err;
  }

  // 4. IOCTL_AFD_BIND.
  auto *bind_info = reinterpret_cast<AFD_BIND_INFO_TL *>(w->bind_buf);
  bind_info->ShareAccess =
      (state->socket_options.load(cpp::MemoryOrder::ACQUIRE) &
       SocketState::OPT_REUSEADDR)
          ? AfdBindReuseAddress
          : AfdBindNormalAddressUse;
  __builtin_memcpy(w->bind_buf + sizeof(AFD_BIND_INFO_TL), w->bind_addr,
                   SIZEOF_SOCKADDR_UN);

  s = afd_ioctl(socket, evt, IOCTL_AFD_BIND, w->bind_buf,
                sizeof(w->bind_buf), w->bind_buf, SIZEOF_SOCKADDR_UN,
                &w->bind_iosb);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);

  int32_t actual_local_len =
      static_cast<int32_t>(sockaddr_un_effective_length(w->bind_addr));

  // 5. Bound-state context with selector blob.
  s = afd_set_unix_context(
      socket, evt, &w->bound_ctx, &w->bound_iosb,
      AFD_UNIX_CONTEXT_STATE_BOUND, actual_local_len, SIZEOF_SOCKADDR_UN,
      AFD_UNIX_SHARED_INFO_FLAGS_BOUND, w->bind_addr, nullptr,
      /*include_selector=*/true, /*is_open_state=*/false);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);

  // 6. Cache bound address.
  __builtin_memcpy(state->local_addr, w->bind_addr, SIZEOF_SOCKADDR_UN);
  state->local_len = static_cast<socklen_t>(actual_local_len);

  return 0;
}

//===----------------------------------------------------------------------===//
// Ops: do_connect
//===----------------------------------------------------------------------===//

static intptr_t af_unix_do_connect(HANDLE socket, SocketState *state,
                                   bool nonblock,
                                   const struct sockaddr *addr,
                                   socklen_t addrlen) {
  if (addr->sa_family != AF_UNIX || addrlen < 2)
    return -EINVAL;

  auto *sun = reinterpret_cast<const struct sockaddr_un *>(addr);
  size_t path_len = addrlen - offsetof(struct sockaddr_un, sun_path);

  if (path_len == 0 || sun->sun_path[0] == '\0')
    return -EINVAL;
  if (path_len > UNIX_SUN_PATH_LEN)
    return -ENAMETOOLONG;

  HANDLE evt = state->ioctl_event;
  auto *w = reinterpret_cast<UnixConnectWork *>(socket_scratch(state));
  __builtin_memset(w, 0, sizeof(UnixConnectWork));

  NTSTATUS s;

  // Auto-bind wildcard if not yet bound.
  if (state->phase.load(cpp::MemoryOrder::ACQUIRE) == SocketPhase::UNBOUND) {
    // Open-state context.
    s = afd_set_unix_context(
        socket, evt, &w->open_ctx, &w->open_iosb,
        AFD_UNIX_CONTEXT_STATE_OPEN, SIZEOF_SOCKADDR_UN, SIZEOF_SOCKADDR_UN,
        AFD_UNIX_SHARED_INFO_FLAGS_BOUND, nullptr, nullptr,
        /*include_selector=*/false, /*is_open_state=*/true);
    if (!NT_SUCCESS(s))
      return -ntstatus_to_errno_socket(s);

    // Bind wildcard.
    build_wildcard_sockaddr(w->wildcard);
    auto *bind_info [[maybe_unused]] =
        reinterpret_cast<AFD_BIND_INFO_TL *>(w->auto_bind_buf);
    __builtin_memcpy(w->auto_bind_buf + sizeof(AFD_BIND_INFO_TL),
                     w->wildcard, SIZEOF_SOCKADDR_UN);

    s = afd_ioctl(socket, evt, IOCTL_AFD_BIND, w->auto_bind_buf,
                  sizeof(w->auto_bind_buf), w->auto_bind_buf,
                  SIZEOF_SOCKADDR_UN, &w->auto_bind_iosb);
    if (!NT_SUCCESS(s))
      return -ntstatus_to_errno_socket(s);

    // Bound-state context.
    s = afd_set_unix_context(
        socket, evt, &w->bound_ctx, &w->bound_iosb,
        AFD_UNIX_CONTEXT_STATE_BOUND, SIZEOF_SOCKADDR_UN,
        SIZEOF_SOCKADDR_UN, AFD_UNIX_SHARED_INFO_FLAGS_BOUND,
        w->wildcard, nullptr, /*include_selector=*/true,
        /*is_open_state=*/false);
    if (!NT_SUCCESS(s))
      return -ntstatus_to_errno_socket(s);

    __builtin_memcpy(state->local_addr, w->wildcard, SIZEOF_SOCKADDR_UN);
    state->local_len = SIZEOF_SOCKADDR_UN;
    state->phase.store(SocketPhase::BOUND, cpp::MemoryOrder::RELEASE);
  }

  // Transport prime with server path.
  s = afd_transport_prime(socket, evt, w->transport_payload, w->nt_path,
                          w->path_z, &w->prime_iosb, sun->sun_path,
                          path_len);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);

  // Build connect payload.
  auto *conn = reinterpret_cast<AFD_CONNECT_JOIN_INFO_TL *>(w->connect_buf);
  conn->SanActive = 0;
  conn->RootEndpoint = nullptr;
  conn->ConnectEndpoint = nullptr;

  int err = build_pathname_sockaddr(w->remote_addr, sun->sun_path, path_len);
  if (err)
    return err;
  __builtin_memcpy(state->remote_addr, w->remote_addr, SIZEOF_SOCKADDR_UN);
  state->remote_len =
      static_cast<socklen_t>(sockaddr_un_effective_length(w->remote_addr));
  state->socket_error.store(0, cpp::MemoryOrder::RELEASE);

  __builtin_memcpy(w->connect_buf + sizeof(AFD_CONNECT_JOIN_INFO_TL),
                   w->remote_addr, SIZEOF_SOCKADDR_UN);

  // IOCTL_AFD_CONNECT — use persistent connect_iosb in state.
  __builtin_memset(&state->connect_iosb, 0, sizeof(state->connect_iosb));
  s = ::NtDeviceIoControlFile(socket, evt, nullptr, nullptr,
                              &state->connect_iosb, IOCTL_AFD_CONNECT,
                              w->connect_buf, sizeof(w->connect_buf),
                              nullptr, 0);

  if (s == STATUS_PENDING) {
    state->phase.store(SocketPhase::CONNECTING, cpp::MemoryOrder::RELEASE);
    if (nonblock)
      return -EINPROGRESS;

    // Blocking: alertable wait.
    s = ::NtWaitForSingleObject(evt, /*Alertable=*/TRUE, nullptr);
    if (s == STATUS_USER_APC || s == STATUS_ALERTED) {
      IO_STATUS_BLOCK cancel_iosb = {};
      ::NtCancelIoFileEx(socket, &state->connect_iosb, &cancel_iosb);
      ::NtWaitForSingleObject(evt, 0, nullptr);
      state->phase.store(SocketPhase::BOUND, cpp::MemoryOrder::RELEASE);
      return -EINTR;
    }
    if (NT_SUCCESS(s))
      s = state->connect_iosb.Status;
  }

  // Finalize phase transition.
  s = socket_finalize_connect(state, s);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);

  // Post-connect context update (best-effort).
  (void)afd_set_unix_context(
      socket, evt, &w->connected_ctx, &w->connected_iosb,
      AFD_UNIX_CONTEXT_STATE_CONNECTED, state->local_len, state->remote_len,
      AFD_UNIX_SHARED_INFO_FLAGS_BOUND, state->local_addr, state->remote_addr,
      /*include_selector=*/true, /*is_open_state=*/false);

  return 0;
}

//===----------------------------------------------------------------------===//
// Ops: setup_accepted
//===----------------------------------------------------------------------===//

static intptr_t
af_unix_setup_accepted(HANDLE listener, SocketState *listener_state,
                       int32_t wfl_sequence, const uint8_t *wfl_remote,
                       socklen_t wfl_remote_len, HANDLE *out_handle,
                       SocketState *acc_state, struct sockaddr *addr,
                       socklen_t *addrlen) {
  HANDLE evt = acc_state->ioctl_event;
  auto *w = reinterpret_cast<UnixAcceptWork *>(socket_scratch(acc_state));
  __builtin_memset(w, 0, sizeof(UnixAcceptWork));

  // Open accept-target endpoint.
  HANDLE acc_handle = nullptr;
  NTSTATUS s = afd_open_endpoint(&acc_handle, AF_UNIX, SOCK_STREAM, 0,
                                 AFD_OPEN_FLAG_ACCEPT_TARGET);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);

  // IOCTL_AFD_ACCEPT.
  w->accept_info.Sequence = wfl_sequence;
  w->accept_info.AcceptHandle = acc_handle;

  s = afd_ioctl(listener, listener_state->ioctl_event, IOCTL_AFD_ACCEPT,
                &w->accept_info, sizeof(w->accept_info), nullptr, 0,
                &w->accept_iosb);
  if (!NT_SUCCESS(s)) {
    NtClose(acc_handle);
    return -ntstatus_to_errno_socket(s);
  }

  // The accepted socket needs its own ioctl event — create now.
  // (acc_state->ioctl_event was set by the caller before calling us.)
  // If no event was provided, create one.
  if (!evt) {
    auto evt_oa = windows::internal_oa();
    NtCreateEvent(&evt, EVENT_MODIFY_STATE | SYNCHRONIZE, &evt_oa,
                  SynchronizationEvent,
                  FALSE);
    acc_state->ioctl_event = evt;
  }

  // Set non-blocking on accepted endpoint.
  if (evt) {
    w->nb_info.InformationType = AFD_NONBLOCKING_MODE;
    w->nb_info.Information.Boolean = TRUE;
    afd_ioctl(acc_handle, evt, IOCTL_AFD_SET_INFORMATION, &w->nb_info,
              sizeof(w->nb_info), nullptr, 0, &w->nb_iosb);
  }

  // Compute remote address length.
  socklen_t accepted_remote_len =
      static_cast<socklen_t>(sockaddr_un_effective_length(wfl_remote));

  // Post-accept context (best-effort).
  if (evt) {
    (void)afd_set_unix_context(
        acc_handle, evt, &w->ctx, &w->ctx_iosb,
        AFD_UNIX_CONTEXT_STATE_CONNECTED, listener_state->local_len,
        static_cast<int32_t>(accepted_remote_len),
        AFD_UNIX_SHARED_INFO_FLAGS_BOUND, listener_state->local_addr,
        wfl_remote, /*include_selector=*/false, /*is_open_state=*/false);
  }

  // Populate accepted state.
  acc_state->ops = &af_unix_ops;
  acc_state->domain = AF_UNIX;
  acc_state->type = SOCK_STREAM;
  acc_state->protocol = 0;
  acc_state->phase.store(SocketPhase::CONNECTED, cpp::MemoryOrder::RELAXED);

  __builtin_memcpy(acc_state->local_addr, listener_state->local_addr,
                   MAX_SOCKADDR_STORAGE);
  acc_state->local_len = listener_state->local_len;
  __builtin_memcpy(acc_state->remote_addr, wfl_remote,
                   wfl_remote_len <= MAX_SOCKADDR_STORAGE
                       ? wfl_remote_len
                       : MAX_SOCKADDR_STORAGE);
  acc_state->remote_len = accepted_remote_len;

  // Output address to user if requested.
  if (addr && addrlen) {
    socklen_t copy = *addrlen;
    if (copy > accepted_remote_len)
      copy = accepted_remote_len;
    __builtin_memcpy(addr, wfl_remote, copy);
    *addrlen = accepted_remote_len;
  }

  *out_handle = acc_handle;
  return 0;
}

//===----------------------------------------------------------------------===//
// Ops: post_listen
//===----------------------------------------------------------------------===//

static intptr_t af_unix_post_listen(HANDLE socket, SocketState *state) {
  auto *w = reinterpret_cast<UnixListenWork *>(socket_scratch(state));
  __builtin_memset(w, 0, sizeof(UnixListenWork));

  NTSTATUS s = afd_set_unix_context(
      socket, state->ioctl_event, &w->ctx, &w->iosb,
      AFD_UNIX_CONTEXT_STATE_BOUND, state->local_len, SIZEOF_SOCKADDR_UN,
      AFD_UNIX_SHARED_INFO_FLAGS_LISTENING, state->local_addr, nullptr,
      /*include_selector=*/true, /*is_open_state=*/false);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);
  return 0;
}

//===----------------------------------------------------------------------===//
// Ops: do_getsockname / do_getpeername
//===----------------------------------------------------------------------===//

static bool
query_name_via_endpoint(HANDLE socket, SocketState *state,
                        ULONG endpoint_ioctl_code, UnixNameQueryWork *w,
                        uint8_t *cache, socklen_t *cache_len,
                        struct sockaddr *addr, socklen_t *addrlen) {
  __builtin_memset(w->endpoint_out, 0, SIZEOF_SOCKADDR_UN);
  NTSTATUS s = afd_transport_ioctl(socket, state->ioctl_event,
                                   TlEndpointIoControlType,
                                   endpoint_ioctl_code, nullptr, 0,
                                   w->endpoint_out, SIZEOF_SOCKADDR_UN,
                                   &w->endpoint_iosb);
  ULONG out_len =
      NT_SUCCESS(s) ? static_cast<ULONG>(w->endpoint_iosb.Information) : 0;
  if (!NT_SUCCESS(s) || out_len == 0)
    return false;

  cache_addr(cache, cache_len, w->endpoint_out, out_len);
  copy_sockaddr_to_user(cache, *cache_len, addr, addrlen);
  return true;
}

static bool
query_name_via_context(HANDLE socket, SocketState *state, bool remote,
                       UnixNameQueryWork *w, uint8_t *cache,
                       socklen_t *cache_len, struct sockaddr *addr,
                       socklen_t *addrlen) {
  __builtin_memset(w->context_storage, 0, sizeof(w->context_storage));
  __builtin_memset(&w->context_iosb, 0, sizeof(w->context_iosb));
  NTSTATUS s =
      afd_ioctl(socket, state->ioctl_event, IOCTL_AFD_GET_CONTEXT, nullptr, 0,
                w->context_storage, sizeof(w->context_storage),
                &w->context_iosb);
  if (!NT_SUCCESS(s))
    return false;

  auto *ctx =
      reinterpret_cast<const AFD_UNIX_CONTEXT_IMAGE *>(w->context_storage);
  __builtin_memset(w->extracted_addr, 0, SIZEOF_SOCKADDR_UN);
  ULONG out_len = 0;
  bool ok = context_extract_address(ctx, w->context_iosb.Information, remote,
                                    w->extracted_addr, &out_len);
  if (!ok || out_len == 0)
    return false;

  cache_addr(cache, cache_len, w->extracted_addr, out_len);
  copy_sockaddr_to_user(cache, *cache_len, addr, addrlen);
  return true;
}

static intptr_t af_unix_do_getsockname(HANDLE socket, SocketState *state,
                                       struct sockaddr *addr,
                                       socklen_t *addrlen) {
  if (state->phase.load(cpp::MemoryOrder::ACQUIRE) == SocketPhase::CONNECTING)
    (void)socket_probe_connect_completion(state);

  auto *w = reinterpret_cast<UnixNameQueryWork *>(socket_scratch(state));
  __builtin_memset(w, 0, sizeof(UnixNameQueryWork));

  // Try endpoint ioctl.
  if (query_name_via_endpoint(socket, state,
                              AFD_AFINUX_ENDPOINT_QUERY_LOCAL_ADDRESS, w,
                              state->local_addr, &state->local_len,
                              addr, addrlen))
    return 0;

  // Try context extraction.
  if (query_name_via_context(socket, state, /*remote=*/false, w,
                             state->local_addr, &state->local_len,
                             addr, addrlen))
    return 0;

  // Try cache.
  if (state->local_len > 0) {
    copy_sockaddr_to_user(state->local_addr, state->local_len, addr, addrlen);
    return 0;
  }

  // Fallback: AFD_GET_ADDRESS.
  __builtin_memset(w->endpoint_out, 0, SIZEOF_SOCKADDR_UN);
  __builtin_memset(&w->endpoint_iosb, 0, sizeof(w->endpoint_iosb));
  NTSTATUS s = afd_ioctl(socket, state->ioctl_event, IOCTL_AFD_GET_ADDRESS,
                         nullptr, 0, w->endpoint_out, SIZEOF_SOCKADDR_UN,
                         &w->endpoint_iosb);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);

  socklen_t actual = static_cast<socklen_t>(w->endpoint_iosb.Information);
  cache_addr(state->local_addr, &state->local_len, w->endpoint_out,
             static_cast<ULONG>(actual));
  copy_sockaddr_to_user(state->local_addr, state->local_len, addr, addrlen);
  return 0;
}

static intptr_t af_unix_do_getpeername(HANDLE socket, SocketState *state,
                                       struct sockaddr *addr,
                                       socklen_t *addrlen) {
  if (state->phase.load(cpp::MemoryOrder::ACQUIRE) == SocketPhase::CONNECTING) {
    NTSTATUS cs = socket_probe_connect_completion(state);
    if (cs == STATUS_PENDING)
      return -ENOTCONN;
  }
  if (state->phase.load(cpp::MemoryOrder::ACQUIRE) != SocketPhase::CONNECTED)
    return -ENOTCONN;

  auto *w = reinterpret_cast<UnixNameQueryWork *>(socket_scratch(state));
  __builtin_memset(w, 0, sizeof(UnixNameQueryWork));

  // Try endpoint ioctl.
  if (query_name_via_endpoint(socket, state,
                              AFD_AFINUX_ENDPOINT_QUERY_REMOTE_ADDRESS, w,
                              state->remote_addr, &state->remote_len,
                              addr, addrlen))
    return 0;

  // Try context extraction.
  if (query_name_via_context(socket, state, /*remote=*/true, w,
                             state->remote_addr, &state->remote_len,
                             addr, addrlen))
    return 0;

  // Try cache.
  if (state->remote_len > 0) {
    copy_sockaddr_to_user(state->remote_addr, state->remote_len, addr,
                          addrlen);
    return 0;
  }

  // Fallback: AFD_GET_REMOTE_ADDRESS.
  __builtin_memset(w->endpoint_out, 0, SIZEOF_SOCKADDR_UN);
  __builtin_memset(&w->endpoint_iosb, 0, sizeof(w->endpoint_iosb));
  NTSTATUS s = afd_ioctl(socket, state->ioctl_event,
                         IOCTL_AFD_GET_REMOTE_ADDRESS, nullptr, 0,
                         w->endpoint_out, SIZEOF_SOCKADDR_UN,
                         &w->endpoint_iosb);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);

  socklen_t actual = static_cast<socklen_t>(w->endpoint_iosb.Information);
  cache_addr(state->remote_addr, &state->remote_len, w->endpoint_out,
             static_cast<ULONG>(actual));
  copy_sockaddr_to_user(state->remote_addr, state->remote_len, addr, addrlen);
  return 0;
}

//===----------------------------------------------------------------------===//
// Ops: do_getsockopt / do_setsockopt (AF_UNIX-specific options)
//===----------------------------------------------------------------------===//

static intptr_t af_unix_do_getsockopt(HANDLE socket, SocketState *state,
                                      int level, int optname, void *optval,
                                      socklen_t *optlen) {
  // AF_UNIX only supports SO_PEERCRED at the protocol level.
  if (level != SOL_SOCKET || optname != SO_PEERCRED)
    return -ENOPROTOOPT;

  if (state->phase.load(cpp::MemoryOrder::ACQUIRE) != SocketPhase::CONNECTED)
    return -ENOTCONN;
  if (!optval || !optlen || *optlen < sizeof(int))
    return -EINVAL;

  auto *w = reinterpret_cast<UnixSockoptWork *>(socket_scratch(state));
  __builtin_memset(w, 0, sizeof(UnixSockoptWork));

  NTSTATUS s = afd_transport_ioctl(
      socket, state->ioctl_event, TlSocketIoControlType,
      SIO_AF_UNIX_GETPEERPID, nullptr, 0, &w->pid, sizeof(w->pid), &w->iosb);
  if (!NT_SUCCESS(s))
    return -ntstatus_to_errno_socket(s);

  int val = static_cast<int>(w->pid);
  __builtin_memcpy(optval, &val, sizeof(int));
  *optlen = sizeof(int);
  return 0;
}

static intptr_t af_unix_do_setsockopt(HANDLE /*socket*/, SocketState * /*state*/,
                                      int /*level*/, int /*optname*/,
                                      const void * /*optval*/,
                                      socklen_t /*optlen*/) {
  // AF_UNIX has no protocol-level settable options beyond SOL_SOCKET
  // (which is handled by generic code).
  return -ENOPROTOOPT;
}

//===----------------------------------------------------------------------===//
// AF_UNIX ops table — static instance
//===----------------------------------------------------------------------===//
// resolve_af_ops() lives in af_resolve.cpp — the central registry.
// Adding a new AF never touches this file.

const AddressFamilyOps af_unix_ops = {
    /*domain=*/AF_UNIX,
    /*max_addr_len=*/SIZEOF_SOCKADDR_UN,
    /*validate_create=*/af_unix_validate_create,
    /*do_bind=*/af_unix_do_bind,
    /*do_connect=*/af_unix_do_connect,
    /*setup_accepted=*/af_unix_setup_accepted,
    /*post_listen=*/af_unix_post_listen,
    /*do_getsockname=*/af_unix_do_getsockname,
    /*do_getpeername=*/af_unix_do_getpeername,
    /*do_getsockopt=*/af_unix_do_getsockopt,
    /*do_setsockopt=*/af_unix_do_setsockopt,
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
