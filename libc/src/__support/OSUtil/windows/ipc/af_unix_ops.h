//===-- AF_UNIX address family operations -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// AF_UNIX-specific operations for the socket engine. Implements the
// AddressFamilyOps interface with all afunix.sys specifics: transport
// prime, SET/GET_CONTEXT, context image validation and extraction,
// endpoint address queries.
//
// All operations use the per-socket scratch region in SocketState for
// transient buffers. Work structs are defined here for each operation,
// overlaid onto scratch at call time.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AF_UNIX_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AF_UNIX_OPS_H

#include "src/__support/OSUtil/windows/ipc/af_ops.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/nt/nt_afd.h"
#include "src/__support/macros/config.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

//===----------------------------------------------------------------------===//
// AF_UNIX constants
//===----------------------------------------------------------------------===//

inline constexpr int UNIX_ADDRESS_FAMILY = 1;
inline constexpr int UNIX_STREAM_SOCKET_TYPE = 1;
inline constexpr size_t UNIX_SUN_PATH_LEN = 108;
inline constexpr socklen_t SIZEOF_SOCKADDR_UN = 110; // 2 (family) + 108 (path)
static_assert(SIZEOF_SOCKADDR_UN == sizeof(uint16_t) + UNIX_SUN_PATH_LEN,
              "SIZEOF_SOCKADDR_UN must equal sa_family_t + sun_path");

inline constexpr uint32_t UNIX_CONTEXT_HEADER_LEN = sizeof(AFD_CONTEXT_HEADER);
inline constexpr uint32_t UNIX_CONTEXT_ADDRESS_STORAGE_LEN =
    AFD_UNIX_CONTEXT_ADDRESS_SLOT_LENGTH;
inline constexpr uint32_t UNIX_SELECTOR_BLOB_LEN =
    AFD_UNIX_CONTEXT_SELECTOR_LENGTH;
inline constexpr uint32_t UNIX_CONTEXT_SELECTOR_OFFSET_VAL =
    AFD_UNIX_CONTEXT_SELECTOR_OFFSET;
// Read-back buffer size for IOCTL_AFD_GET_CONTEXT. Sized to exactly our own
// producer shape: the 0x160 AFD_UNIX_CONTEXT_IMAGE prefix plus a 4-byte
// kernel-minimum selector. Our sockets only ever carry context that we
// wrote ourselves via afd_set_unix_context — the kernel never synthesises
// one, and no foreign producer (Winsock/wshunix) runs in this libc's
// address space. WSHUNIX_SOCKET_CONTEXT_SNAPSHOT_HEADER remains declared
// in nt_afd.h for archival reference; if a future path ever needs to
// decode a foreign blob, widen this back to
// `AFD_UNIX_CONTEXT_SELECTOR_OFFSET + 0x1000` and restore the dual-shape
// branch in context_is_plausible.
inline constexpr uint32_t UNIX_CONTEXT_QUERY_STORAGE_LEN =
    AFD_UNIX_CONTEXT_SELECTOR_OFFSET + AFD_UNIX_CONTEXT_MIN_SELECTOR_LENGTH;

//===----------------------------------------------------------------------===//
// Work structs — overlaid onto SocketState scratch region
//
// Each struct is used by exactly one operation at a time. The scratch
// region is ~7.6 KB; static_asserts verify each fits.
//===----------------------------------------------------------------------===//

// NT path buffer size for AF_UNIX socket operations. Must fit \??\<CWD>\<path>
// where socket paths are at most 108 bytes and CWDs are typically under 260.
inline constexpr size_t UNIX_SOCKET_NT_PATH_WCHARS = 384;

/// Scratch layout for af_unix_do_bind.
struct UnixBindWork {
  // Open-state context image (sent before first bind ioctl).
  AFD_UNIX_CONTEXT_IMAGE open_ctx;
  IO_STATUS_BLOCK open_iosb;

  // Transport prime payload, NT path buffer, and null-terminated POSIX path.
  alignas(8) uint8_t
      transport_payload[sizeof(HANDLE) + sizeof(WCHAR) * UNIX_SOCKET_NT_PATH_WCHARS];
  WCHAR nt_path[UNIX_SOCKET_NT_PATH_WCHARS];
  char path_z[109]; // sun_path (108) + NUL
  IO_STATUS_BLOCK prime_iosb;

  // IOCTL_AFD_BIND input/output (aliased).
  alignas(8) uint8_t bind_buf[sizeof(AFD_BIND_INFO_TL) + SIZEOF_SOCKADDR_UN];
  uint8_t bind_addr[SIZEOF_SOCKADDR_UN];
  IO_STATUS_BLOCK bind_iosb;

  // Bound-state context image (sent after bind succeeds).
  AFD_UNIX_CONTEXT_IMAGE bound_ctx;
  IO_STATUS_BLOCK bound_iosb;
};

/// Scratch layout for af_unix_do_connect.
struct UnixConnectWork {
  // Auto-bind workspace (used if socket is UNBOUND).
  AFD_UNIX_CONTEXT_IMAGE open_ctx;
  IO_STATUS_BLOCK open_iosb;
  uint8_t wildcard[SIZEOF_SOCKADDR_UN];
  alignas(8) uint8_t auto_bind_buf[sizeof(AFD_BIND_INFO_TL) +
                                   SIZEOF_SOCKADDR_UN];
  IO_STATUS_BLOCK auto_bind_iosb;
  AFD_UNIX_CONTEXT_IMAGE bound_ctx;
  IO_STATUS_BLOCK bound_iosb;

  // Transport prime workspace.
  alignas(8) uint8_t
      transport_payload[sizeof(HANDLE) + sizeof(WCHAR) * UNIX_SOCKET_NT_PATH_WCHARS];
  WCHAR nt_path[UNIX_SOCKET_NT_PATH_WCHARS];
  char path_z[109]; // sun_path (108) + NUL
  IO_STATUS_BLOCK prime_iosb;

  // Connect payload.
  alignas(8) uint8_t connect_buf[sizeof(AFD_CONNECT_JOIN_INFO_TL) +
                                 SIZEOF_SOCKADDR_UN];
  uint8_t remote_addr[SIZEOF_SOCKADDR_UN];

  // Connected-state context image (sent after connect succeeds).
  AFD_UNIX_CONTEXT_IMAGE connected_ctx;
  IO_STATUS_BLOCK connected_iosb;
};

/// Scratch layout for af_unix_setup_accepted.
struct UnixAcceptWork {
  AFD_ACCEPT_INFO accept_info;
  IO_STATUS_BLOCK accept_iosb;

  // Nonblocking mode setup.
  AFD_INFORMATION nb_info;
  IO_STATUS_BLOCK nb_iosb;

  // Post-accept context image.
  AFD_UNIX_CONTEXT_IMAGE ctx;
  IO_STATUS_BLOCK ctx_iosb;
};

/// Scratch layout for af_unix_post_listen.
struct UnixListenWork {
  AFD_UNIX_CONTEXT_IMAGE ctx;
  IO_STATUS_BLOCK iosb;
};

/// Scratch layout for af_unix_do_getsockname / af_unix_do_getpeername.
struct UnixNameQueryWork {
  // Endpoint ioctl output.
  uint8_t endpoint_out[SIZEOF_SOCKADDR_UN];
  IO_STATUS_BLOCK endpoint_iosb;

  // GET_CONTEXT decode buffer — the largest single allocation.
  alignas(AFD_CONTEXT_HEADER)
      uint8_t context_storage[UNIX_CONTEXT_QUERY_STORAGE_LEN];
  IO_STATUS_BLOCK context_iosb;

  // Extracted address output.
  uint8_t extracted_addr[SIZEOF_SOCKADDR_UN];
};

/// Scratch layout for af_unix_do_getsockopt (SO_PEERCRED).
struct UnixSockoptWork {
  IO_STATUS_BLOCK iosb;
  ULONG pid;
};

// Verify all Work structs fit in the scratch region.
static_assert(sizeof(UnixBindWork) <= SOCKET_SCRATCH_CAPACITY,
              "UnixBindWork exceeds scratch capacity");
static_assert(sizeof(UnixConnectWork) <= SOCKET_SCRATCH_CAPACITY,
              "UnixConnectWork exceeds scratch capacity");
static_assert(sizeof(UnixAcceptWork) <= SOCKET_SCRATCH_CAPACITY,
              "UnixAcceptWork exceeds scratch capacity");
static_assert(sizeof(UnixListenWork) <= SOCKET_SCRATCH_CAPACITY,
              "UnixListenWork exceeds scratch capacity");
static_assert(sizeof(UnixNameQueryWork) <= SOCKET_SCRATCH_CAPACITY,
              "UnixNameQueryWork exceeds scratch capacity");
static_assert(sizeof(UnixSockoptWork) <= SOCKET_SCRATCH_CAPACITY,
              "UnixSockoptWork exceeds scratch capacity");

//===----------------------------------------------------------------------===//
// AF_UNIX ops table — the single static instance
//===----------------------------------------------------------------------===//

extern const AddressFamilyOps af_unix_ops;

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AF_UNIX_OPS_H
