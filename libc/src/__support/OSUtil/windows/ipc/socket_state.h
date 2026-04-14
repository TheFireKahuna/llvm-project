//===-- Socket state for AFD endpoints ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-socket state for AFD endpoints. Tracks the socket lifecycle
// (unbound → bound → listening/connected), address family ops table,
// cached addresses, shutdown direction, a per-socket event handle for
// synchronous AFD ioctls, and an inline scratch region for all transient
// operation buffers.
//
// Allocated as a 2-page (8 KB) arena via page_alloc. The first ~400 bytes
// hold the struct fields; the remaining ~7.6 KB is scratch workspace used
// by lifecycle and query operations. Only one operation runs per socket at
// a time (serialized by ioctl_event), so the scratch is safely reused.
//
// Shared by dup'd fds (refcount via OFD).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SOCKET_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SOCKET_STATE_H

#include "hdr/types/socklen_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ipc/af_ops.h"
#include "src/__support/OSUtil/windows/nt/nt_afd.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

//===----------------------------------------------------------------------===//
// Socket lifecycle phase
//===----------------------------------------------------------------------===//

enum class SocketPhase : uint8_t {
  UNBOUND,
  BOUND,
  CONNECTING,
  LISTENING,
  CONNECTED,
};

//===----------------------------------------------------------------------===//
// Per-socket state — 2-page arena
//===----------------------------------------------------------------------===//

struct SocketState {
  // ─── Allocation geometry ─────────────────────────────────────────
  static constexpr size_t ALLOC_SIZE = 8192; // 2 pages

  // ─── Immutable after creation ────────────────────────────────────
  const AddressFamilyOps *ops;
  int domain;
  int type;
  int protocol;

  // ─── Lifecycle tracking ──────────────────────────────────────────
  cpp::Atomic<SocketPhase> phase;

  /// Shutdown direction bits: SHUT_RD=1, SHUT_WR=2.
  cpp::Atomic<uint8_t> shutdown_flags;

  /// Sticky socket error surfaced through SO_ERROR.
  cpp::Atomic<int> socket_error;

  /// SOL_SOCKET option flags — set by setsockopt, consumed by bind/connect.
  /// Bit 0: SO_REUSEADDR  — bind uses AfdBindReuseAddress
  /// Bit 1: SO_KEEPALIVE  — forwarded to transport for AF_INET (no-op AF_UNIX)
  static constexpr uint8_t OPT_REUSEADDR = 1u << 0;
  static constexpr uint8_t OPT_KEEPALIVE = 1u << 1;
  cpp::Atomic<uint8_t> socket_options;

  /// Per-socket event for synchronous AFD ioctls (afd_ioctl helper).
  /// Created once at socket(), closed at release.
  HANDLE ioctl_event;

  /// Persistent IOSB for nonblocking connect(). The kernel writes
  /// completion status here while the socket remains in CONNECTING phase.
  ///
  /// Synchronization: the phase CAS (BOUND/UNBOUND → CONNECTING)
  /// serializes access — only the thread that wins the CAS may initiate
  /// the connect and write to this IOSB. Reaping is serialized by
  /// socket_reap_connect_if_needed() which transitions CONNECTING →
  /// CONNECTED before reading the final status.
  IO_STATUS_BLOCK connect_iosb;

  // ─── Address cache ───────────────────────────────────────────────
  // Sized for MAX_SOCKADDR_STORAGE (128 bytes) to accommodate all
  // supported address families: AF_UNIX (110), AF_INET (16),
  // AF_INET6 (28).
  uint8_t local_addr[MAX_SOCKADDR_STORAGE];
  socklen_t local_len;

  uint8_t remote_addr[MAX_SOCKADDR_STORAGE];
  socklen_t remote_len;

  // ─── Inline WSABUF slots for data-path I/O ──────────────────────
  // Used by send/recv/sendmsg/recvmsg to avoid both stack allocation
  // and page_alloc for the common case (<=16 iovecs). Lives in the
  // page-allocated arena, not scratch (concurrent sends and recvs are
  // permitted, so the single-occupancy scratch region cannot be used).
  static constexpr size_t WSABUF_INLINE_COUNT = 16;
  AFD_WSABUF wsabuf_inline[WSABUF_INLINE_COUNT];
};

//===----------------------------------------------------------------------===//
// Scratch region — lives in the tail of the 2-page allocation
//===----------------------------------------------------------------------===//

/// Offset from the start of SocketState to the scratch region, rounded
/// up to 16-byte alignment for ioctl buffer requirements.
inline constexpr size_t SOCKET_SCRATCH_OFFSET =
    (sizeof(SocketState) + 15u) & ~size_t{15};

/// Usable scratch capacity in bytes.
inline constexpr size_t SOCKET_SCRATCH_CAPACITY =
    SocketState::ALLOC_SIZE - SOCKET_SCRATCH_OFFSET;

// Verify the scratch region is large enough for all operations.
// Largest consumer: AF_UNIX GET_CONTEXT decode (~4,448 bytes).
static_assert(SOCKET_SCRATCH_CAPACITY >= 7168,
              "Scratch capacity too small — SocketState fields grew");

/// Return a pointer to the scratch workspace for this socket.
LIBC_INLINE uint8_t *socket_scratch(SocketState *s) {
  return reinterpret_cast<uint8_t *>(s) + SOCKET_SCRATCH_OFFSET;
}

//===----------------------------------------------------------------------===//
// Allocation
//===----------------------------------------------------------------------===//

LIBC_INLINE SocketState *socket_state_alloc() {
  auto *s = static_cast<SocketState *>(page_alloc(SocketState::ALLOC_SIZE));
  if (s)
    __builtin_memset(s, 0, SocketState::ALLOC_SIZE);
  return s;
}

LIBC_INLINE void socket_state_free(SocketState *s) {
  if (s) {
    if (s->ioctl_event)
      NtClose(s->ioctl_event);
    page_free(s);
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SOCKET_STATE_H
