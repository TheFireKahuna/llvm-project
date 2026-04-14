//===-- Address family operations dispatch table ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-address-family operations table for the socket engine. One static
// instance per supported family (AF_UNIX, future AF_INET/AF_INET6). Resolved
// once at socket() and stored in SocketState::ops.
//
// The ops functions own all AF-specific logic: address validation, endpoint
// preparation (transport prime, SET_CONTEXT for AF_UNIX; nothing for AF_INET),
// AFD ioctl issuance for bind/connect/accept, context management, and address
// queries. They use the per-socket scratch region in SocketState for all
// transient buffers — no stack arrays.
//
// Generic lifecycle code (socket_lifecycle.cpp) is a thin state-machine
// orchestrator: fd lookup, phase checks, delegation to ops, phase transitions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AF_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AF_OPS_H

#include "hdr/types/socklen_t.h"
#include "hdr/types/struct_sockaddr.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct SocketState; // forward — defined in socket_state.h

/// Maximum sockaddr size across all supported families.
/// AF_UNIX: 110, AF_INET: 16, AF_INET6: 28.
inline constexpr socklen_t MAX_SOCKADDR_STORAGE = 128;

/// Per-address-family operations. Each function uses the SocketState's
/// scratch region for all transient buffers (no stack arrays).
///
/// Serialization: the per-socket ioctl_event guarantees at most one
/// lifecycle operation per socket at a time. Ops functions may freely
/// overlay the entire scratch region with their own Work structs.
struct AddressFamilyOps {
  int domain;
  socklen_t max_addr_len;

  // ─── socket() ──────────────────────────────────────────────────────
  /// Validate type+protocol combination for this address family.
  /// Returns 0 on success, -errno on failure.
  int (*validate_create)(int type, int protocol);

  // ─── bind() ────────────────────────────────────────────────────────
  /// Full bind operation: validate address, prepare endpoint (AF_UNIX:
  /// open-state context + transport prime), issue IOCTL_AFD_BIND,
  /// post-process (AF_UNIX: bound-state context), cache local address
  /// in state. Returns 0 or -errno.
  intptr_t (*do_bind)(HANDLE socket, SocketState *state,
                      const struct sockaddr *addr, socklen_t addrlen);

  // ─── connect() ─────────────────────────────────────────────────────
  /// Full connect operation: validate address, auto-bind if UNBOUND,
  /// prepare (AF_UNIX: transport prime), issue IOCTL_AFD_CONNECT,
  /// handle nonblock (EINPROGRESS) or blocking wait, finalize
  /// (AF_UNIX: connected-state context). Cache remote address.
  /// Returns 0, -EINPROGRESS, or -errno.
  intptr_t (*do_connect)(HANDLE socket, SocketState *state, bool nonblock,
                         const struct sockaddr *addr, socklen_t addrlen);

  // ─── accept() ──────────────────────────────────────────────────────
  /// After generic WAIT_FOR_LISTEN: open accept-target endpoint, issue
  /// IOCTL_AFD_ACCEPT, set nonblocking, AF-specific post-accept
  /// (AF_UNIX: SET_CONTEXT). Populate acc_state with addresses.
  ///
  /// @param listener        Listener AFD handle.
  /// @param listener_state  Listener's SocketState (for local address).
  /// @param wfl_sequence    Connection sequence from WAIT_FOR_LISTEN.
  /// @param wfl_remote      Remote address bytes from WAIT_FOR_LISTEN.
  /// @param wfl_remote_len  Length of remote address data.
  /// @param out_handle      [out] Accepted socket's AFD handle.
  /// @param acc_state       Pre-allocated SocketState for the accepted
  ///                        socket. Uses acc_state->scratch internally.
  /// @param addr            [out] Optional user address buffer.
  /// @param addrlen         [in/out] Optional address length.
  /// Returns 0 or -errno.
  intptr_t (*setup_accepted)(HANDLE listener, SocketState *listener_state,
                             int32_t wfl_sequence,
                             const uint8_t *wfl_remote, socklen_t wfl_remote_len,
                             HANDLE *out_handle, SocketState *acc_state,
                             struct sockaddr *addr, socklen_t *addrlen);

  // ─── listen() ──────────────────────────────────────────────────────
  /// Post-listen: AF-specific context update after IOCTL_AFD_START_LISTEN.
  /// AF_UNIX: SET_CONTEXT with listening flags. AF_INET: no-op.
  /// Returns 0 or -errno.
  intptr_t (*post_listen)(HANDLE socket, SocketState *state);

  // ─── getsockname / getpeername ─────────────────────────────────────
  /// Query local/remote address. AF_UNIX: endpoint ioctl → context
  /// fallback → cache → AFD_GET_ADDRESS. AF_INET: AFD_GET_ADDRESS.
  /// Returns 0 or -errno.
  intptr_t (*do_getsockname)(HANDLE socket, SocketState *state,
                             struct sockaddr *addr, socklen_t *addrlen);
  intptr_t (*do_getpeername)(HANDLE socket, SocketState *state,
                             struct sockaddr *addr, socklen_t *addrlen);

  // ─── setsockopt / getsockopt (non-SOL_SOCKET) ─────────────────────
  /// Handle protocol-level options. SOL_SOCKET options are handled by
  /// generic code. Returns 0, -errno, or -ENOPROTOOPT if not handled.
  intptr_t (*do_getsockopt)(HANDLE socket, SocketState *state, int level,
                            int optname, void *optval, socklen_t *optlen);
  intptr_t (*do_setsockopt)(HANDLE socket, SocketState *state, int level,
                            int optname, const void *optval, socklen_t optlen);
};

/// Resolve the ops table for a given address family.
/// Returns nullptr if the family is not supported (caller returns EAFNOSUPPORT).
const AddressFamilyOps *resolve_af_ops(int domain);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AF_OPS_H
