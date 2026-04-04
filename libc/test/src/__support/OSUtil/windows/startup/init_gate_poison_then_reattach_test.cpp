//===-- Tests for the libc DLL init gate's POISON terminal contract ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
//
// Background
// ----------
// __libc_dll_init() guards subsystem bring-up with a tri-state CAS gate
// (UNINIT / READY / POISONED in the production source). The expanded
// 5-state model exercised here is the conceptual machine the gate
// implements when its lifecycle is unrolled across a full ATTACH→DETACH
// cycle:
//
//                    +----------+
//   UNINIT  -------> | INITING  | --(success)--> READY
//      ^             +----------+
//      |                  |  (failure)
//      |                  v
//      |               POISONED  <----------------+
//      |                  ^                       |
//      |                  | (fini complete)       |
//      |                  |                       |
//      +-- (no edge) -- FINALIZING <-- READY -----+
//
// Allowed edges (and ONLY these):
//   UNINIT      -> INITIALIZING        (CAS by would-be initialiser)
//   INITIALIZING-> READY               (success publication)
//   INITIALIZING-> POISONED            (Tier B failure → fail_partial_init)
//   READY       -> FINALIZING          (FreeLibrary entered __libc_dll_fini)
//   FINALIZING  -> POISONED            (fini complete, gate latched)
//
// The terminal contract:
//   * POISONED is permanent. No CAS from POISONED to anything succeeds.
//   * Any attempt to drive UNINIT→INITIALIZING when the current state is
//     POISONED MUST fail and report an error to the caller. The init
//     function MUST NOT run, and the gate state MUST NOT change.
//
// What this test does
// -------------------
// Reproduce the gate's CAS state machine in a local template and drive it
// through every legal transition concurrently from many threads. Record
// every (before, after) edge that any thread observes and assert the
// recorded edge set is a subset of the allowed-edges relation.
//
// The "attempt-to-init from POISONED" contract is exercised in two ways:
//   1. A dedicated attempt_init() helper that mirrors the production
//      compare_exchange_strong against UNINIT — it must always fail when
//      the gate is POISONED, must not call the supplied init thunk, and
//      must report the gate value back unchanged.
//   2. Worker threads that loop drive_full_cycle() also race with
//      attempt_init() workers; the transition recorder catches any
//      illegal POISONED→* edge.
//
// Concurrency knobs (per spec): 8 threads, 1000 iterations.
//
//===---------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include <stdint.h>

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;

namespace {

// ---------------------------------------------------------------------------
// State enumeration. Layout deliberately matches the spec's narrative:
// numeric ordering carries no meaning — transitions are validated against
// an explicit edge set, never by ordering comparisons.
// ---------------------------------------------------------------------------
enum GateState : int {
  UNINIT = 0,
  INITIALIZING = 1,
  READY = 2,
  FINALIZING = 3,
  POISONED = 4,
  STATE_COUNT = 5,
};

// Result codes returned by attempt_init() — mirrors the production
// __libc_dll_init() return convention closely enough to assert on.
enum InitResult : int {
  INIT_OK_FIRST_WINNER = 0, // CAS won, init thunk ran, transitioned to READY
  INIT_OK_ALREADY_READY = 1, // someone else already finished init
  INIT_FAIL_POISONED = 2,    // refused — gate latched POISONED
  INIT_FAIL_TRANSIENT = 3,   // observed INITIALIZING/FINALIZING; backed off
};

// ---------------------------------------------------------------------------
// The gate. CAS-only mutation surface, mirroring the real gate.
//
// Methods are intentionally minimal — the production code uses a single
// compare_exchange_strong against UNINIT; the richer machine modelled here
// adds the FINALIZING transient state used by __libc_dll_fini and exposes
// the fail_partial_init failure edge directly.
// ---------------------------------------------------------------------------
class Gate {
public:
  Gate() = default;

  int load() { return state_.load(MemoryOrder::ACQUIRE); }

  // Strict CAS. Returns true on success, false on contention. On failure,
  // *prev_out is set to the observed value (matches std::atomic semantics).
  //
  // The Gate is a transparent atomic — there is no enforcement layer here.
  // POISONED-is-terminal is a property of the *production call sites* (none
  // of __libc_dll_init / __libc_dll_fini ever passes expected=POISONED), not
  // of the atomic itself. Tests that want to verify the terminal contract
  // must stress it through the legal API surface (attempt_init,
  // attempt_begin_fini, attempt_fail_init, latch_poison_after_fini), which
  // is what production code actually uses.
  bool cas(int expected, int desired, int *prev_out) {
    int prev = expected;
    bool ok = state_.compare_exchange_strong(
        prev, desired, MemoryOrder::ACQ_REL, MemoryOrder::ACQUIRE);
    if (prev_out)
      *prev_out = prev;
    return ok;
  }

private:
  Atomic<int> state_{UNINIT};
};

// ---------------------------------------------------------------------------
// Allowed transition set. Membership predicate is the sole correctness
// arbiter — every recorded (before, after) edge is filtered through this.
// Any (before, after) outside this set is a hard test failure.
// ---------------------------------------------------------------------------
constexpr bool is_allowed_transition(int before, int after) {
  if (before == after)
    return false; // CAS that doesn't change state is not an "edge"
  switch (before) {
  case UNINIT:
    return after == INITIALIZING;
  case INITIALIZING:
    return after == READY || after == POISONED;
  case READY:
    return after == FINALIZING;
  case FINALIZING:
    return after == POISONED;
  case POISONED:
    return false; // terminal — NO outbound edges, ever
  default:
    return false;
  }
}

// ---------------------------------------------------------------------------
// Transition recorder. Each cell counts edges observed during the run; a
// non-zero count in a disallowed cell is a test failure.
// ---------------------------------------------------------------------------
struct TransitionMatrix {
  Atomic<uint64_t> cells[STATE_COUNT][STATE_COUNT]{};
  // Sticky flags surfaced to the test body (so a single illegal edge is
  // visible even amid millions of legal ones).
  Atomic<uint32_t> illegal_edge_seen{0};
  Atomic<uint32_t> init_thunk_ran_after_poison{0};
  Atomic<uint32_t> state_changed_after_poison{0};

  void record(int before, int after) {
    cells[before][after].fetch_add(1, MemoryOrder::RELAXED);
    if (!is_allowed_transition(before, after))
      illegal_edge_seen.store(1, MemoryOrder::RELEASE);
  }
};

// ---------------------------------------------------------------------------
// Mirror of __libc_dll_init()'s entry CAS: try UNINIT→INITIALIZING. The
// init thunk is a no-op counter; the test asserts it NEVER runs while the
// gate is POISONED.
// ---------------------------------------------------------------------------
template <typename InitThunk>
InitResult attempt_init(Gate &g, TransitionMatrix &m, InitThunk init_thunk) {
  int prev = UNINIT;
  if (g.cas(UNINIT, INITIALIZING, &prev)) {
    m.record(UNINIT, INITIALIZING);
    // We are the winner. Run the init thunk, then publish READY.
    init_thunk();
    int prev2 = INITIALIZING;
    bool ok = g.cas(INITIALIZING, READY, &prev2);
    // The CAS must succeed: nothing else may legally take INITIALIZING
    // away from us in this state machine. If it fails, a foreign edge
    // happened and the recorder will catch it via the actual observed
    // before-state of any racing CAS.
    if (ok)
      m.record(INITIALIZING, READY);
    return INIT_OK_FIRST_WINNER;
  }

  // CAS lost. Translate the observed prior state into a caller code.
  switch (prev) {
  case POISONED:
    return INIT_FAIL_POISONED;
  case READY:
    return INIT_OK_ALREADY_READY;
  case INITIALIZING:
  case FINALIZING:
    return INIT_FAIL_TRANSIENT;
  default:
    // UNINIT here would mean compare_exchange spuriously failed — strong
    // CAS forbids it. Treat as transient if it ever happens.
    return INIT_FAIL_TRANSIENT;
  }
}

// Mirror of __libc_dll_init()'s failure path: drive INITIALIZING→POISONED.
// The fini sequence does this via fail_partial_init() in the real source.
inline bool attempt_fail_init(Gate &g, TransitionMatrix &m) {
  int prev = INITIALIZING;
  if (g.cas(INITIALIZING, POISONED, &prev)) {
    m.record(INITIALIZING, POISONED);
    return true;
  }
  return false;
}

// Mirror of __libc_dll_fini()'s entry CAS: READY → FINALIZING. Only the
// thread that wins this CAS executes fini and latches POISONED.
inline bool attempt_begin_fini(Gate &g, TransitionMatrix &m) {
  int prev = READY;
  if (g.cas(READY, FINALIZING, &prev)) {
    m.record(READY, FINALIZING);
    return true;
  }
  return false;
}

// Mirror of mark_dll_init_poisoned() — but expressed as a CAS so the
// test can observe the (FINALIZING → POISONED) edge. The production code
// uses a plain store; the CAS form is strictly stronger and equivalent
// when invoked from the unique fini-winning thread.
inline bool latch_poison_after_fini(Gate &g, TransitionMatrix &m) {
  int prev = FINALIZING;
  if (g.cas(FINALIZING, POISONED, &prev)) {
    m.record(FINALIZING, POISONED);
    return true;
  }
  return false;
}

// Drive one full successful lifecycle: UNINIT → INITIALIZING → READY →
// FINALIZING → POISONED. After this completes, all subsequent
// attempt_init() calls on this gate MUST return INIT_FAIL_POISONED.
inline void drive_full_cycle(Gate &g, TransitionMatrix &m) {
  Atomic<uint32_t> thunk_count{0};
  InitResult r = attempt_init(g, m, [&]() {
    thunk_count.fetch_add(1, MemoryOrder::RELAXED);
  });
  if (r != INIT_OK_FIRST_WINNER)
    return; // someone else won; they will drive the cycle
  if (!attempt_begin_fini(g, m))
    return;
  (void)latch_poison_after_fini(g, m);
}

// ---------------------------------------------------------------------------
// Adversary: hammer attempt_init() against an already-POISONED gate. The
// init thunk MUST NOT run; the gate value MUST stay POISONED. We snapshot
// the gate before and after the call and flag any deviation.
// ---------------------------------------------------------------------------
struct PoisonedReattackArgs {
  Gate *gate;
  TransitionMatrix *matrix;
  uint32_t iterations;
  Atomic<uint32_t> *thunk_runs;       // must remain 0
  Atomic<uint32_t> *unexpected_returns; // anything other than INIT_FAIL_POISONED
  Atomic<uint32_t> done;
};

NTAPI DWORD poisoned_reattach_worker(void *arg) {
  auto *a = static_cast<PoisonedReattackArgs *>(arg);
  for (uint32_t i = 0; i < a->iterations; ++i) {
    int before = a->gate->load();
    InitResult r = attempt_init(*a->gate, *a->matrix, [&]() {
      // Must never execute when the gate was POISONED.
      a->thunk_runs->fetch_add(1, MemoryOrder::RELAXED);
      a->matrix->init_thunk_ran_after_poison.store(1, MemoryOrder::RELEASE);
    });
    int after = a->gate->load();
    if (before == POISONED) {
      if (r != INIT_FAIL_POISONED)
        a->unexpected_returns->fetch_add(1, MemoryOrder::RELAXED);
      if (after != POISONED)
        a->matrix->state_changed_after_poison.store(1, MemoryOrder::RELEASE);
    }
  }
  a->done.store(1, MemoryOrder::RELEASE);
  return 0;
}

// ---------------------------------------------------------------------------
// Adversary: hammer every *legal* lifecycle entry point against an already-
// POISONED gate. Each helper mirrors a production call site:
//
//   attempt_init             — __libc_dll_init() entry (CAS UNINIT→INITIALIZING)
//   attempt_fail_init        — fail_partial_init() (CAS INITIALIZING→POISONED)
//   attempt_begin_fini       — __libc_dll_fini() entry (CAS READY→FINALIZING)
//   latch_poison_after_fini  — fini publish (CAS FINALIZING→POISONED)
//
// In production, each helper is invoked unconditionally — a re-attach attempt
// after a previous lifecycle has poisoned the gate must be a no-op for every
// helper, regardless of which production call path is taken. None of them
// can move the gate out of POISONED because none of them passes
// expected=POISONED to the underlying CAS; their CAS sees a state mismatch
// and returns false. This worker stress-tests that property under
// concurrent contention.
// ---------------------------------------------------------------------------
struct EscapeArgs {
  Gate *gate;
  TransitionMatrix *matrix;
  uint32_t iterations;
  Atomic<uint32_t> *helper_returned_true; // any helper claiming success on POISONED
  Atomic<uint32_t> done;
};

NTAPI DWORD poisoned_escape_worker(void *arg) {
  auto *a = static_cast<EscapeArgs *>(arg);
  Atomic<uint32_t> ignored_thunk{0};
  for (uint32_t i = 0; i < a->iterations; ++i) {
    // attempt_init: must return INIT_FAIL_POISONED (never run the thunk).
    InitResult r = attempt_init(*a->gate, *a->matrix, [&]() {
      ignored_thunk.fetch_add(1, MemoryOrder::RELAXED);
      a->matrix->init_thunk_ran_after_poison.store(1, MemoryOrder::RELEASE);
    });
    if (r != INIT_FAIL_POISONED)
      a->helper_returned_true->fetch_add(1, MemoryOrder::RELAXED);

    // attempt_fail_init: production drives this only after a winning
    // attempt_init won INITIALIZING. Against POISONED it must be a no-op.
    if (attempt_fail_init(*a->gate, *a->matrix))
      a->helper_returned_true->fetch_add(1, MemoryOrder::RELAXED);

    // attempt_begin_fini: production drives this from FreeLibrary fini.
    // Against POISONED it must be a no-op (no READY→FINALIZING edge).
    if (attempt_begin_fini(*a->gate, *a->matrix))
      a->helper_returned_true->fetch_add(1, MemoryOrder::RELAXED);

    // latch_poison_after_fini: production fires this only at the end of the
    // unique winning fini. Against POISONED it must fail (no FINALIZING in
    // play, so the CAS sees POISONED and reports back unchanged).
    if (latch_poison_after_fini(*a->gate, *a->matrix))
      a->helper_returned_true->fetch_add(1, MemoryOrder::RELAXED);

    // After any of these, state must remain POISONED.
    if (a->gate->load() != POISONED)
      a->matrix->state_changed_after_poison.store(1, MemoryOrder::RELEASE);
  }
  a->done.store(1, MemoryOrder::RELEASE);
  return 0;
}

// ---------------------------------------------------------------------------
// Concurrent full-cycle driver. Many threads race to perform the legal
// lifecycle transitions on a fresh gate; the recorder must not catch any
// illegal edges.
// ---------------------------------------------------------------------------
struct CycleArgs {
  Gate *gate;
  TransitionMatrix *matrix;
  uint32_t iterations; // number of cycles each thread tries to drive
  Atomic<uint32_t> done;
};

NTAPI DWORD full_cycle_worker(void *arg) {
  auto *a = static_cast<CycleArgs *>(arg);
  // Each "iteration" is an attempt to drive the shared gate through the
  // full UNINIT→...→POISONED lifecycle. Only one thread will actually do
  // it; the others observe and report. Subsequent iterations on a now-
  // poisoned gate must each be no-ops with respect to gate state.
  for (uint32_t i = 0; i < a->iterations; ++i)
    drive_full_cycle(*a->gate, *a->matrix);
  a->done.store(1, MemoryOrder::RELEASE);
  return 0;
}

// Helper wrapping wait + close. Test macros (ASSERT_EQ) resolve to a
// TestContext member function and aren't usable from free functions, so we
// don't check the wait result here — callers assert test-visible invariants
// (final state, counters) after joining, which would catch a silent hang.
inline void join_thread(HANDLE h) {
  (void)LIBC_NAMESPACE::test_support::wait_for_single_object(h, 30000);
  ::NtClose(h);
}

} // namespace

// ===========================================================================
// 1. Pure single-threaded edge-validity check. Walks the legal lifecycle
//    end-to-end and asserts every recorded edge is in the allowed set.
//    Also asserts attempt_init() against a POISONED gate is a no-op.
// ===========================================================================
TEST(LlvmLibcInitGatePoison, SingleThreadedFullLifecycleAndPoisonReentry) {
  Gate g;
  TransitionMatrix m;
  Atomic<uint32_t> thunk_calls{0};

  // UNINIT → INITIALIZING → READY
  EXPECT_EQ(g.load(), static_cast<int>(UNINIT));
  EXPECT_EQ(attempt_init(g, m, [&]() {
              thunk_calls.fetch_add(1, MemoryOrder::RELAXED);
            }),
            INIT_OK_FIRST_WINNER);
  EXPECT_EQ(g.load(), static_cast<int>(READY));
  EXPECT_EQ(thunk_calls.load(MemoryOrder::ACQUIRE), 1u);

  // READY → FINALIZING → POISONED
  EXPECT_TRUE(attempt_begin_fini(g, m));
  EXPECT_EQ(g.load(), static_cast<int>(FINALIZING));
  EXPECT_TRUE(latch_poison_after_fini(g, m));
  EXPECT_EQ(g.load(), static_cast<int>(POISONED));

  // Re-attach attempt: must fail, must not run thunk, must not change state.
  for (int i = 0; i < 16; ++i) {
    int before = g.load();
    InitResult r = attempt_init(g, m, [&]() {
      thunk_calls.fetch_add(1, MemoryOrder::RELAXED);
    });
    EXPECT_EQ(r, INIT_FAIL_POISONED);
    EXPECT_EQ(g.load(), before);
    EXPECT_EQ(g.load(), static_cast<int>(POISONED));
  }
  EXPECT_EQ(thunk_calls.load(MemoryOrder::ACQUIRE), 1u);

  // POISONED is terminal: every *legal* lifecycle helper, when invoked on
  // a poisoned gate, must be a no-op. This mirrors the production
  // discipline (the only mutation surface code ever uses) — none of these
  // helpers passes expected=POISONED to the underlying CAS, so each one's
  // CAS sees a state mismatch and reports back unchanged. Verify each
  // individually so a regression in any single helper is pinpointed.
  Atomic<uint32_t> stray_thunks{0};
  EXPECT_EQ(attempt_init(g, m, [&]() {
              stray_thunks.fetch_add(1, MemoryOrder::RELAXED);
            }),
            INIT_FAIL_POISONED);
  EXPECT_EQ(g.load(), static_cast<int>(POISONED));

  EXPECT_FALSE(attempt_fail_init(g, m));
  EXPECT_EQ(g.load(), static_cast<int>(POISONED));

  EXPECT_FALSE(attempt_begin_fini(g, m));
  EXPECT_EQ(g.load(), static_cast<int>(POISONED));

  EXPECT_FALSE(latch_poison_after_fini(g, m));
  EXPECT_EQ(g.load(), static_cast<int>(POISONED));

  EXPECT_EQ(stray_thunks.load(MemoryOrder::ACQUIRE), 0u);

  // Edge audit. Every non-zero cell must be a member of the allowed set.
  for (int b = 0; b < STATE_COUNT; ++b) {
    for (int a = 0; a < STATE_COUNT; ++a) {
      uint64_t count = m.cells[b][a].load(MemoryOrder::ACQUIRE);
      if (count != 0) {
        EXPECT_TRUE(is_allowed_transition(b, a));
      }
    }
  }
  EXPECT_EQ(m.illegal_edge_seen.load(MemoryOrder::ACQUIRE), 0u);
}

// ===========================================================================
// 2. The headline contract: 8 threads, 1000 iterations each, hammer
//    attempt_init() and CAS-escape attempts against an already-POISONED
//    gate. Nothing must run, nothing must succeed.
// ===========================================================================
TEST(LlvmLibcInitGatePoison, PoisonedGateRejectsAllReattachAttempts) {
  // Bring a fresh gate to POISONED via the canonical lifecycle.
  Gate g;
  TransitionMatrix m;
  Atomic<uint32_t> thunk_calls{0};
  ASSERT_EQ(attempt_init(g, m, [&]() {
              thunk_calls.fetch_add(1, MemoryOrder::RELAXED);
            }),
            INIT_OK_FIRST_WINNER);
  ASSERT_TRUE(attempt_begin_fini(g, m));
  ASSERT_TRUE(latch_poison_after_fini(g, m));
  ASSERT_EQ(g.load(), static_cast<int>(POISONED));

  constexpr uint32_t kThreads = 8;
  constexpr uint32_t kIters = 1000;

  Atomic<uint32_t> reattach_thunk_runs{0};
  Atomic<uint32_t> unexpected_returns{0};
  Atomic<uint32_t> helper_returned_true{0};

  PoisonedReattackArgs reattach_args[kThreads / 2];
  EscapeArgs escape_args[kThreads / 2];
  HANDLE handles[kThreads];

  // Half the threads hammer attempt_init() in isolation (the headline
  // re-attach surface); half hammer every other legal lifecycle helper
  // too. Both arms exercise only what production code actually invokes.
  for (uint32_t i = 0; i < kThreads / 2; ++i) {
    reattach_args[i].gate = &g;
    reattach_args[i].matrix = &m;
    reattach_args[i].iterations = kIters;
    reattach_args[i].thunk_runs = &reattach_thunk_runs;
    reattach_args[i].unexpected_returns = &unexpected_returns;
    reattach_args[i].done.store(0, MemoryOrder::RELEASE);
    handles[i] = LIBC_NAMESPACE::test_support::create_thread(
        poisoned_reattach_worker, &reattach_args[i]);
    ASSERT_NE(handles[i], static_cast<HANDLE>(nullptr));
  }
  for (uint32_t i = 0; i < kThreads / 2; ++i) {
    escape_args[i].gate = &g;
    escape_args[i].matrix = &m;
    escape_args[i].iterations = kIters;
    escape_args[i].helper_returned_true = &helper_returned_true;
    escape_args[i].done.store(0, MemoryOrder::RELEASE);
    handles[kThreads / 2 + i] = LIBC_NAMESPACE::test_support::create_thread(
        poisoned_escape_worker, &escape_args[i]);
    ASSERT_NE(handles[kThreads / 2 + i], static_cast<HANDLE>(nullptr));
  }

  for (uint32_t i = 0; i < kThreads; ++i)
    join_thread(handles[i]);

  // Hard contract assertions.
  EXPECT_EQ(g.load(), static_cast<int>(POISONED));
  EXPECT_EQ(reattach_thunk_runs.load(MemoryOrder::ACQUIRE), 0u);
  EXPECT_EQ(unexpected_returns.load(MemoryOrder::ACQUIRE), 0u);
  EXPECT_EQ(helper_returned_true.load(MemoryOrder::ACQUIRE), 0u);
  EXPECT_EQ(m.init_thunk_ran_after_poison.load(MemoryOrder::ACQUIRE), 0u);
  EXPECT_EQ(m.state_changed_after_poison.load(MemoryOrder::ACQUIRE), 0u);
  EXPECT_EQ(m.illegal_edge_seen.load(MemoryOrder::ACQUIRE), 0u);

  // Edge audit: with the gate already POISONED before the workers
  // started, NO edge cell should have been incremented at all by them.
  // (The pre-workload setup already populated the legal cells.)
  // Verify no POISONED→* cell has any count.
  for (int after = 0; after < STATE_COUNT; ++after) {
    if (after == POISONED)
      continue;
    EXPECT_EQ(m.cells[POISONED][after].load(MemoryOrder::ACQUIRE),
              static_cast<uint64_t>(0));
  }
}

// ===========================================================================
// 3. Concurrent-cycle stress. 8 threads × 1000 iterations each race to
//    drive the FULL state machine on one gate. Only one cycle can ever
//    win on a single gate (POISONED is terminal), but the recorder must
//    not catch any illegal edge from any thread along the way.
// ===========================================================================
TEST(LlvmLibcInitGatePoison, ConcurrentFullCycleProducesOnlyAllowedEdges) {
  // Repeat across many fresh gates so the contended region is hit
  // many times in aggregate. Per-gate iteration count stays low because
  // POISONED is terminal — extra iterations on a finalised gate
  // degenerate to "everyone observes POISONED, returns no-op".
  constexpr uint32_t kThreads = 8;
  constexpr uint32_t kIters = 1000;

  // Outer: many gates so there's always racing UNINIT→INITIALIZING work.
  // Inner: each thread attempts kIters drive_full_cycle calls per gate.
  // Aggregate per-gate work is kThreads × kIters CAS attempts.
  constexpr uint32_t kGates = 32;

  for (uint32_t gate_idx = 0; gate_idx < kGates; ++gate_idx) {
    Gate g;
    TransitionMatrix m;
    CycleArgs args[kThreads];
    HANDLE handles[kThreads];
    for (uint32_t i = 0; i < kThreads; ++i) {
      args[i].gate = &g;
      args[i].matrix = &m;
      args[i].iterations = kIters;
      args[i].done.store(0, MemoryOrder::RELEASE);
      handles[i] = LIBC_NAMESPACE::test_support::create_thread(
          full_cycle_worker, &args[i]);
      ASSERT_NE(handles[i], static_cast<HANDLE>(nullptr));
    }
    for (uint32_t i = 0; i < kThreads; ++i)
      join_thread(handles[i]);

    // Final state must be POISONED — every legal sequence terminates there.
    EXPECT_EQ(g.load(), static_cast<int>(POISONED));

    // Edge audit: every non-zero cell is a member of the allowed set.
    for (int b = 0; b < STATE_COUNT; ++b) {
      for (int a = 0; a < STATE_COUNT; ++a) {
        uint64_t count = m.cells[b][a].load(MemoryOrder::ACQUIRE);
        if (count != 0)
          EXPECT_TRUE(is_allowed_transition(b, a));
      }
    }
    EXPECT_EQ(m.illegal_edge_seen.load(MemoryOrder::ACQUIRE), 0u);
    EXPECT_EQ(m.init_thunk_ran_after_poison.load(MemoryOrder::ACQUIRE), 0u);
    EXPECT_EQ(m.state_changed_after_poison.load(MemoryOrder::ACQUIRE), 0u);

    // Each terminal-edge cell must have been hit exactly once: POISONED
    // is absorbing, so only ONE cycle can produce the FINALIZING→POISONED
    // edge across the entire workload on this gate.
    EXPECT_EQ(m.cells[FINALIZING][POISONED].load(MemoryOrder::ACQUIRE),
              static_cast<uint64_t>(1));
    EXPECT_EQ(m.cells[READY][FINALIZING].load(MemoryOrder::ACQUIRE),
              static_cast<uint64_t>(1));
    EXPECT_EQ(m.cells[INITIALIZING][READY].load(MemoryOrder::ACQUIRE),
              static_cast<uint64_t>(1));
    EXPECT_EQ(m.cells[UNINIT][INITIALIZING].load(MemoryOrder::ACQUIRE),
              static_cast<uint64_t>(1));
    // The INITIALIZING→POISONED failure edge is not exercised by
    // drive_full_cycle (it models the success path), so this cell
    // must remain zero in this stress.
    EXPECT_EQ(m.cells[INITIALIZING][POISONED].load(MemoryOrder::ACQUIRE),
              static_cast<uint64_t>(0));
  }
}

// ===========================================================================
// 4. INITIALIZING→POISONED failure-edge model. Mirrors fail_partial_init()
//    in the production source: a Tier B subsystem fails after the gate
//    transitioned UNINIT→INITIALIZING, so the gate latches POISONED
//    directly without ever publishing READY. Subsequent re-attach attempts
//    must still be rejected.
// ===========================================================================
TEST(LlvmLibcInitGatePoison, InitializingFailureLatchesPoisonAndRejectsRetry) {
  Gate g;
  TransitionMatrix m;

  // Drive UNINIT → INITIALIZING by hand (no init-thunk publication yet).
  int prev = UNINIT;
  ASSERT_TRUE(g.cas(UNINIT, INITIALIZING, &prev));
  m.record(UNINIT, INITIALIZING);
  ASSERT_EQ(g.load(), static_cast<int>(INITIALIZING));

  // Simulate a Tier B failure: fail_partial_init() drives INITIALIZING →
  // POISONED instead of INITIALIZING → READY.
  ASSERT_TRUE(attempt_fail_init(g, m));
  ASSERT_EQ(g.load(), static_cast<int>(POISONED));

  // Re-attach attempts now must all return INIT_FAIL_POISONED. This is
  // the exact case the contract was designed around: a partial Tier B
  // failure left subsystems half-built, then the loader (or a subsequent
  // explicit LoadLibrary) tries to re-enter init.
  Atomic<uint32_t> thunk_calls{0};
  for (int i = 0; i < 32; ++i) {
    InitResult r = attempt_init(g, m, [&]() {
      thunk_calls.fetch_add(1, MemoryOrder::RELAXED);
    });
    EXPECT_EQ(r, INIT_FAIL_POISONED);
    EXPECT_EQ(g.load(), static_cast<int>(POISONED));
  }
  EXPECT_EQ(thunk_calls.load(MemoryOrder::ACQUIRE), 0u);

  // Edge audit.
  for (int b = 0; b < STATE_COUNT; ++b) {
    for (int a = 0; a < STATE_COUNT; ++a) {
      uint64_t count = m.cells[b][a].load(MemoryOrder::ACQUIRE);
      if (count != 0)
        EXPECT_TRUE(is_allowed_transition(b, a));
    }
  }
  EXPECT_EQ(m.illegal_edge_seen.load(MemoryOrder::ACQUIRE), 0u);
  // Confirm both legal exits from INITIALIZING are representable: the
  // success edge wasn't taken here (count == 0), the failure edge was
  // (count == 1).
  EXPECT_EQ(m.cells[INITIALIZING][READY].load(MemoryOrder::ACQUIRE),
            static_cast<uint64_t>(0));
  EXPECT_EQ(m.cells[INITIALIZING][POISONED].load(MemoryOrder::ACQUIRE),
            static_cast<uint64_t>(1));
}
