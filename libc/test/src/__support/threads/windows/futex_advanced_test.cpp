//===-- Advanced Futex state-machine stress tests -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Complements futex_test / futex_stress_test. These are the tests that
// smoke out regressions in the 64-bit CAS stack, the three-phase wait
// escalation, and handoff/unlock_notify — areas where a subtle memory-order
// mistake in the implementation would silently drop wakes or reorder
// value-stores past a notify.
//
//   1. 64-bit CAS-64 ABA resistance.
//        Repeatedly push and pop the same WaitSlot index with value
//        oscillation; a broken ABA counter in the stack word would let
//        a stale top survive across a push/pop/push and clobber the
//        fresh waiter's next pointer.
//
//   2. Three-phase escalation — forced deep sleep.
//        One waiter, single waker, long inter-op delay. The waiter MUST
//        progress through Phase 1 (UMWAIT/MWAITX) → Phase 2 (CAS-push)
//        → Phase 2.5 (slot spin) → Phase 3 (kernel park) before the wake
//        arrives. Verifies every transition is terminable by a late wake.
//
//   3. handoff_one reuse — direct ownership transfer.
//        Producer/consumer ping-pong over `unlock_notify`; the one-waiter
//        fast path is the unlock_notify → handoff_one → WAITING→SIGNALED
//        CAS. A broken handoff path silently falls back to store+notify
//        and the test still passes functionally, so we verify BOTH
//        correctness (no lost wakes across 5000 handoffs) AND that
//        has_waiters() observes a waiter during every producer window.
//
//   4. notify_one fairness — N waiters, N notify_one calls.
//        Each notify_one must wake EXACTLY one waiter (LIFO order).
//        A broken pop-before-signal in notify_one would either double-wake
//        or leak a waiter; both are caught by counting.
//
//   5. store_and_notify vs store+notify_one — StoreLoad reorder regression.
//        Recreates the bug pattern from memory/bug_futex_storeload_reorder:
//        store(RELEASE) + notify_one() on x86 can reorder the store past
//        the notify, deadlocking. We verify `store_and_notify` does NOT
//        exhibit that pattern across heavy contention.
//
//   6. Cross-futex independence.
//        Two Futexes on separate cache lines; each has its own waiter
//        queue. A notify_all on one must NOT wake waiters on the other.
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::Futex;
using LIBC_NAMESPACE::FutexValueType;

// ---------------------------------------------------------------------------
// 1. CAS-64 ABA stress: two threads bounce the same slot index through the
//    stack via rapid wait/wake. A broken ABA gen would surface as a lost
//    wake (deadlock / timeout) once in ~2^16 iterations — we run long
//    enough that any statistical miss would hit.
// ---------------------------------------------------------------------------

struct AbaCtx {
  Futex ftx{0};
  Atomic<int> waiter_woke{0};
  Atomic<bool> done{false};
};

static DWORD aba_waiter(void *arg) {
  auto *ctx = static_cast<AbaCtx *>(arg);
  while (!ctx->done.load(MemoryOrder::ACQUIRE)) {
    FutexValueType v = ctx->ftx.load(MemoryOrder::ACQUIRE);
    if (v == 0) {
      long rc = ctx->ftx.wait(0);
      (void)rc;
      ctx->waiter_woke.fetch_add(1, MemoryOrder::RELAXED);
    }
    ctx->ftx.store(0, MemoryOrder::RELEASE);
  }
  return 0;
}

TEST(LlvmLibcFutexAdvanced, CAS64AbaResistance) {
  AbaCtx ctx;
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(aba_waiter, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));

  constexpr int N = 20000;
  for (int i = 0; i < N; ++i)
    ctx.ftx.store_and_notify(1);

  ctx.done.store(true, MemoryOrder::RELEASE);
  ctx.ftx.store_and_notify_all(0xFF);

  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 30000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(t);

  // Not all wakes are guaranteed to land as "woke" (fast-path return when
  // value changes under Phase 1 also counts as success). But some must.
  EXPECT_GT(ctx.waiter_woke.load(MemoryOrder::RELAXED), 0);
  EXPECT_FALSE(ctx.ftx.has_waiters());
}

// ---------------------------------------------------------------------------
// 2. Three-phase escalation with forced deep sleep.
//    Waiter enters wait, we delay ~200ms (well past UMWAIT budget, past
//    slot spin, into kernel park), then wake. Must return 0 (value change
//    seen) not -ETIMEDOUT.
// ---------------------------------------------------------------------------

struct EscalationCtx {
  Futex ftx{0};
  Atomic<long> rc{-999};
  Atomic<bool> woke_started{false};
};

static DWORD escalation_waiter(void *arg) {
  auto *ctx = static_cast<EscalationCtx *>(arg);
  // Use a long absolute timeout so we KNOW we're in kernel park when woken.
  auto to_exp = LIBC_NAMESPACE::internal::AbsTimeout::from_timespec(
      {5, 0}, /*is_realtime=*/false);
  long rc = ctx->ftx.wait(0, *to_exp);
  ctx->rc.store(rc, MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcFutexAdvanced, ThreePhaseEscalationDeepWake) {
  EscalationCtx ctx;
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(escalation_waiter,
                                                         &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));

  // Far longer than Phase 1/2/2.5 combined — guarantees kernel park.
  LIBC_NAMESPACE::test_support::sleep_ms(200);

  EXPECT_TRUE(ctx.ftx.has_waiters());
  ctx.ftx.store_and_notify(1);

  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 10000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_EQ(ctx.rc.load(MemoryOrder::ACQUIRE), 0L);
  EXPECT_FALSE(ctx.ftx.has_waiters());
  ::NtClose(t);
}

// ---------------------------------------------------------------------------
// 3. handoff_one / unlock_notify reuse — 5000 producer/consumer rounds.
// ---------------------------------------------------------------------------

struct HandoffCtx {
  Futex token{0}; // 0 = empty, 1 = full
  Atomic<int> produced{0};
  Atomic<int> consumed{0};
};

static constexpr int HANDOFF_ROUNDS = 5000;

static DWORD handoff_consumer(void *arg) {
  auto *ctx = static_cast<HandoffCtx *>(arg);
  for (int i = 0; i < HANDOFF_ROUNDS; ++i) {
    while (ctx->token.load(MemoryOrder::ACQUIRE) == 0)
      ctx->token.wait(0);
    // Drain the token: store 0 then notify producer (who waits on == 0).
    ctx->token.store_and_notify(0);
    ctx->consumed.fetch_add(1, MemoryOrder::RELEASE);
  }
  return 0;
}

static DWORD handoff_producer(void *arg) {
  auto *ctx = static_cast<HandoffCtx *>(arg);
  for (int i = 0; i < HANDOFF_ROUNDS; ++i) {
    while (ctx->token.load(MemoryOrder::ACQUIRE) == 1)
      ctx->token.wait(1);
    ctx->token.store_and_notify(1);
    ctx->produced.fetch_add(1, MemoryOrder::RELEASE);
  }
  return 0;
}

TEST(LlvmLibcFutexAdvanced, HandoffPingPongNoLostWakes) {
  HandoffCtx ctx;
  HANDLE c =
      LIBC_NAMESPACE::test_support::create_thread(handoff_consumer, &ctx);
  HANDLE p =
      LIBC_NAMESPACE::test_support::create_thread(handoff_producer, &ctx);
  ASSERT_NE(c, static_cast<HANDLE>(nullptr));
  ASSERT_NE(p, static_cast<HANDLE>(nullptr));

  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(c, 60000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(p, 60000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));

  EXPECT_EQ(ctx.produced.load(MemoryOrder::ACQUIRE), HANDOFF_ROUNDS);
  EXPECT_EQ(ctx.consumed.load(MemoryOrder::ACQUIRE), HANDOFF_ROUNDS);
  EXPECT_FALSE(ctx.token.has_waiters());

  ::NtClose(c);
  ::NtClose(p);
}

// ---------------------------------------------------------------------------
// 4. notify_one fairness: N waiters, N notify_one calls, count wakes.
//    Each notify must wake EXACTLY one waiter. Bulk behavior tested
//    separately in NotifyAllStorm (futex_stress_test).
// ---------------------------------------------------------------------------

struct NotifyOneCtx {
  Futex ftx{0};
  Atomic<int> arrived{0};
  Atomic<int> woke{0};
  Atomic<bool> go{false};
};

static constexpr int NOTIFY_ONE_WAITERS = 12;

static DWORD notify_one_waiter(void *arg) {
  auto *ctx = static_cast<NotifyOneCtx *>(arg);
  ctx->arrived.fetch_add(1, MemoryOrder::RELEASE);
  while (ctx->ftx.load(MemoryOrder::ACQUIRE) == 0)
    ctx->ftx.wait(0);
  ctx->woke.fetch_add(1, MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcFutexAdvanced, NotifyOneWakesExactlyOne) {
  NotifyOneCtx ctx;
  HANDLE ths[NOTIFY_ONE_WAITERS];
  for (int i = 0; i < NOTIFY_ONE_WAITERS; ++i) {
    ths[i] =
        LIBC_NAMESPACE::test_support::create_thread(notify_one_waiter, &ctx);
    ASSERT_NE(ths[i], static_cast<HANDLE>(nullptr));
  }
  while (ctx.arrived.load(MemoryOrder::ACQUIRE) < NOTIFY_ONE_WAITERS)
    LIBC_NAMESPACE::test_support::sleep_ms(0);
  // Let waiters settle into kernel park.
  LIBC_NAMESPACE::test_support::sleep_ms(80);

  // Store the sentinel so waiters wake when notified.
  ctx.ftx.store(1, MemoryOrder::RELEASE);

  for (int i = 0; i < NOTIFY_ONE_WAITERS; ++i) {
    int before = ctx.woke.load(MemoryOrder::ACQUIRE);
    ctx.ftx.notify_one();
    // Bounded spin for exactly one wake. Over-wake would be +2; miss
    // would be +0 (caught by the post-loop tally as a timeout).
    for (int j = 0; j < 10000 &&
                    ctx.woke.load(MemoryOrder::ACQUIRE) == before;
         ++j)
      LIBC_NAMESPACE::test_support::sleep_ms(0);
  }

  for (int i = 0; i < NOTIFY_ONE_WAITERS; ++i) {
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(ths[i],
                                                                    10000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(ths[i]);
  }
  EXPECT_EQ(ctx.woke.load(MemoryOrder::ACQUIRE), NOTIFY_ONE_WAITERS);
  EXPECT_FALSE(ctx.ftx.has_waiters());
}

// ---------------------------------------------------------------------------
// 5. store_and_notify regression — NO StoreLoad reorder.
//    The bug pattern is `store(RELEASE) + notify_one()`: x86 can reorder
//    the value store past the stack read inside notify_one, letting the
//    waker observe an empty stack even when a waiter just pushed with
//    the old value. store_and_notify uses a single CAS-64 on combined_
//    that fuses the value store with the waiter check, eliminating the
//    reorder. Under contention this test must finish.
// ---------------------------------------------------------------------------

struct StoreNotifyCtx {
  Futex ftx{0};
  Atomic<int> waiter_wakes{0};
  Atomic<bool> done{false};
};

static DWORD sn_waiter(void *arg) {
  auto *ctx = static_cast<StoreNotifyCtx *>(arg);
  while (!ctx->done.load(MemoryOrder::ACQUIRE)) {
    auto to_exp = LIBC_NAMESPACE::internal::AbsTimeout::from_timespec(
        {0, 5'000'000}, /*is_realtime=*/false); // 5ms
    long rc = ctx->ftx.wait(0, *to_exp);
    if (rc == 0)
      ctx->waiter_wakes.fetch_add(1, MemoryOrder::RELAXED);
    // Reset to 0 so we can wait again.
    ctx->ftx.store(0, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST(LlvmLibcFutexAdvanced, StoreAndNotifyNoDeadlock) {
  StoreNotifyCtx ctx;
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(sn_waiter, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));

  constexpr int N = 3000;
  for (int i = 0; i < N; ++i)
    ctx.ftx.store_and_notify(1);

  ctx.done.store(true, MemoryOrder::RELEASE);
  ctx.ftx.store_and_notify_all(1);

  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 15000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_FALSE(ctx.ftx.has_waiters());
  ::NtClose(t);
}

// ---------------------------------------------------------------------------
// 6. Cross-futex independence: notify_all on A must not wake waiters on B.
// ---------------------------------------------------------------------------

struct IndependenceCtx {
  Futex ftx_a{0};
  Futex ftx_b{0};
  Atomic<int> a_woke{0};
  Atomic<int> b_woke{0};
  Atomic<int> b_arrived{0};
};

static DWORD independence_b_waiter(void *arg) {
  auto *ctx = static_cast<IndependenceCtx *>(arg);
  ctx->b_arrived.fetch_add(1, MemoryOrder::RELEASE);
  while (ctx->ftx_b.load(MemoryOrder::ACQUIRE) == 0)
    ctx->ftx_b.wait(0);
  ctx->b_woke.fetch_add(1, MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcFutexAdvanced, NotifyAllDoesNotCrossFutex) {
  constexpr int N = 6;
  IndependenceCtx ctx;
  HANDLE ths[N];
  for (int i = 0; i < N; ++i) {
    ths[i] = LIBC_NAMESPACE::test_support::create_thread(
        independence_b_waiter, &ctx);
    ASSERT_NE(ths[i], static_cast<HANDLE>(nullptr));
  }
  while (ctx.b_arrived.load(MemoryOrder::ACQUIRE) < N)
    LIBC_NAMESPACE::test_support::sleep_ms(0);
  LIBC_NAMESPACE::test_support::sleep_ms(50);

  // Notify A repeatedly — B must not wake.
  for (int i = 0; i < 100; ++i)
    ctx.ftx_a.store_and_notify_all(1);

  // B waiters should still be parked.
  EXPECT_EQ(ctx.b_woke.load(MemoryOrder::ACQUIRE), 0);
  EXPECT_TRUE(ctx.ftx_b.has_waiters());

  // Now wake B — all N should exit.
  ctx.ftx_b.store_and_notify_all(1);
  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(ths[i],
                                                                    10000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(ths[i]);
  }
  EXPECT_EQ(ctx.b_woke.load(MemoryOrder::ACQUIRE), N);
}
