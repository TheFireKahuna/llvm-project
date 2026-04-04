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

// Number of standard signal slots. Signals 1..SIGRTMIN-1 (32 values).
// Indexed by (signum - 1). Slot 0 is unused (signal 0 is never pended)
// but kept for power-of-two indexing.
inline constexpr int STANDARD_SIG_SLOTS = 32;

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

  // Per-signal si_code for standard signals. Indexed by (signum - 1).
  // RT signals carry their own metadata in SigqueueEntry and do not use
  // this array.
  //
  // Only si_code is stored — si_pid/si_uid are derivable at dispatch time:
  //   SI_USER/SI_TKILL → current process PID/UID (software signals)
  //   Hardware fault codes (SEGV_MAPERR etc.) → 0/0
  //
  // Atomic because concurrent producers writing the same slot (two senders
  // pending the same signum at the same time) would otherwise be a data
  // race on a plain int. Writers use RELEASE; readers (dispatch engine,
  // transfer_all) use ACQUIRE. Last-writer-wins semantics across
  // coalescing writes are preserved — POSIX permits this for standard
  // signals.
  cpp::Atomic<int> standard_si_code[STANDARD_SIG_SLOTS];

  // Per-signal si_addr for standard signals. Indexed by (signum - 1).
  // Populated by VEH transport for hardware faults:
  //   SIGSEGV / SIGBUS (from EXCEPTION_ACCESS_VIOLATION, IN_PAGE_ERROR)
  //     → faulting virtual address (ExceptionInformation[1])
  //   SIGSEGV (STACK_OVERFLOW / ARRAY_BOUNDS), SIGBUS (misalignment),
  //   SIGFPE, SIGILL, SIGTRAP
  //     → faulting instruction address (ExceptionRecord.ExceptionAddress)
  // Software-originated signals (kill, tkill, sigqueue, SIGCHLD, console)
  // leave this as 0 — POSIX leaves si_addr unspecified for non-fault
  // signals, and the siginfo union aliases it with si_pid/si_uid.
  //
  // Same atomic contract as standard_si_code: RELEASE write paired with the
  // RELEASE CAS on `standard`; ACQUIRE load after ACQUIRE on `standard` in
  // the dispatch engine.
  cpp::Atomic<uintptr_t> standard_si_addr[STANDARD_SIG_SLOTS];

  // Initialize to empty state. Must be called before first use.
  LIBC_INLINE void init() {
    standard.store(0, cpp::MemoryOrder::RELAXED);
    inbox.init();
  }
};

// PendingSet: 8 (bitmap) + 48 (inbox) + 128 (si_code) + 256 (si_addr) = 440 B.
static_assert(sizeof(PendingSet) <= 512,
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
//
// si_code and si_addr are stored in the per-signal sidecars BEFORE the
// RELEASE CAS. si_pid/si_uid are not stored — they are derivable at
// dispatch time:
//   SI_USER/SI_TKILL → current process PID/UID (always self for software)
//   Hardware codes (SEGV_MAPERR etc.) → 0/0
// si_addr is nullptr for software signals (kill/tkill/sigqueue/SIGCHLD/
// console) and the faulting VA or instruction PC for VEH-originated
// hardware faults. On coalesce (bit already set), the sidecars are
// overwritten by the latest pender — POSIX permits this for standard
// signals.
[[nodiscard]] LIBC_INLINE bool pend_standard(PendingSet &ps, int signum,
                                             int si_code,
                                             void *fault_addr = nullptr) {
  uint64_t bit = 1ULL << (signum - 1);

  // POSIX mutual cancellation: SIGCONT clears stop signals, and vice versa.
  // Compute the mask of bits to clear (0 if no cancellation applies).
  uint64_t cancel_mask = 0;
  if (signum == SIGCONT)
    cancel_mask = STOP_SIGNALS_MASK; // SIGCONT clears SIGTSTP/SIGTTIN/SIGTTOU
  else if (STOP_SIGNALS_MASK & bit)
    cancel_mask = SIGCONT_MASK; // Stop signal clears SIGCONT

  // Write si_code and si_addr with RELEASE before the RELEASE CAS. These
  // release stores from this thread are ordered with the CAS; the dispatch
  // reader's ACQUIRE load (after its ACQUIRE load on `standard`) therefore
  // sees the sidecar values paired with whichever writer's bit actually
  // landed. On concurrent coalescing writers, last-writer-wins — POSIX
  // does not specify which pender's info survives coalescing.
  ps.standard_si_code[signum - 1].store(si_code, cpp::MemoryOrder::RELEASE);
  ps.standard_si_addr[signum - 1].store(reinterpret_cast<uintptr_t>(fault_addr),
                                        cpp::MemoryOrder::RELEASE);

  // Single CAS loop: atomically set the pending bit AND clear cancelled
  // signals in one operation. This eliminates the intermediate state where
  // cancelled signals are cleared but the new signal isn't yet visible.
  // CAS loop: atomically set the pending bit AND clear cancelled signals.
  // No relax_processor — CAS failure reloads fresh data via `old_val`,
  // which is sufficient backoff for a low-contention bitmask.
  uint64_t old_val = ps.standard.load(cpp::MemoryOrder::RELAXED);
  for (;;) {
    uint64_t new_val = (old_val & ~cancel_mask) | bit;
    if (ps.standard.compare_exchange_weak(old_val, new_val,
                                          cpp::MemoryOrder::RELEASE,
                                          cpp::MemoryOrder::RELAXED))
      return (old_val & bit) == 0; // True if bit was newly set.
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
[[nodiscard]] LIBC_INLINE int drain_next_standard(PendingSet &ps, uint64_t blocked_bits) {
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

// Move every pending signal from `src` into `dst`. Used by thread-exit
// cleanup to route thread-directed signals destined for an exiting thread
// into the process-wide pending set so any surviving thread can dispatch
// them — closing the "pthread_kill to exiting thread" silent-drop window.
//
// Behavior:
//   - Standard bits on src are atomically exchanged to 0 and OR'd into
//     dst; si_code sidecars are copied BEFORE the OR so readers on dst
//     see consistent (bit, si_code) pairs.
//   - RT entries on src are popped from src.inbox and pend_rt'd into dst,
//     which publishes their presence bits in dst.standard.
//
// CONSUMER CONSTRAINT: src.inbox.try_pop() is single-consumer. The caller
// must ensure no other consumer runs on src concurrently. For thread-exit
// cleanup this is naturally satisfied — the owning thread has stopped
// dispatching by the time deregister_thread_state runs.
//
// Producer-safe: concurrent senders may still pend to src during transfer.
// The standard-bit exchange captures anything pended before it; anything
// pended between the exchange and the caller's owner_tid clear is caught
// by a follow-up transfer after the clear. Signals pended by senders who
// observed owner_tid==0 take the sender-side ESRCH path and are not
// transferred here.
void transfer_all(PendingSet &src, PendingSet &dst);

} // namespace signal_pending
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PENDING_PENDING_STORAGE_H
