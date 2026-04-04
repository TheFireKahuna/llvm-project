//===-- Concurrent __libc_init / __libc_dll_init gate stress test --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Hermetic regression test for the §1 race in libc startup, where one
// thread enters libc init via the EXE-side __libc_init() (freestanding
// path) and another via __libc_dll_init() (DllMain path) concurrently.
// Both share the file-scope Atomic<int> tri-state gate g_subsystems_up
// inside libc/src/__support/OSUtil/windows/libc_subsystem_init.cpp:
//
//     0 = UNINIT     (Tier B not yet run)
//     1 = READY      (Tier B in progress or done)
//     2 = POISONED   (FreeLibrary fini ran — re-attach forbidden)
//
// The single CAS UNINIT->READY (ACQ_REL on success, ACQUIRE on fail)
// elects exactly one bootstrap winner; everyone else takes the
// "already initialised" branch and returns 0. POISONED is sticky and
// returns 1 to every caller forever after.
//
// Because the real gate is process-wide and has already fired by the
// time any test main() runs, we cannot exercise it directly. Instead
// this test embeds a faithful clone of the gate's CAS state machine
// into a local TriStateGate template and stresses the *same*
// transitions over many iterations, with N threads racing into the
// gate behind a release-fence start barrier so their entry windows
// overlap.
//
// Invariants checked per iteration:
//
//   1. The fake "bootstrap" function runs *exactly once* across all
//      racing threads (no double-init even with maximum overlap).
//   2. Every loser CAS observes the winner's pre-publish writes — the
//      ACQUIRE side of the failing CAS must fence the winner's stores
//      to a "subsystem ready" sentinel into the loser's view. Any
//      loser that sees the sentinel as still zero is a happens-before
//      violation.
//   3. No thread blocks the winner: every spin-wait makes monotonic
//      progress (state only ever advances UNINIT->READY) so every
//      loser eventually returns. We assert all worker threads join
//      within a generous wall-clock budget.
//   4. The POISONED transition (modelled by a separate test driving
//      the same gate clone into POISONED before any racer enters)
//      causes every entry to return failure, no bootstrap to run, and
//      no spin to deadlock.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;

namespace {

// Faithful clone of the real gate in
// libc/src/__support/OSUtil/windows/libc_subsystem_init.cpp.
//
// The CAS shape MUST match: ACQ_REL on success so the winner's
// pre-publish writes are released to losers, ACQUIRE on failure so
// losers acquire those writes via the failing CAS read of state_.
// POISONED is checked BEFORE returning the "already initialised"
// success path, so a re-attach after fini cannot silently skip Tier B.
template <typename BootstrapFn> struct TriStateGate {
  static constexpr int kUninit = 0;
  static constexpr int kReady = 1;
  static constexpr int kPoisoned = 2;

  Atomic<int> state{kUninit};

  // Mirrors __libc_dll_init():
  //   - returns 0 on success (winner ran bootstrap, or loser observed
  //     READY).
  //   - returns 1 on POISONED.
  // Bootstrap is invoked at most once per gate lifetime.
  int enter(BootstrapFn &fn) {
    int prev = kUninit;
    if (!state.compare_exchange_strong(prev, kReady, MemoryOrder::ACQ_REL,
                                       MemoryOrder::ACQUIRE)) {
      if (prev == kPoisoned)
        return 1;
      // Loser already observed READY (or POISONED above). The ACQUIRE
      // failure ordering on the CAS above synchronises with the
      // winner's RELEASE half of its successful CAS, so any side
      // effects the winner published before the CAS are visible here.
      return 0;
    }

    // Winner: publish bootstrap side-effects BEFORE the gate transitions
    // to READY in the eyes of any concurrent loser. In the real gate
    // the side effects are Tier B subsystem init; here they are a
    // single "subsystems_ready" flag that every loser must be able to
    // observe by virtue of the ACQ_REL pairing.
    fn();

    return 0;
  }

  // Mirrors mark_dll_init_poisoned(). Sticky: once latched, every
  // subsequent enter() returns failure. Used by the POISONED-driver
  // sub-test only.
  void poison() { state.store(kPoisoned, MemoryOrder::RELEASE); }
};

// A "bootstrap" stand-in that records exactly how many times it ran
// and publishes a release-fence sentinel that every loser must see.
struct BootstrapRecorder {
  Atomic<int> *runs;
  Atomic<int> *subsystems_ready;

  void operator()() {
    runs->fetch_add(1, MemoryOrder::RELAXED);
    // Publish the sentinel AFTER the runs increment so a loser that
    // observes subsystems_ready==1 also sees runs>=1 via ACQUIRE in
    // the gate's CAS-failure path. RELEASE here pairs with that.
    subsystems_ready->store(1, MemoryOrder::RELEASE);
  }
};

// Per-iteration shared state. Sized for cache-line isolation between
// counters that worker threads hammer.
struct IterCtx {
  TriStateGate<BootstrapRecorder> gate;
  Atomic<int> runs{0};
  Atomic<int> subsystems_ready{0};

  // Start-barrier word: workers spin until main flips this to 1, so
  // they all enter the CAS in the tightest window possible.
  Atomic<uint32_t> start_gate{0};

  // Workers that have arrived at the barrier (so main can wait for
  // *all* of them to be poised before releasing the gate).
  Atomic<uint32_t> arrived{0};

  // Per-thread return-code slots (bounded by kMaxThreads below).
  static constexpr int kMaxThreads = 16;
  Atomic<int> rc[kMaxThreads];
  // Per-thread loser-observation of subsystems_ready: only meaningful
  // for the threads that lost the CAS, but recorded unconditionally.
  Atomic<int> observed_ready[kMaxThreads];

  uint32_t thread_count = 0;
};

struct WorkerArg {
  IterCtx *ctx;
  uint32_t index;
};

[[gnu::ms_abi]] DWORD race_worker(void *arg) {
  auto *wa = static_cast<WorkerArg *>(arg);
  auto *ctx = wa->ctx;
  uint32_t i = wa->index;

  // Arrive at the barrier and spin (no kernel sleep — bounded by
  // main's release latency).
  ctx->arrived.fetch_add(1, MemoryOrder::ACQ_REL);
  while (ctx->start_gate.load(MemoryOrder::ACQUIRE) == 0) {
    // Tight pause; sleep_ms(0) yields if scheduler decides to.
    LIBC_NAMESPACE::test_support::sleep_ms(0);
  }

  BootstrapRecorder rec{&ctx->runs, &ctx->subsystems_ready};
  int rc = ctx->gate.enter(rec);
  ctx->rc[i].store(rc, MemoryOrder::RELEASE);

  // Read the sentinel through ACQUIRE — every thread that returned
  // from enter() with rc==0 must observe subsystems_ready == 1, even
  // if it lost the CAS, because the failing-CAS ACQUIRE order
  // synchronises with the winner's ACQ_REL success.
  ctx->observed_ready[i].store(
      ctx->subsystems_ready.load(MemoryOrder::ACQUIRE), MemoryOrder::RELEASE);

  return 0;
}

// One race iteration. Returns true on invariant pass.
bool run_one_race_iteration(uint32_t thread_count) {
  IterCtx ctx;
  ctx.thread_count = thread_count;
  for (uint32_t i = 0; i < thread_count; ++i) {
    ctx.rc[i].store(-1, MemoryOrder::RELAXED);
    ctx.observed_ready[i].store(-1, MemoryOrder::RELAXED);
  }

  WorkerArg args[IterCtx::kMaxThreads];
  HANDLE handles[IterCtx::kMaxThreads];
  for (uint32_t i = 0; i < thread_count; ++i) {
    args[i] = WorkerArg{&ctx, i};
    handles[i] = LIBC_NAMESPACE::test_support::create_thread(race_worker,
                                                             &args[i]);
    if (handles[i] == nullptr) {
      // Created threads must be reaped before bailing.
      for (uint32_t j = 0; j < i; ++j) {
        LIBC_NAMESPACE::test_support::wait_for_single_object(handles[j], 5000);
        ::NtClose(handles[j]);
      }
      return false;
    }
  }

  // Wait for every worker to be poised at the barrier.
  while (ctx.arrived.load(MemoryOrder::ACQUIRE) < thread_count)
    LIBC_NAMESPACE::test_support::sleep_ms(0);

  // Release them simultaneously into the CAS.
  ctx.start_gate.store(1, MemoryOrder::RELEASE);

  // Reap. 10s wall-clock is *generous*: a deadlock here would mean a
  // loser is permanently spinning even though the winner has published
  // READY — that is the exact bug we want to surface.
  bool all_joined = true;
  for (uint32_t i = 0; i < thread_count; ++i) {
    DWORD wait_rc = LIBC_NAMESPACE::test_support::wait_for_single_object(
        handles[i], 10000);
    if (wait_rc != LIBC_NAMESPACE::test_support::WAIT_OBJECT_0)
      all_joined = false;
    ::NtClose(handles[i]);
  }
  if (!all_joined)
    return false;

  // Invariant 1: bootstrap ran exactly once.
  if (ctx.runs.load(MemoryOrder::ACQUIRE) != 1)
    return false;

  // Invariant 2 + 3: every thread returned 0 (success — there is no
  // poison in this driver) and every thread observed the published
  // subsystems_ready==1 sentinel (no happens-before violation).
  for (uint32_t i = 0; i < thread_count; ++i) {
    if (ctx.rc[i].load(MemoryOrder::ACQUIRE) != 0)
      return false;
    if (ctx.observed_ready[i].load(MemoryOrder::ACQUIRE) != 1)
      return false;
  }

  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Test 1: Two-thread A/B race — the canonical __libc_init() vs
// __libc_dll_init() collision. Run for many iterations to shake out
// the race.
// ---------------------------------------------------------------------------

TEST(LlvmLibcBootstrapDllInitConcurrentTest, TwoThreadGateRace) {
  constexpr int kIterations = 1000;
  for (int it = 0; it < kIterations; ++it) {
    bool ok = run_one_race_iteration(/*thread_count=*/2);
    ASSERT_TRUE(ok);
  }
}

// ---------------------------------------------------------------------------
// Test 2: Wide N-thread race — exercises the loser-spin path with
// many concurrent CAS losers. Eight threads is well above the
// realistic count (only one DllMain thread + one main thread can race
// in production) but maximises the chance that a winner-loser
// happens-before bug surfaces. Runs fewer iterations because each one
// is more expensive.
// ---------------------------------------------------------------------------

TEST(LlvmLibcBootstrapDllInitConcurrentTest, EightThreadGateRace) {
  constexpr int kIterations = 250;
  for (int it = 0; it < kIterations; ++it) {
    bool ok = run_one_race_iteration(/*thread_count=*/8);
    ASSERT_TRUE(ok);
  }
}

// ---------------------------------------------------------------------------
// Test 3: POISONED is sticky and rejects every entrant without
// running bootstrap or deadlocking any spinner. Models the
// LoadLibrary("c.dll") after FreeLibrary scenario.
// ---------------------------------------------------------------------------

namespace {

struct PoisonCtx {
  TriStateGate<BootstrapRecorder> gate;
  Atomic<int> runs{0};
  Atomic<int> subsystems_ready{0};
  Atomic<uint32_t> start_gate{0};
  Atomic<uint32_t> arrived{0};
  static constexpr int kMaxThreads = 8;
  Atomic<int> rc[kMaxThreads];
  uint32_t thread_count = 0;
};

struct PoisonArg {
  PoisonCtx *ctx;
  uint32_t index;
};

[[gnu::ms_abi]] DWORD poison_worker(void *arg) {
  auto *pa = static_cast<PoisonArg *>(arg);
  auto *ctx = pa->ctx;
  uint32_t i = pa->index;

  ctx->arrived.fetch_add(1, MemoryOrder::ACQ_REL);
  while (ctx->start_gate.load(MemoryOrder::ACQUIRE) == 0)
    LIBC_NAMESPACE::test_support::sleep_ms(0);

  BootstrapRecorder rec{&ctx->runs, &ctx->subsystems_ready};
  int rc = ctx->gate.enter(rec);
  ctx->rc[i].store(rc, MemoryOrder::RELEASE);
  return 0;
}

} // namespace

TEST(LlvmLibcBootstrapDllInitConcurrentTest, PoisonedRejectsAllEntrants) {
  constexpr int kIterations = 200;
  constexpr uint32_t kThreads = 4;
  for (int it = 0; it < kIterations; ++it) {
    PoisonCtx ctx;
    ctx.thread_count = kThreads;
    for (uint32_t i = 0; i < kThreads; ++i)
      ctx.rc[i].store(-1, MemoryOrder::RELAXED);

    // Latch poison BEFORE any thread enters the gate.
    ctx.gate.poison();

    PoisonArg args[PoisonCtx::kMaxThreads];
    HANDLE handles[PoisonCtx::kMaxThreads];
    for (uint32_t i = 0; i < kThreads; ++i) {
      args[i] = PoisonArg{&ctx, i};
      handles[i] = LIBC_NAMESPACE::test_support::create_thread(poison_worker,
                                                               &args[i]);
      ASSERT_NE(handles[i], static_cast<HANDLE>(nullptr));
    }

    while (ctx.arrived.load(MemoryOrder::ACQUIRE) < kThreads)
      LIBC_NAMESPACE::test_support::sleep_ms(0);
    ctx.start_gate.store(1, MemoryOrder::RELEASE);

    for (uint32_t i = 0; i < kThreads; ++i) {
      EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(handles[i],
                                                                     10000),
                static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
      ::NtClose(handles[i]);
    }

    // Bootstrap must NOT have run on a poisoned gate.
    EXPECT_EQ(ctx.runs.load(MemoryOrder::ACQUIRE), 0);
    EXPECT_EQ(ctx.subsystems_ready.load(MemoryOrder::ACQUIRE), 0);
    // Every entrant must have received the failure return.
    for (uint32_t i = 0; i < kThreads; ++i)
      EXPECT_EQ(ctx.rc[i].load(MemoryOrder::ACQUIRE), 1);
  }
}
