//===-- Pending signal storage (Layer 1) --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pure data structure layer for pending signal management. No dispatch policy,
// no handler invocation — just concurrent storage and retrieval.
//
// Standard signals (1..SIGRTMIN-1): coalescing atomic bitmask.
// RT signals (SIGRTMIN..SIGRTMAX): single Vyukov MPSC inbox queue for all RT
// signal numbers. POSIX lowest-numbered-first ordering is achieved by the
// dispatch engine (Layer 3) at drain time via consumer-side per-signal
// buckets — not at insertion time.
//
// This separation keeps Layer 1 as pure storage with no ordering policy:
//   - Concurrency (multi-producer): Vyukov inbox — 1 XCHG per push.
//   - Priority ordering (POSIX lowest-first): Layer 3 consumer-side buckets.
//
// POSIX mutual cancellation: SIGCONT clears pending stop signals
// (SIGTSTP/SIGTTIN/SIGTTOU), and vice versa.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PENDING_PENDING_STORAGE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PENDING_PENDING_STORAGE_H

#include "hdr/signal_macros.h"
#include "hdr/types/sigset_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/signal/pending/rt_queue.h"
#include "src/__support/OSUtil/windows/signal/signal_constants.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/spin_wait.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Number of distinct RT signal numbers.
inline constexpr int RT_SIG_COUNT = SIGRTMAX - SIGRTMIN + 1;
static_assert(RT_SIG_COUNT > 0 && RT_SIG_COUNT <= 32,
              "RT_SIG_COUNT must fit within a uint32_t bitmap");

// Bitmask covering all RT signal bits in the pending bitmap.
// RT signals occupy bits (SIGRTMIN-1)..(SIGRTMAX-1) in the 64-bit word.
inline constexpr uint64_t RT_SIGNALS_MASK = [] {
  uint64_t mask = 0;
  for (int sig = SIGRTMIN; sig <= SIGRTMAX; ++sig)
    mask |= 1ULL << (sig - 1);
  return mask;
}();

// Bitmask covering standard signals only (1..SIGRTMIN-1).
inline constexpr uint64_t STANDARD_SIGNALS_MASK =
    SIGNAL_BITS_MASK & ~RT_SIGNALS_MASK;

// ---------------------------------------------------------------------------
// PendingSet — per-thread or process-wide pending signal storage
// ---------------------------------------------------------------------------

struct PendingSet {
  // Bitmask for all signals 1..63. Bit (sig-1) corresponds to signal sig.
  //
  // For standard signals (1..SIGRTMIN-1): the bit alone indicates pending.
  //   Setting an already-set bit is a no-op (coalescing).
  //
  // For RT signals (SIGRTMIN..SIGRTMAX): the bit indicates "at least one
  //   entry exists in the inbox for this signal number". The bit is set when
  //   an entry is pushed, cleared by the dispatch engine when it has drained
  //   all entries for that signal.
  // Named "standard" for historical reasons but serves as the unified
  // presence bitmap for ALL signal types (both standard and RT).
  cpp::Atomic<uint64_t> standard{0};

  // Single Vyukov MPSC queue for ALL RT signal entries regardless of signal
  // number. Producers push here; the dispatch engine drains into per-signal
  // buckets at delivery time for POSIX lowest-numbered-first ordering.
  RtQueue inbox;

  // Initialize to empty state. Must be called before first use.
  LIBC_INLINE void init() {
    standard.store(0, cpp::MemoryOrder::RELAXED);
    inbox.init();
  }
};

// PendingSet: 8 (bitmap) + 48 (inbox) = 56 bytes.
static_assert(sizeof(PendingSet) <= 64,
              "PendingSet grew beyond expected size — review layout");

// ---------------------------------------------------------------------------
// Pend API — used by Layer 2 transports
// ---------------------------------------------------------------------------

namespace signal_pending {

// Pend a standard signal (1..SIGRTMIN-1). Thread-safe, lock-free.
//
// Returns true if the bit was newly set (first pending instance).
// Returns false if the signal was already pending (coalesced).
//
// Callers use the return value to skip redundant APC/alert: if the signal
// was already pending, there's no need to wake the target again.
//
// Also handles POSIX mutual cancellation (SIGCONT ↔ stop signals).
LIBC_INLINE bool pend_standard(PendingSet &ps, int signum) {
  uint64_t bit = 1ULL << (signum - 1);

  // POSIX mutual cancellation: SIGCONT clears stop signals, and vice versa.
  // Compute the mask of bits to clear (0 if no cancellation applies).
  uint64_t cancel_mask = 0;
  if (signum == SIGCONT)
    cancel_mask = STOP_SIGNALS_MASK; // SIGCONT clears SIGTSTP/SIGTTIN/SIGTTOU
  else if (STOP_SIGNALS_MASK & bit)
    cancel_mask = SIGCONT_MASK; // Stop signal clears SIGCONT

  // Single CAS loop: atomically set the pending bit AND clear cancelled
  // signals in one operation. This eliminates the intermediate state where
  // cancelled signals are cleared but the new signal isn't yet visible.
  uint64_t old_val = ps.standard.load(cpp::MemoryOrder::RELAXED);
  for (;;) {
    uint64_t new_val = (old_val & ~cancel_mask) | bit;
    if (ps.standard.compare_exchange_weak(old_val, new_val,
                                          cpp::MemoryOrder::RELEASE,
                                          cpp::MemoryOrder::RELAXED))
      return (old_val & bit) == 0; // True if bit was newly set.
    spin_wait::relax_processor();
  }
}

// Pend an RT signal. Pushes to the unified inbox — no per-signal routing.
// Sets the presence bit for this signal number in the standard bitmap.
//
// The entry must be allocated from sigqueue_pool. The caller must have
// initialized si_signo, si_code, value, pid before calling this.
//
// Thread-safe, lock-free (XCHG on inbox tail + atomic OR on bitmap).
LIBC_INLINE void pend_rt(PendingSet &ps, SigqueueEntry *entry) {
  // Push to unified inbox — one XCHG regardless of signal number.
  ps.inbox.push(entry);

  // Set the presence bit. RELEASE ensures the inbox push is visible
  // before the drain loop sees the bit.
  uint64_t bit = 1ULL << (entry->si_signo - 1);
  ps.standard.fetch_or(bit, cpp::MemoryOrder::RELEASE);
}

// Drain the next deliverable standard signal from the pending set.
//
// Scans for the lowest-numbered unblocked standard signal (1..SIGRTMIN-1).
// Clears the pending bit atomically and returns the signal number.
// Returns 0 if no standard signals are pending and unblocked.
//
// RT signal drain is NOT here — it is owned by the dispatch engine (Layer 3),
// which absorbs the inbox into consumer-side per-signal buckets and delivers
// lowest-numbered first via __builtin_ctz(bitmap).
//
// Single-consumer for per-thread sets; multiple consumers possible for
// process-wide set (CAS loop handles contention).
LIBC_INLINE int drain_next_standard(PendingSet &ps, uint64_t blocked_bits) {
  // Load pending bits with ACQUIRE to synchronize with RELEASE in pend_*.
  uint64_t pending = ps.standard.load(cpp::MemoryOrder::ACQUIRE);

  // Only consider standard signals, exclude blocked.
  uint64_t deliverable = pending & ~blocked_bits & STANDARD_SIGNALS_MASK;

  if (deliverable == 0)
    return 0;

  // Find lowest-numbered pending unblocked standard signal.
  int bit_idx = __builtin_ctzll(deliverable);
  uint64_t bit = 1ULL << bit_idx;

  // CAS loop to clear the bit atomically. On failure, `expected` is
  // refreshed by the CAS. We must recompute the lowest deliverable signal
  // because the pending set may have changed (new signals arrived, or
  // another consumer claimed ours on the process-wide set).
  uint64_t expected = pending;
  while (!ps.standard.compare_exchange_weak(
      expected, expected & ~bit, cpp::MemoryOrder::ACQ_REL,
      cpp::MemoryOrder::ACQUIRE)) {
    // Recompute deliverable set from the refreshed expected value.
    deliverable = expected & ~blocked_bits & STANDARD_SIGNALS_MASK;
    if (deliverable == 0)
      return 0; // Nothing deliverable anymore.
    bit_idx = __builtin_ctzll(deliverable);
    bit = 1ULL << bit_idx;
  }

  return bit_idx + 1;
}

// Check if any unblocked signals are pending. Fast-path for dispatch
// boundary: if nothing is pending, skip the drain loop entirely.
//
// Safe to call from any thread (used for process-wide pending set).
LIBC_INLINE bool has_pending(PendingSet &ps, const sigset_t &blocked) {
  uint64_t pending = ps.standard.load(cpp::MemoryOrder::ACQUIRE);
  uint64_t blocked_bits = sigset_to_bits(blocked);
  return (pending & ~blocked_bits & SIGNAL_BITS_MASK) != 0;
}

// Clear all pending instances of a specific signal.
// Used by sigaction(SIG_IGN) and exec to discard pending signals.
// For RT signals, drains the inbox, frees matching entries, re-pushes others.
//
// Producer-safe: concurrent pend_rt() calls are tolerated. An entry that
// arrives during clear will be properly tracked (pend_rt re-sets its own
// bit after our clear). The dispatch engine discards it per the new
// disposition. At worst, a stale presence bit remains — the dispatch
// engine already handles stale RT bits with one extra empty drain pass.
//
// CONSUMER CONSTRAINT: try_pop() is single-consumer. clear_signal must
// NOT be called concurrently with dispatch_pending() on the same
// PendingSet. For per-thread sets, this is naturally satisfied (only the
// owning thread dispatches). For the process-wide set, the caller must
// ensure no dispatch is in progress (e.g., by holding the signal lock or
// by calling from a context where dispatch is quiesced such as exec/fork).
void clear_signal(PendingSet &ps, int signum);

// Clear all pending signals. Used during exec and fork reinit.
// Drains and frees all inbox entries.
// Same single-consumer constraint as clear_signal.
void clear_all(PendingSet &ps);

} // namespace signal_pending
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PENDING_PENDING_STORAGE_H
