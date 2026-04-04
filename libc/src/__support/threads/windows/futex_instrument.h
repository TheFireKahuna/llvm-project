//===--- Futex/wait_slot instrumentation (temporary, bench-only) -*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Process-global atomic counters + per-worker phase words used by
// futex_bench to prove closure of the Futex/wait_slot state machine
// (every push pairs with pop/timeout/drain, every handoff publish pairs
// with a wait() returning 1, etc.).
//
// Design constraints driven by the bug under investigation (8T/16T
// mutex hang in Phase [K]):
//
//   1. Must not mask the hang via timing perturbation. Each bump is a
//      single RELAXED fetch_add on its OWN cache line — no false
//      sharing, no cross-counter bouncing, no fences beyond the RMW.
//      Perf cost per bump on Zen/TSO: ~20 cycles amortized per core.
//
//   2. Must not introduce its own hang. No locks, no blocking, no
//      syscalls on hot paths. Phase words are plain `store(RELAXED)`
//      writes through a per-thread pointer; nullptr when not set.
//
//   3. Instrumentation is a compile-time on/off flag
//      (LIBC_FUTEX_INSTRUMENT). When unset, bump() and set_phase()
//      collapse to empty inline functions (zero code emitted). The
//      bench and wait_slot.cpp define the macro to 1; production
//      pthread_mutex / cnd_var TUs leave it undefined so their inline
//      copies of Futex::wait have no bumps.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_INSTRUMENT_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_INSTRUMENT_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace futex_instrument {

// ---------------------------------------------------------------------------
// Counter IDs — one per decision point. Ordered by subsystem so the dump
// reads top-to-bottom along the code path.
// ---------------------------------------------------------------------------
enum Counter : uint32_t {
  // --- Bench wrapper (per-iter) ---
  CTR_BENCH_LOCK_CAS,               // CAS 0→1 acquired uncontended
  CTR_BENCH_LOCK_HANDOFF,           // wait() returned 1 → acquired via handoff
  CTR_BENCH_UNLOCK,                 // unlock_notify call (one per iter)

  // --- Futex::wait entry / fast paths ---
  CTR_WAIT_ENTRIES,                 // every call
  CTR_WAIT_FAST_VAL_CHANGED,        // value already changed at entry
  CTR_WAIT_PHASE1_HW_EXIT,          // hw-monitored spin saw change
  CTR_WAIT_PHASE15_EXPIRED,         // timeout already expired
  CTR_WAIT_PHASE175_RECLAIMED,      // stale-primary-slot self-reclaim fired
  CTR_WAIT_PHASE2_POOL_ENOMEM,      // slot alloc failed
  CTR_WAIT_PHASE2_VAL_CHANGED_CAS,  // CAS-64 push saw value move
  CTR_WAIT_PHASE2_PUSH_OK,          // CAS-64 push succeeded

  // --- Futex::wait Phase 2.5 / Phase 3 ---
  CTR_WAIT_PHASE25_WAKE_CLEAN_OR_ORPHAN,  // wake_word!=0, state==SIGNALED
  CTR_WAIT_PHASE25_WAKE_GHOST,            // wake_word!=0, state!=SIGNALED
  CTR_WAIT_PHASE3_CAS_OK,                 // WAITING→IN_KERNEL committed
  CTR_WAIT_PHASE3_CAS_FAIL_ORPHAN,        // CAS failed → self-commit ORPHAN

  // --- Futex::wait Phase 4 (kernel loop) ---
  CTR_WAIT_PHASE4_ITERATIONS,             // every loop-top pass
  CTR_WAIT_PHASE4_PREPARK_WAKE_SIGNALED,  // pre-NtWait wake observed (live)
  CTR_WAIT_PHASE4_PREPARK_WAKE_GHOST,     // pre-NtWait wake observed (ghost)
  CTR_WAIT_PHASE4_NTWAIT_CALLS,           // NtWaitForAlertByThreadId invoked
  CTR_WAIT_PHASE4_NTWAIT_RET_ALERTED,     // status == STATUS_ALERTED
  CTR_WAIT_PHASE4_NTWAIT_RET_TIMEOUT,     // status == STATUS_TIMEOUT
  CTR_WAIT_PHASE4_NTWAIT_RET_USER_APC,    // status == STATUS_USER_APC
  CTR_WAIT_PHASE4_NTWAIT_RET_OTHER,       // anything else
  CTR_WAIT_PHASE4_POSTWAIT_WAKE_SIGNALED, // wake_word!=0, state==SIGNALED
  CTR_WAIT_PHASE4_POSTWAIT_WAKE_GHOST,    // wake_word!=0, state!=SIGNALED
  CTR_WAIT_PHASE4_DRAIN_MARK_LATE,        // absorb-style drain set late-alert
  CTR_WAIT_PHASE4_CANCEL_BY_TIMEOUT,      // cancel_or_absorb(-ETIMEDOUT)
  CTR_WAIT_PHASE4_CANCEL_STALE_PRED,      // stale-latched + value moved off
  CTR_WAIT_PHASE4_STALE_RELOOP,           // stale-latched + pred still holds
  CTR_WAIT_PHASE4_SIGNAL_LIKE_INT,        // interruptible EINTR
  CTR_WAIT_PHASE4_SIGNAL_LIKE_NONINT,     // non-interruptible reloop

  // --- Futex::wait return codes ---
  CTR_WAIT_RET_0,
  CTR_WAIT_RET_1,
  CTR_WAIT_RET_ETIMEDOUT,
  CTR_WAIT_RET_EINTR,
  CTR_WAIT_RET_EINVAL,
  CTR_WAIT_RET_ENOMEM,
  CTR_WAIT_RET_OTHER,

  // --- HANDOFF_BIT decode — one counter per call site that returns 1 ---
  //
  // Every wait() return of 1 must fall into exactly one of these
  // buckets. Summing them gives an independent check on CTR_WAIT_RET_1
  // and localises WHICH decode site is seeing a HANDOFF that has no
  // matching publish. Each "observed" counter records the raw
  // wake_word read just before the HANDOFF test, so comparing
  // observed vs consumed tells us how many reads saw HANDOFF but
  // didn't turn into ret=1 (gated by state check or similar).
  CTR_WAIT_HANDOFF_CONSUMED_PHASE25,
  CTR_WAIT_HANDOFF_CONSUMED_PHASE3_SELFCOMMIT,
  CTR_WAIT_HANDOFF_CONSUMED_PHASE4_PREPARK,
  CTR_WAIT_HANDOFF_CONSUMED_PHASE4_POSTWAIT,
  CTR_WAIT_NOHANDOFF_PHASE25,
  CTR_WAIT_NOHANDOFF_PHASE3_SELFCOMMIT,
  CTR_WAIT_NOHANDOFF_PHASE4_PREPARK,
  CTR_WAIT_NOHANDOFF_PHASE4_POSTWAIT,

  // Gen snapshot at slot alloc, compared at HANDOFF decode. If a
  // waiter's current slot gen differs from the gen under which the
  // waker's wake_word CAS landed, that's a smoking-gun ghost
  // consume. Count slots whose gen matched and didn't match at
  // decode time (per site).
  CTR_WAIT_HANDOFF_PHASE25_GEN_MATCH,
  CTR_WAIT_HANDOFF_PHASE25_GEN_MISMATCH,
  CTR_WAIT_HANDOFF_PHASE4_GEN_MATCH,
  CTR_WAIT_HANDOFF_PHASE4_GEN_MISMATCH,

  // Ghost-arming events: a publish_lost at site X where the value
  // that beat us to the CAS carries HANDOFF_BIT. That HANDOFF bit
  // survives on the slot until either (a) the owner's Phase-2.5 /
  // Phase-4 decode reads it (ghost consumption; the state check is
  // SUPPOSED to reject it, but only rejects if state != SIGNALED at
  // decode time), or (b) clear_slot_owned zeroes wake_word on the
  // owner's clean exit. Gap between (a) and (b) is the ghost-
  // consumption window.
  CTR_HO_WAITING_PUBLISH_LOST_PRIOR_HANDOFF,
  CTR_HO_IN_KERNEL_PUBLISH_LOST_PRIOR_HANDOFF,
  CTR_POP_PUBLISH_LOST_PRIOR_HANDOFF,

  // Bench-level: direct double-ownership detector. Worker fetch_adds
  // an in-CS atomic on entry; if the pre-increment value was !=0,
  // another worker was in the CS simultaneously. Costs 2 lock xadd
  // per iter but is the most direct possible evidence of a mutex
  // correctness violation.
  CTR_BENCH_DOUBLE_OWNERSHIP_OBSERVED,
  CTR_BENCH_MAX_CS_OCCUPANCY_OBSERVED,  // largest `in_cs` observed

  // --- pop_and_signal_one ---
  CTR_POP_CALLS,
  CTR_POP_EMPTY,
  CTR_POP_TOP_STALE,
  CTR_POP_TOP_LIVE_WAITING,
  CTR_POP_TOP_LIVE_IN_KERNEL,
  CTR_POP_TOP_LIVE_TIMEDOUT,
  CTR_POP_TOP_LIVE_SIGNALED_HELP,
  CTR_POP_TOP_LIVE_IDLE_RECOVER,
  CTR_POP_PREMARK_CAS_OK,
  CTR_POP_PREMARK_CAS_RETRY,
  CTR_POP_DETACH_OK,
  CTR_POP_DETACH_FAIL_ORPHAN,
  CTR_POP_PUBLISH_OK,
  CTR_POP_PUBLISH_LOST,
  CTR_POP_ALERT_ISSUED,
  CTR_POP_ALERT_SKIPPED_NOT_SIGNALED,
  CTR_POP_ALERT_SKIPPED_NOT_IN_KERNEL,
  CTR_POP_RET_TRUE,
  CTR_POP_RET_FALSE,

  // --- handoff_one ---
  CTR_HO_CALLS,
  CTR_HO_EMPTY_NO_WAITERS,
  CTR_HO_TOP_STALE,
  CTR_HO_TOP_LIVE_WAITING,
  CTR_HO_TOP_LIVE_IN_KERNEL,
  CTR_HO_TOP_LIVE_TIMEDOUT,
  CTR_HO_TOP_LIVE_SIGNALED_HELP,
  CTR_HO_TOP_LIVE_IDLE_RECOVER,
  CTR_HO_WAITING_PREMARK_RETRY,
  CTR_HO_WAITING_DETACH_OK,
  CTR_HO_WAITING_DETACH_FAIL,
  CTR_HO_WAITING_PUBLISH_OK,
  CTR_HO_WAITING_PUBLISH_LOST_COMPLETED,
  CTR_HO_WAITING_GHOST_REVERT_OK,
  CTR_HO_WAITING_GHOST_REVERT_LOST,
  CTR_HO_WAITING_POSTSTATE_BAD,
  CTR_HO_WAITING_ROLLBACK_CAS_OK,
  CTR_HO_WAITING_ROLLBACK_CAS_LOST,
  CTR_HO_IN_KERNEL_PREMARK_RETRY,
  CTR_HO_IN_KERNEL_DETACH_OK,
  CTR_HO_IN_KERNEL_DETACH_FAIL,
  CTR_HO_IN_KERNEL_PUBLISH_OK,
  CTR_HO_IN_KERNEL_PUBLISH_LOST,
  CTR_HO_IN_KERNEL_ALERT_ISSUED,
  CTR_HO_IN_KERNEL_ALERT_SKIPPED,
  CTR_HO_RET_HANDOFF,
  CTR_HO_RET_COMPLETED,
  CTR_HO_RET_EMPTY,

  // --- unlock_notify outer dispatch ---
  CTR_UN_RET_HANDOFF,
  CTR_UN_RET_COMPLETED,
  CTR_UN_RET_EMPTY_POP_WOKE,
  CTR_UN_RET_EMPTY_POP_NONE,

  // --- wait_slot.cpp ---
  CTR_WS_ALLOC_SLOT,
  CTR_WS_ALLOC_SECONDARY,
  CTR_WS_ALLOC_SLOT_REUSE_FAST,
  CTR_WS_FREE_SLOT,
  CTR_WS_RECLAIM_CALLS,
  CTR_WS_RECLAIM_IDEMPOTENT,
  CTR_WS_RECLAIM_HAZARD_EMPTY,
  CTR_WS_RECLAIM_RETIRED,
  CTR_WS_RECLAIM_QUEUE_FULL_DIRECT,
  CTR_WS_RETIRE_FLUSH,
  CTR_WS_FREELIST_PUSH,
  CTR_WS_ALERT_ONE_FAST,
  CTR_WS_ALERT_ONE_VALIDATED_LIVE,
  CTR_WS_ALERT_ONE_VALIDATED_DEAD,
  CTR_WS_ALERT_ONE_UNBOUND,
  CTR_WS_ALERT_MULTIPLE_CALLS,
  CTR_WS_ALERT_MULTIPLE_TIDS_ISSUED,
  CTR_WS_ALERT_MULTIPLE_TIDS_FILTERED,
  CTR_WS_HAZARD_ENTER,
  CTR_WS_HAZARD_EXIT,
  CTR_WS_CLEANUP_IDLE,
  CTR_WS_CLEANUP_SIGNALED_CLEAN,
  CTR_WS_CLEANUP_SIGNALED_ORPHAN_OR_PREMARK,
  CTR_WS_CLEANUP_WAITING_OR_IN_KERNEL_OR_TIMEDOUT,

  NUM_COUNTERS
};

// Cache-line-padded counter. Each counter lives on its OWN 64-byte line
// so concurrent bumps from different logical sites never share a line.
// sizeof(cpp::Atomic<uint64_t>) is 8; padding fills to 64.
struct alignas(64) CounterLine {
  cpp::Atomic<uint64_t> v;
  char pad[56];
};

// Per-worker phase word. One line per worker. Updated with plain
// RELAXED stores; read by the watchdog at dump time (no synchronisation
// beyond the implicit fetch that the dump does).
enum Phase : uint32_t {
  PHASE_INIT = 0,
  PHASE_IN_CAS,
  PHASE_IN_WAIT_ENTRY,
  PHASE_IN_PHASE1,
  PHASE_IN_PHASE2_PUSH,
  PHASE_IN_PHASE25_SPIN,
  PHASE_IN_PHASE3_CAS,
  PHASE_IN_PHASE4_PREPARK,
  PHASE_IN_PHASE4_NTWAIT,
  PHASE_IN_PHASE4_POSTWAIT,
  PHASE_IN_PHASE4_DRAIN,
  PHASE_IN_CS,
  PHASE_IN_UNLOCK_NOTIFY,
  PHASE_IN_POP,
  PHASE_IN_HANDOFF_ONE,
  PHASE_POST_WAIT_OK,
  PHASE_WORKER_DONE,
};

struct alignas(64) WorkerPhase {
  cpp::Atomic<uint32_t> phase;            // one of Phase
  cpp::Atomic<uint32_t> worker_id;        // bench-assigned index
  cpp::Atomic<uint32_t> last_ret;         // most recent wait() return
  cpp::Atomic<uint32_t> iter;             // current iteration
  cpp::Atomic<uint64_t> heartbeat;        // bumped in CS; watchdog reads
  // Per-wait diagnostic — lets the dump pinpoint which futex a stuck
  // waiter is parked on, and which slot they pushed. Written at
  // Futex::wait entry and slot-alloc; read under-guard by the watchdog.
  cpp::Atomic<uint64_t> wait_address{0};  // Futex* (0 when not in wait)
  cpp::Atomic<uint32_t> my_slot_idx{0};   // pool index of current slot
  cpp::Atomic<uint32_t> last_wait_status{0}; // last NtWait NTSTATUS
  char pad[64 - 40];
};

static constexpr uint32_t kMaxInstrWorkers = 64;

// --- Shared symbols (defined in futex_instrument.cpp) --------------------

extern CounterLine g_counters[NUM_COUNTERS];
extern WorkerPhase g_phases[kMaxInstrWorkers];

// Writer callback — supplied by the bench so the dump path can share
// the bench's NtWriteFile-backed output without linking its own io.
using WriteStrFn = void (*)(const char *);
void set_writers(WriteStrFn ws_fn);

// Called once by the bench before spawning workers to zero counters.
// Not required for correctness (BSS is zero), but makes back-to-back
// rounds independent.
void reset();

// Human-readable name for a Counter ID. Used by dump().
const char *name_of(Counter c);

// Dump counters + closure identities + per-worker phase table + the
// waiters stolen from `lock.stack_` if non-null. Writes to the
// supplied buffer (caller provides; typically the bench's write_str
// buffer, or we just call write_str directly — see impl).
//
// `lock_futex` is an opaque pointer to a Futex instance; impl reinterprets.
// Pass nullptr to skip the stack walk.
//
// nworkers is the number of worker-phase entries to print (<= kMaxInstrWorkers).
void dump(const char *header, void *lock_futex, uint32_t nworkers);

// Closure assertion on the bench loop — returns true iff the primary
// ledgers are closed. Prints the specific identity that failed.
// `expected_iters` = total iterations across all workers (e.g. 50000).
bool check_closure(uint64_t expected_iters);

// --- Inline per-site bump / phase helpers -------------------------------

#ifdef LIBC_FUTEX_INSTRUMENT

// Per-thread pointer to this thread's worker phase slot. Set by the
// bench at worker entry; stays nullptr for any other thread (Futex
// users outside the bench), so set_phase() is a nullptr-guarded no-op.
// RELEASE semantics aren't needed — only the owning thread writes and
// only the watchdog (same process, single read) reads.
extern LIBC_THREAD_LOCAL WorkerPhase *tls_my_phase;

LIBC_INLINE void bump(Counter c) {
  g_counters[c].v.fetch_add(1, cpp::MemoryOrder::RELAXED);
}

LIBC_INLINE void set_phase(Phase p) {
  WorkerPhase *wp = tls_my_phase;
  if (wp)
    wp->phase.store(static_cast<uint32_t>(p), cpp::MemoryOrder::RELAXED);
}

LIBC_INLINE void set_last_ret(long r) {
  WorkerPhase *wp = tls_my_phase;
  if (wp)
    wp->last_ret.store(static_cast<uint32_t>(r), cpp::MemoryOrder::RELAXED);
}

LIBC_INLINE void bump_heartbeat() {
  WorkerPhase *wp = tls_my_phase;
  if (wp)
    wp->heartbeat.fetch_add(1, cpp::MemoryOrder::RELAXED);
}

LIBC_INLINE void bind_self(WorkerPhase *wp) { tls_my_phase = wp; }

LIBC_INLINE uint64_t read_counter(Counter c) {
  return g_counters[c].v.load(cpp::MemoryOrder::RELAXED);
}

// Per-wait diagnostic setters. All no-op when tls_my_phase is null (any
// thread not running through the instrumented bench trampoline — e.g.,
// pthread-runtime internals that also call Futex::wait).
LIBC_INLINE void set_wait_address(uintptr_t addr) {
  WorkerPhase *wp = tls_my_phase;
  if (wp)
    wp->wait_address.store(addr, cpp::MemoryOrder::RELAXED);
}

LIBC_INLINE void set_my_slot_idx(uint32_t idx) {
  WorkerPhase *wp = tls_my_phase;
  if (wp)
    wp->my_slot_idx.store(idx, cpp::MemoryOrder::RELAXED);
}

LIBC_INLINE void set_last_wait_status(uint32_t status) {
  WorkerPhase *wp = tls_my_phase;
  if (wp)
    wp->last_wait_status.store(status, cpp::MemoryOrder::RELAXED);
}

#else // !LIBC_FUTEX_INSTRUMENT

// Zero-codegen stubs. All arguments discarded; the call site's
// registered counter constant never even enters an argument register.
LIBC_INLINE void bump(Counter) {}
LIBC_INLINE void set_phase(Phase) {}
LIBC_INLINE void set_last_ret(long) {}
LIBC_INLINE void bump_heartbeat() {}
LIBC_INLINE void bind_self(WorkerPhase *) {}
LIBC_INLINE uint64_t read_counter(Counter) { return 0; }
LIBC_INLINE void set_wait_address(uintptr_t) {}
LIBC_INLINE void set_my_slot_idx(uint32_t) {}
LIBC_INLINE void set_last_wait_status(uint32_t) {}

#endif // LIBC_FUTEX_INSTRUMENT

} // namespace futex_instrument
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_INSTRUMENT_H
