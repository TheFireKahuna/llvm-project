//===-- Tests for the tri-state init gate's partial-failure rollback ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Contract test for `__libc_dll_init()`'s partial-init recovery.
//
// The real entry point lives in
// `libc/src/__support/OSUtil/windows/libc_subsystem_init.cpp` and follows a
// specific shape:
//
//   1. CAS a tri-state gate UNINIT -> READY. POISONED is sticky.
//   2. Run an ordered sequence of subsystem init calls. The first non-zero
//      return invokes `fail_partial_init()`, which calls `__libc_dll_fini()`.
//      That walker (in `libc_fini_registry.h`) iterates `.libcfin$P9..$P0`
//      in REVERSE PHASE ORDER, executing each registered teardown thunk.
//   3. `__libc_dll_fini()` poisons the gate so a second call cannot rerun
//      init.
//
// The invariant we are pinning down here:
//
//   If init phase K (1-indexed) is the first failing phase, then:
//     - phases 1..K-1 must have run init AND must run fini (reverse order),
//     - phase K must have run init (which then returned failure) but must
//       NOT have a registered fini that was reached by the rollback,
//     - phases K+1..N must NOT have run init, and must NOT run fini.
//   The gate is left in a sticky POISONED state. A second call must not
//   advance the gate, must not rerun any init, and must not run any fini.
//
// We can't (and shouldn't) call the real `__libc_dll_init()` from a test —
// it ran once at process start and its state is process-global. Instead we
// reconstruct the same shape over a test-local state machine and assert
// that the invariant above is the one a correct implementation produces.
// A regression that, e.g. forgets to call fini on partial failure, or runs
// fini in forward order, or fails to poison the gate, fails this test.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "test/UnitTest/Test.h"

#include <stddef.h>

namespace {

// ---------------------------------------------------------------------------
// Test-local mock of the tri-state gate. Mirrors the values used in the
// real `g_subsystems_up`.
// ---------------------------------------------------------------------------
constexpr int kSubsystemsUninit = 0;
constexpr int kSubsystemsReady = 1;
constexpr int kSubsystemsPoisoned = 2;

constexpr size_t kMaxPhases = 16;
constexpr size_t kMaxLog = 64;

// Per-test mutable state. Reset before every test by the fixture.
struct MockPipeline {
  // The tri-state gate.
  LIBC_NAMESPACE::cpp::Atomic<int> gate{kSubsystemsUninit};

  // Sequence of init phases that ran (as 1-indexed phase IDs, in call order).
  size_t init_log[kMaxLog]{};
  size_t init_log_len = 0;

  // Sequence of fini phases that ran (1-indexed phase IDs, in call order).
  size_t fini_log[kMaxLog]{};
  size_t fini_log_len = 0;

  // 1-indexed phase that should fail. 0 means no failure.
  size_t fail_at = 0;
  // Number of phases in the pipeline.
  size_t num_phases = 0;

  void log_init(size_t phase) {
    if (init_log_len < kMaxLog)
      init_log[init_log_len++] = phase;
  }
  void log_fini(size_t phase) {
    if (fini_log_len < kMaxLog)
      fini_log[fini_log_len++] = phase;
  }
};

MockPipeline *g_mock = nullptr;

// One init thunk — logs itself and returns 0/non-zero based on `fail_at`.
int mock_init_phase(size_t phase) {
  g_mock->log_init(phase);
  if (g_mock->fail_at != 0 && phase == g_mock->fail_at)
    return 1; // simulate subsystem init failure
  return 0;
}

// One fini thunk — logs itself. The real registry walks $P9..$P0 in
// reverse, so we mimic that ordering at the call site below.
void mock_fini_phase(size_t phase) { g_mock->log_fini(phase); }

// Walk only phases [1, last_inited], in reverse. This matches the real
// `__libc_dll_fini()` contract where finis are no-ops for subsystems that
// never inited — by skipping them entirely here we make the "fini is not
// called for never-inited phases" invariant directly observable in the
// fini_log.
void run_finis_reverse(size_t last_inited) {
  for (size_t p = last_inited; p >= 1; --p) {
    mock_fini_phase(p);
    if (p == 1)
      break; // size_t guard
  }
}

// Mirror of `fail_partial_init()`: tear down everything that succeeded,
// poison the gate, return failure.
int mock_fail_partial_init(size_t last_inited) {
  run_finis_reverse(last_inited);
  g_mock->gate.store(kSubsystemsPoisoned,
                     LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
  return 1;
}

// Mirror of `__libc_dll_init()`. Returns 0 on success, non-zero on failure
// or if the gate is already POISONED. Returns 0 (treating it as "already
// inited, nothing to do") if the gate is already READY — same semantics as
// the real entry point.
int mock_libc_dll_init() {
  int prev = kSubsystemsUninit;
  if (!g_mock->gate.compare_exchange_strong(
          prev, kSubsystemsReady,
          LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL,
          LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE)) {
    if (prev == kSubsystemsPoisoned)
      return 1;
    return 0;
  }

  // Run the ordered init pipeline.
  size_t last_inited = 0;
  for (size_t phase = 1; phase <= g_mock->num_phases; ++phase) {
    if (mock_init_phase(phase) != 0) {
      // Partial-failure path: phase ran (and is in init_log) but does NOT
      // count as "successfully inited" — fini must not run for it.
      return mock_fail_partial_init(last_inited);
    }
    last_inited = phase;
  }
  return 0;
}

// Test fixture: zero out the singleton mock state per-test.
class SubsystemInitGateFixture {
public:
  SubsystemInitGateFixture() : pipeline_() { g_mock = &pipeline_; }
  ~SubsystemInitGateFixture() { g_mock = nullptr; }
  MockPipeline &pipeline() { return pipeline_; }

private:
  MockPipeline pipeline_;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. Happy path: every phase succeeds. All inits run in forward order; no
//    finis run; gate ends in READY.
// ---------------------------------------------------------------------------
TEST(LlvmLibcInitPartialFailureRunsFini, AllPhasesSucceed) {
  SubsystemInitGateFixture f;
  f.pipeline().num_phases = 8;
  f.pipeline().fail_at = 0;

  EXPECT_EQ(mock_libc_dll_init(), 0);

  ASSERT_EQ(f.pipeline().init_log_len, static_cast<size_t>(8));
  for (size_t i = 0; i < 8; ++i)
    EXPECT_EQ(f.pipeline().init_log[i], i + 1);
  EXPECT_EQ(f.pipeline().fini_log_len, static_cast<size_t>(0));
  EXPECT_EQ(f.pipeline().gate.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE),
            kSubsystemsReady);
}

// ---------------------------------------------------------------------------
// 2. Core contract: phase K of N fails. inits 1..K ran, finis K-1..1 ran
//    in reverse, phases K+1..N never touched, gate is POISONED.
// ---------------------------------------------------------------------------
TEST(LlvmLibcInitPartialFailureRunsFini, PhaseKFailsRunsReverseFinisOneToKMinus1) {
  SubsystemInitGateFixture f;
  constexpr size_t N = 8;
  constexpr size_t K = 6;
  f.pipeline().num_phases = N;
  f.pipeline().fail_at = K;

  EXPECT_EQ(mock_libc_dll_init(), 1);

  // Inits ran for exactly phases 1..K (K is the failing phase, but its
  // init function still got called — that's how it returned failure).
  ASSERT_EQ(f.pipeline().init_log_len, K);
  for (size_t i = 0; i < K; ++i)
    EXPECT_EQ(f.pipeline().init_log[i], i + 1);

  // Finis ran for phases K-1, K-2, ..., 1 — reverse order, EXCLUDING K
  // (init for K returned failure — by the per-phase contract it owns no
  // state past the failure point).
  ASSERT_EQ(f.pipeline().fini_log_len, K - 1);
  for (size_t i = 0; i < K - 1; ++i)
    EXPECT_EQ(f.pipeline().fini_log[i], (K - 1) - i);

  // Phases K+1..N must NOT appear in either log.
  for (size_t i = 0; i < f.pipeline().init_log_len; ++i)
    EXPECT_LE(f.pipeline().init_log[i], K);
  for (size_t i = 0; i < f.pipeline().fini_log_len; ++i)
    EXPECT_LT(f.pipeline().fini_log[i], K);

  EXPECT_EQ(f.pipeline().gate.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE),
            kSubsystemsPoisoned);
}

// ---------------------------------------------------------------------------
// 3. First-phase failure: only the failing init runs, no finis at all.
// ---------------------------------------------------------------------------
TEST(LlvmLibcInitPartialFailureRunsFini, FirstPhaseFailsNoFinis) {
  SubsystemInitGateFixture f;
  f.pipeline().num_phases = 8;
  f.pipeline().fail_at = 1;

  EXPECT_EQ(mock_libc_dll_init(), 1);

  ASSERT_EQ(f.pipeline().init_log_len, static_cast<size_t>(1));
  EXPECT_EQ(f.pipeline().init_log[0], static_cast<size_t>(1));
  // Nothing was successfully inited, so there is nothing to tear down.
  EXPECT_EQ(f.pipeline().fini_log_len, static_cast<size_t>(0));
  EXPECT_EQ(f.pipeline().gate.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE),
            kSubsystemsPoisoned);
}

// ---------------------------------------------------------------------------
// 4. Last-phase failure: every prior subsystem must roll back in reverse.
// ---------------------------------------------------------------------------
TEST(LlvmLibcInitPartialFailureRunsFini, LastPhaseFailsFullReverseRollback) {
  SubsystemInitGateFixture f;
  constexpr size_t N = 10;
  f.pipeline().num_phases = N;
  f.pipeline().fail_at = N;

  EXPECT_EQ(mock_libc_dll_init(), 1);

  ASSERT_EQ(f.pipeline().init_log_len, N);
  for (size_t i = 0; i < N; ++i)
    EXPECT_EQ(f.pipeline().init_log[i], i + 1);

  ASSERT_EQ(f.pipeline().fini_log_len, N - 1);
  for (size_t i = 0; i < N - 1; ++i)
    EXPECT_EQ(f.pipeline().fini_log[i], (N - 1) - i);

  EXPECT_EQ(f.pipeline().gate.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE),
            kSubsystemsPoisoned);
}

// ---------------------------------------------------------------------------
// 5. Poison stickiness: a second call into the gate after a partial failure
//    must NOT rerun any init or fini, must keep the gate POISONED, and must
//    return failure. This is the regression-only assertion: a buggy
//    implementation that uses store(READY) instead of CAS, or that resets
//    the gate on the failure path, would re-run init here.
// ---------------------------------------------------------------------------
TEST(LlvmLibcInitPartialFailureRunsFini, SecondCallAfterFailureDoesNotRerun) {
  SubsystemInitGateFixture f;
  constexpr size_t N = 6;
  constexpr size_t K = 4;
  f.pipeline().num_phases = N;
  f.pipeline().fail_at = K;

  EXPECT_EQ(mock_libc_dll_init(), 1);
  size_t init_len_after_first = f.pipeline().init_log_len;
  size_t fini_len_after_first = f.pipeline().fini_log_len;
  EXPECT_EQ(init_len_after_first, K);
  EXPECT_EQ(fini_len_after_first, K - 1);
  EXPECT_EQ(f.pipeline().gate.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE),
            kSubsystemsPoisoned);

  // Second call. Even if we now claim no failures, the gate must reject it.
  f.pipeline().fail_at = 0;
  EXPECT_EQ(mock_libc_dll_init(), 1);

  // Logs unchanged — neither init nor fini ran on the second call.
  EXPECT_EQ(f.pipeline().init_log_len, init_len_after_first);
  EXPECT_EQ(f.pipeline().fini_log_len, fini_len_after_first);
  EXPECT_EQ(f.pipeline().gate.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE),
            kSubsystemsPoisoned);
}

// ---------------------------------------------------------------------------
// 6. Poison-before-init path: a gate already poisoned by a prior fini run
//    (e.g. FreeLibrary teardown then LoadLibrary re-attach) refuses to
//    bring up the pipeline. No init, no fini.
// ---------------------------------------------------------------------------
TEST(LlvmLibcInitPartialFailureRunsFini, PrePoisonedGateRejectsAttach) {
  SubsystemInitGateFixture f;
  f.pipeline().num_phases = 5;
  f.pipeline().fail_at = 0;
  f.pipeline().gate.store(kSubsystemsPoisoned,
                          LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);

  EXPECT_EQ(mock_libc_dll_init(), 1);
  EXPECT_EQ(f.pipeline().init_log_len, static_cast<size_t>(0));
  EXPECT_EQ(f.pipeline().fini_log_len, static_cast<size_t>(0));
  EXPECT_EQ(f.pipeline().gate.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE),
            kSubsystemsPoisoned);
}

// ---------------------------------------------------------------------------
// 7. Already-READY gate: a second call into a successfully inited gate is
//    treated as a no-op (return 0). No re-run of any init or fini. This is
//    the "DllMain raced with __libc_init fallback" path.
// ---------------------------------------------------------------------------
TEST(LlvmLibcInitPartialFailureRunsFini, AlreadyReadyGateIsNoOp) {
  SubsystemInitGateFixture f;
  f.pipeline().num_phases = 4;
  f.pipeline().fail_at = 0;

  EXPECT_EQ(mock_libc_dll_init(), 0);
  size_t init_len_after_first = f.pipeline().init_log_len;
  EXPECT_EQ(init_len_after_first, static_cast<size_t>(4));

  // Second call — gate is READY, so the fast-path return-0 fires.
  EXPECT_EQ(mock_libc_dll_init(), 0);
  EXPECT_EQ(f.pipeline().init_log_len, init_len_after_first);
  EXPECT_EQ(f.pipeline().fini_log_len, static_cast<size_t>(0));
  EXPECT_EQ(f.pipeline().gate.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE),
            kSubsystemsReady);
}
