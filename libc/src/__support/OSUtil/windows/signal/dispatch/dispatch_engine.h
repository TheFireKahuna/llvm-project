//===-- Dispatch engine (Layer 3) ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Signal dispatch engine. Drains pending signals (Layer 1), applies masks,
// invokes handlers, and manages the dispatch state machine.
//
// Key components:
//   - Dispatch state machine: IDLE → TRIGGERED → DRAINING
//   - RtBuckets: consumer-side per-signal sort for POSIX lowest-first RT
//   - RestartState: push/pop SA_RESTART stack per nesting level
//   - Alt-stack space check with real SP measurement 
//
// Replaces dispatch_fwd.h with the full API. Transport layers (Layer 2)
// call trigger() and trigger_any_thread(). I/O paths call
// should_restart_syscall(). All dispatch boundary code calls
// dispatch_pending().
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_DISPATCH_ENGINE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_DISPATCH_ENGINE_H

#include "hdr/signal_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/OSUtil/windows/signal/pending/rt_queue.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// Forward declarations.
struct ThreadSignalState;

ThreadSignalState *get_thread_state_noinit();

// ---------------------------------------------------------------------------
// Dispatch state machine constants
// ---------------------------------------------------------------------------

enum DispatchState : uint8_t {
  DISPATCH_STATE_IDLE = 0,      // No signals pending (or unknown)
  DISPATCH_STATE_TRIGGERED = 1, // Transport pended signal, not yet claimed
  DISPATCH_STATE_DRAINING = 2,  // dispatch_pending() actively draining
};

// ---------------------------------------------------------------------------
// RtBuckets — consumer-side per-signal sort structure
// ---------------------------------------------------------------------------
//
// Stack-local. Only exists during dispatch_pending(). Zero persistent cost.
// Absorbs entries from the single Vyukov inbox and sorts them into per-signal
// buckets for POSIX lowest-numbered-first delivery.

struct RtBuckets {
  SigqueueEntry *heads[RT_SIG_COUNT] = {};
  SigqueueEntry *tails[RT_SIG_COUNT] = {};
  uint32_t bitmap = 0; // Which buckets are non-empty (30 bits)

  // Drain the inbox into per-signal buckets. Preserves FIFO per signal.
  LIBC_INLINE void absorb(RtQueue &inbox) {
    while (SigqueueEntry *e = inbox.try_pop()) {
      int idx = e->si_signo - SIGRTMIN;
      e->next.store(nullptr, cpp::MemoryOrder::RELAXED);
      if (!heads[idx]) {
        heads[idx] = e;
        bitmap |= (1u << idx);
      } else {
        tails[idx]->next.store(e, cpp::MemoryOrder::RELAXED);
      }
      tails[idx] = e;
    }
  }

  // Pop the lowest-numbered unblocked RT signal entry.
  // blocked_rt_mask: bit 0 = SIGRTMIN, bit 29 = SIGRTMAX.
  //
  // Single-consumer: both absorb() and pop_lowest() run in the same
  // dispatch thread. RELAXED is correct — same-thread sequencing
  // guarantees visibility of the stores in absorb().
  LIBC_INLINE SigqueueEntry *pop_lowest(uint32_t blocked_rt_mask) {
    uint32_t deliverable = bitmap & ~blocked_rt_mask;
    if (!deliverable)
      return nullptr;

    int idx = __builtin_ctz(deliverable);
    SigqueueEntry *e = heads[idx];
    SigqueueEntry *next = e->next.load(cpp::MemoryOrder::RELAXED);
    heads[idx] = next;
    if (!next) {
      tails[idx] = nullptr;
      bitmap &= ~(1u << idx);
    }
    return e;
  }

  // Push all remaining entries back into the pending set's inbox.
  // Called when dispatch_pending exits with blocked RT entries still in
  // the buckets. Without this, absorbed-but-blocked entries are leaked
  // (removed from inbox, never re-pushed), causing the bitmap bit to
  // remain set with no inbox entry — an infinite loop on next dispatch.
  LIBC_INLINE void repush_remaining(PendingSet &ps) {
    while (bitmap) {
      int idx = __builtin_ctz(bitmap);
      while (SigqueueEntry *e = heads[idx]) {
        SigqueueEntry *next = e->next.load(cpp::MemoryOrder::RELAXED);
        heads[idx] = next;
        signal_pending::pend_rt(ps, e);
      }
      tails[idx] = nullptr;
      bitmap &= ~(1u << idx);
    }
  }

  // Check if any unblocked RT signals are in the buckets.
  LIBC_INLINE bool has_deliverable(uint32_t blocked_rt_mask) const {
    return (bitmap & ~blocked_rt_mask) != 0;
  }
};

// ---------------------------------------------------------------------------
// Public dispatch API
// ---------------------------------------------------------------------------

namespace signal_dispatch {

// Convert the RT portion of a blocked sigset to a 30-bit bucket mask.
// Bit 0 = SIGRTMIN, bit 29 = SIGRTMAX.
LIBC_INLINE uint32_t blocked_to_rt_mask(uint64_t blocked_bits) {
  static_assert(SIGRTMIN >= 1, "SIGRTMIN must be at least 1 for bit shift");
  static_assert(RT_SIG_COUNT <= 30,
                "RtBuckets bitmap is uint32_t; RT_SIG_COUNT must be <= 30");
  return static_cast<uint32_t>(blocked_bits >> (SIGRTMIN - 1)) &
         ((1u << RT_SIG_COUNT) - 1);
}

// Trigger dispatch on a specific thread. Called by transports after pend().
// CAS IDLE → TRIGGERED. If already TRIGGERED or DRAINING, no-op — the
// pending bit is already set and the drain loop will see it.
void trigger(ThreadSignalState *state);

// Trigger dispatch on any suitable thread (process-directed signals).
// Tries preferred_thread first, falls back to registry walk.
void trigger_any_thread();

// Main dispatch entry point. Called at dispatch boundaries:
//   - After APC wake in alertable waits
//   - After should_restart_syscall() check
//   - At cancellation points
//   - Explicitly from raise()/kill(getpid())
//
// Owns the full dispatch loop:
//   Phase 1: Absorb RT entries from inbox into RtBuckets
//   Phase 2: Deliver standard signals (lowest first)
//   Phase 3: Deliver RT signals (lowest-numbered first, FIFO within)
//   Phase 4: Re-check for new arrivals
void dispatch_pending(ThreadSignalState *state);

// SA_RESTART query for I/O paths. Calls dispatch_pending() first to
// process any signals that arrived via APC, then checks the restart stack.
bool should_restart_syscall(ThreadSignalState *state);

} // namespace signal_dispatch
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_DISPATCH_ENGINE_H
