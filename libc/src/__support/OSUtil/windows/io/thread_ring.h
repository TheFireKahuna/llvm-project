//===-- Per-thread IoRing for Windows ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-thread IoRing — one ring shared by all fds on a thread.
//
// The ring pointer lives in ThreadLifecycle::thread_ring, accessed via
// get_current_lifecycle(). No dedicated TEB slot — the lifecycle root
// is the single TEB entry point for all per-thread state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_THREAD_RING_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_THREAD_RING_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_ioring_ops.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace ioring {

// needs_wake tri-state values (stored in RingState::needs_wake).
inline constexpr ULONG WAKE_NONE = 0;   // No thread waiting.
inline constexpr ULONG WAKE_FUTEX = 1;  // Thread parked on futex.
inline constexpr ULONG WAKE_EVENT = 2;  // Thread in alertable wait on wait_event.

struct ThreadRing {
  RingState ring;
  HANDLE event;           // IoRing completion event — reactor WCP consumer.
  HANDLE wait_event;      // Separate alertable-wait event — signaled by waker
                          // when needs_wake == WAKE_EVENT. Not watched by the
                          // reactor WCP, so no double-consumer race with the
                          // IoRing completion event.
  uint32_t generation{0}; // Monotonic tag generation — incremented per op.
                          // Enables stale CQE rejection after EINTR cancel.

  // Reactor registration for the waker bridge. The completion event is
  // watched by the reactor; when it fires (CQ empty→non-empty), the drain
  // thread calls futex_addr::wake(&cq->Tail, 1) to unpark any I/O thread
  // blocked in ioring_submit(). One-shot — re-armed in the callback.
  internal::reactor::ReactorToken reactor_token{
      internal::reactor::INVALID_TOKEN};

  // --- Registered buffer pool (lazy-init) ---
  //
  // Simple committed region registered as a single IORING_BUFFER_INFO
  // entry — one MDL pre-pinned at registration time. Allocated lazily on
  // first O_DIRECT I/O via ensure_reg_buf(), not at ThreadRing creation.
  //
  // Bounce-buffer pattern: kernel DMAs into the registered region via
  // buf_offset in the SQE, then userspace memcpy to the caller's buffer.
  // The memcpy cost (~2-5K cycles for 64K) is dwarfed by MDL savings
  // (~726K cycles for unbuffered I/O — research D4).
  //
  // For single-op pread/pwrite: head resets to 0 each op (degenerate).
  // For pipeline batch I/O: head advances per SQE, tail reclaims on CQE
  // drain. Allocations that would wrap past reg_buf_size reset head to 0
  // (kernel buf_offset must stay within [0, reg_buf_size)).
  //
  // No file handle registration — research Section 6 shows ~0% benefit
  // for cached I/O. Raw HANDLEs + IORING_OP_FLAG_REGISTERED_BUFFER only.
  static constexpr uint32_t REG_BUF_SIZE = 262144; // 256KB physical

  void *reg_buf_base{nullptr};    // NtAllocateVirtualMemory base
  uint32_t reg_buf_head{0};       // Allocation cursor [0, REG_BUF_SIZE)
  uint32_t reg_buf_tail{0};       // Reclaim cursor [0, REG_BUF_SIZE)
  bool reg_buf_ready{false};      // Registration succeeded

  /// Advance generation and return the new value.
  /// Single-threaded (per-thread ring) — no atomic needed.
  [[nodiscard]] LIBC_INLINE uint32_t next_generation() { return ++generation; }

  /// Available space in the registered buffer ring.
  LIBC_INLINE uint32_t reg_buf_avail() const {
    return REG_BUF_SIZE - (reg_buf_head - reg_buf_tail);
  }

  /// Acquire a region from the registered buffer ring for a single I/O op.
  /// Returns the buf_offset for the SQE, or -1 if insufficient space.
  /// The kernel's buf_offset must be in [0, REG_BUF_SIZE), so if the
  /// allocation would wrap, we reset head to 0 (wasting tail space).
  LIBC_INLINE int32_t reg_buf_acquire(uint32_t len) {
    if (!reg_buf_ready || len > REG_BUF_SIZE)
      return -1;
    // For single-op path: head == tail (ring empty), fast reset.
    if (reg_buf_head == reg_buf_tail) {
      reg_buf_head = 0;
      reg_buf_tail = 0;
    }
    // Would wrap past the registered range?
    if (reg_buf_head + len > REG_BUF_SIZE) {
      // Only reset if all prior data has been reclaimed.
      if (reg_buf_tail != reg_buf_head)
        return -1; // Outstanding ops occupy the ring — can't reset.
      reg_buf_head = 0;
      reg_buf_tail = 0;
    }
    uint32_t offset = reg_buf_head;
    reg_buf_head += len;
    return static_cast<int32_t>(offset);
  }

  /// Reclaim a region after the CQE has been consumed and data copied out.
  LIBC_INLINE void reg_buf_release(uint32_t len) {
    reg_buf_tail += len;
  }

  /// Pointer into the registered buffer at the given offset.
  LIBC_INLINE void *reg_buf_ptr(uint32_t offset) const {
    return static_cast<char *>(reg_buf_base) + offset;
  }

  /// Lazy-init the registered buffer. Called on first O_DIRECT I/O.
  /// Returns true if the buffer is ready (already or newly initialized).
  /// Out-of-line to keep the hot path small.
  bool ensure_reg_buf();
};

inline constexpr ULONG THREAD_RING_SQ_SIZE = 16;
inline constexpr ULONG THREAD_RING_CQ_SIZE = 32;

// Maximum ops per BatchEngine batch — decoupled from ring SQ capacity.
// The pipeline pushes up to SQ_SIZE SQEs at once, then refills as CQEs
// drain. Tracking arrays in BatchEngine are sized to this, not the ring.
inline constexpr uint32_t BATCH_MAX_OPS = 64;

void thread_ring_init();
void thread_ring_shutdown();

// Close and free a ThreadRing. Called from lifecycle_cleanup.
void thread_ring_destroy(void *ptr);

// Allocate a ring on first I/O. Out-of-line slow path.
ThreadRing *thread_ring_create();

// Hot path: read from lifecycle root. One pointer chase from TEB.
LIBC_INLINE ThreadRing *get_thread_ring() {
  auto *lc = get_current_lifecycle();
  if (LIBC_UNLIKELY(!lc))
    return nullptr;

  auto *tr = static_cast<ThreadRing *>(lc->thread_ring);
  if (LIBC_LIKELY(tr != nullptr))
    return tr;

  return thread_ring_create();
}

} // namespace ioring
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_THREAD_RING_H
