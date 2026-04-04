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
// cached addresses, shutdown direction, and an inline scratch region for
// all transient operation buffers.
//
// Allocated as a single-page (4 KB) arena via page_alloc. The first ~620
// bytes hold the struct fields; the remaining ~3.4 KB is scratch workspace
// used by lifecycle and query operations. The socket HANDLE is bound to
// the reactor IOCP at socket() / accept() time; every AFD IRP posts its
// completion to the reactor and a per-call IoWaiter (stack or embedded)
// serialises the caller's park and wake. There is no per-socket NT Event.
//
// Scratch-region serialisation: only one lifecycle operation runs per
// socket at a time. Concurrent calls on the same socket are governed by
// phase CAS transitions (UNBOUND → BOUND, BOUND → CONNECTING, etc.).
// Data-path operations (send / recv / sendmsg / recvmsg) do NOT use
// scratch — they stack-allocate their waiter and inline their WSABUF
// slots via wsabuf_inline.
//
// Shared by dup'd fds (refcount via OFD).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SOCKET_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SOCKET_STATE_H

#include "hdr/types/socklen_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ipc/af_ops.h"
#include "src/__support/OSUtil/windows/ipc/afd_io_completion.h"
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
  static constexpr size_t ALLOC_SIZE = 4096; // 1 page

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

  /// SOL_SOCKET boolean option flags — set by setsockopt, consumed by
  /// bind/connect and the AF-specific do_setsockopt forwarders.
  ///
  /// Bit layout is part of the internal ABI between the generic engine
  /// and af_*_ops implementations; it is not exposed to user code.
  /// Values are powers of two so the field can hold all toggles
  /// simultaneously.
  ///
  /// Bit 0: OPT_REUSEADDR    — bind uses AfdBindReuseAddress (all AFs)
  /// Bit 1: OPT_KEEPALIVE    — AF_INET forwards to transport; no-op for
  ///                           AF_UNIX
  /// Bit 2: OPT_BROADCAST    — AF_INET UDP only; ignored elsewhere
  /// Bit 3: OPT_OOBINLINE    — AF_INET TCP only; ignored elsewhere
  /// Bit 4: OPT_DONTROUTE    — AF_INET/INET6 only; ignored elsewhere
  /// Bit 5: OPT_LINGER       — enable-flag for SO_LINGER; the duration
  ///                           lives in `linger_seconds`
  static constexpr uint32_t OPT_REUSEADDR = 1u << 0;
  static constexpr uint32_t OPT_KEEPALIVE = 1u << 1;
  static constexpr uint32_t OPT_BROADCAST = 1u << 2;
  static constexpr uint32_t OPT_OOBINLINE = 1u << 3;
  static constexpr uint32_t OPT_DONTROUTE = 1u << 4;
  static constexpr uint32_t OPT_LINGER    = 1u << 5;
  cpp::Atomic<uint32_t> socket_options;

  /// SO_LINGER duration, in seconds. Meaningful only when OPT_LINGER is
  /// set in socket_options. POSIX `struct linger::l_linger` fits easily
  /// in 32 bits (practical values are ≤ 3600). The close-path actor
  /// (future) reads this together with the OPT_LINGER bit to decide
  /// between graceful-drain and RST.
  cpp::Atomic<uint32_t> linger_seconds;

  /// SO_RCVTIMEO / SO_SNDTIMEO in microseconds. Zero means "no timeout"
  /// (blocking forever — the POSIX default). Enforcement lives in the
  /// per-AF data plane and is wired up alongside AF_INET; the fields
  /// are stored here so that getsockopt returns the last setsockopt
  /// value even when enforcement is not yet implemented.
  cpp::Atomic<int64_t> rcvtimeo_us;
  cpp::Atomic<int64_t> sndtimeo_us;

  /// Persistent waiter + IOSB for nonblocking connect(). The IRP outlives
  /// the originating connect() call, so its completion record must live
  /// in the SocketState rather than on the caller's stack.
  ///
  /// Synchronization: the phase CAS (BOUND/UNBOUND → CONNECTING)
  /// serializes access — only the thread that wins the CAS may initiate
  /// the connect and touch these fields. Reaping is serialized by
  /// socket_probe_connect_completion(), which polls waiter.done
  /// non-blockingly and only reads the final status after CONNECTING →
  /// CONNECTED or CONNECTING → BOUND has published.
  ///
  /// `connect_waiter.tlw` is null: no thread parks on this waiter — the
  /// probe poll reads `done` directly. The router fires and publishes
  /// status regardless.
  afd_io::IoWaiter connect_waiter;
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

// Verify the scratch region is large enough for all operations. The
// per-op Work structs in af_*_ops.h assert their own fit individually;
// this floor exists to catch silent SocketState growth that would shrink
// the scratch below the largest Work struct (currently UnixConnectWork at
// ~3.3 KB). Raise if a new hot-path struct crosses this threshold.
static_assert(SOCKET_SCRATCH_CAPACITY >= 3456,
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
  if (s)
    page_free(s);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SOCKET_STATE_H
