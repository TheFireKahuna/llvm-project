//===-- Stress tests for Windows Futex and WaitSlot -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Exercises the crash-prone paths in the Treiber-stack futex:
//
//   1. Concurrent wait/wake ping-pong — verifies Treiber stack integrity
//      under rapid push/pop (CAS contention on head index).
//
//   2. Timeout + wake race — a waiter times out while a waker pops its
//      slot. Tests the TIMED_OUT→reclaim_slot path and the IN_KERNEL→SIGNALED
//      CAS race in claim_slot.
//
//   3. notify_all under contention — all waiters pushed onto the same
//      chain, then bulk-woken. Validates detach-entire-chain CAS and
//      sequential claim_slot walk.
//
//   4. store_and_notify atomicity — value change + pop in one CAS.
//      Verifies waiters see the new value after waking.
//
//   5. Rapid thread create/exit — stresses FLS cleanup (slot_cleanup)
//      racing with wakers calling reclaim_slot. This is the highest-risk
//      path: slot reuse between freelist and FLS callback.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "hdr/errno_macros.h"

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::Futex;
using LIBC_NAMESPACE::FutexValueType;

// ---------------------------------------------------------------------------
// 1. Ping-pong: two threads alternate incrementing a futex value.
//    Tests wait→push→kernel→claim→alert→wake cycle under tight contention.
// ---------------------------------------------------------------------------

static constexpr int PING_PONG_ROUNDS = 2000;

struct PingPongCtx {
  Futex ftx{0};
  Atomic<int> errors{0};
};

static DWORD ping_pong_even(void *arg) {
  auto *ctx = static_cast<PingPongCtx *>(arg);
  for (int i = 0; i < PING_PONG_ROUNDS; i++) {
    FutexValueType expected = static_cast<FutexValueType>(i * 2);
    // Wait until value == expected (our turn).
    while (ctx->ftx.load(MemoryOrder::ACQUIRE) != expected)
      ctx->ftx.wait(ctx->ftx.load(MemoryOrder::RELAXED));

    // Store next value and wake.
    ctx->ftx.store_and_notify(expected + 1);
  }
  return 0;
}

static DWORD ping_pong_odd(void *arg) {
  auto *ctx = static_cast<PingPongCtx *>(arg);
  for (int i = 0; i < PING_PONG_ROUNDS; i++) {
    FutexValueType expected = static_cast<FutexValueType>(i * 2 + 1);
    while (ctx->ftx.load(MemoryOrder::ACQUIRE) != expected)
      ctx->ftx.wait(ctx->ftx.load(MemoryOrder::RELAXED));

    ctx->ftx.store_and_notify(expected + 1);
  }
  return 0;
}

TEST(LlvmLibcFutexStressTest, PingPong) {
  PingPongCtx ctx;
  HANDLE t1 =
      LIBC_NAMESPACE::test_support::create_thread(ping_pong_even, &ctx);
  HANDLE t2 =
      LIBC_NAMESPACE::test_support::create_thread(ping_pong_odd, &ctx);
  ASSERT_NE(t1, static_cast<HANDLE>(nullptr));
  ASSERT_NE(t2, static_cast<HANDLE>(nullptr));

  DWORD r1 = LIBC_NAMESPACE::test_support::wait_for_single_object(t1, 30000);
  DWORD r2 = LIBC_NAMESPACE::test_support::wait_for_single_object(t2, 30000);
  EXPECT_EQ(r1,
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_EQ(r2,
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));

  // Final value should be 2 * PING_PONG_ROUNDS.
  EXPECT_EQ(ctx.ftx.get_value(),
            static_cast<FutexValueType>(PING_PONG_ROUNDS * 2));

  ::NtClose(t1);
  ::NtClose(t2);
}

// ---------------------------------------------------------------------------
// 2. Timeout + wake race: waiter uses a short timeout while another thread
//    wakes it. The race between TIMED_OUT and SIGNALED is the most delicate
//    path in the futex state machine.
// ---------------------------------------------------------------------------

struct TimeoutRaceCtx {
  Futex ftx{0};
  Atomic<int> woke_count{0};
  Atomic<int> timeout_count{0};
  Atomic<bool> done{false};
};

static DWORD timeout_waiter(void *arg) {
  auto *ctx = static_cast<TimeoutRaceCtx *>(arg);
  while (!ctx->done.load(MemoryOrder::ACQUIRE)) {
    auto timeout = LIBC_NAMESPACE::internal::AbsTimeout::from_timespec(
        {0, 500000}, /*is_realtime=*/false); // 0.5ms
    long ret = ctx->ftx.wait(0, timeout ? *timeout
                                        : LIBC_NAMESPACE::Futex::Timeout{});
    if (ret == 0)
      ctx->woke_count.fetch_add(1, MemoryOrder::RELAXED);
    else
      ctx->timeout_count.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

static DWORD timeout_waker(void *arg) {
  auto *ctx = static_cast<TimeoutRaceCtx *>(arg);
  while (!ctx->done.load(MemoryOrder::ACQUIRE)) {
    ctx->ftx.store_and_notify(1);
    ctx->ftx.store(0, MemoryOrder::RELEASE);
  }
  return 0;
}

TEST(LlvmLibcFutexStressTest, TimeoutWakeRace) {
  TimeoutRaceCtx ctx;
  constexpr int N_WAITERS = 4;
  constexpr int N_WAKERS = 2;
  HANDLE threads[N_WAITERS + N_WAKERS];

  for (int i = 0; i < N_WAITERS; i++)
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(timeout_waiter, &ctx);
  for (int i = 0; i < N_WAKERS; i++)
    threads[N_WAITERS + i] =
        LIBC_NAMESPACE::test_support::create_thread(timeout_waker, &ctx);

  // Let it run for ~500ms.
  LIBC_NAMESPACE::test_support::sleep_ms(500);
  ctx.done.store(true, MemoryOrder::RELEASE);

  // Wake all waiters so they can see the done flag.
  ctx.ftx.store_and_notify_all(1);

  for (int i = 0; i < N_WAITERS + N_WAKERS; i++) {
    DWORD r =
        LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 10000);
    EXPECT_EQ(r,
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(threads[i]);
  }

  // Verify some activity occurred (not stuck).
  int total = ctx.woke_count.load(MemoryOrder::RELAXED) +
              ctx.timeout_count.load(MemoryOrder::RELAXED);
  EXPECT_GT(total, 0);
}

// ---------------------------------------------------------------------------
// 3. notify_all storm: N threads all wait on the same futex, then one
//    thread calls notify_all. Tests bulk Treiber chain detach + walk.
// ---------------------------------------------------------------------------

struct NotifyAllCtx {
  Futex ftx{0};
  Atomic<int> arrived{0};
  Atomic<int> woke{0};
};

static DWORD notify_all_waiter(void *arg) {
  auto *ctx = static_cast<NotifyAllCtx *>(arg);
  ctx->arrived.fetch_add(1, MemoryOrder::RELEASE);
  // Wait for value change from 0 to 1.
  while (ctx->ftx.load(MemoryOrder::ACQUIRE) == 0)
    ctx->ftx.wait(0);
  ctx->woke.fetch_add(1, MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcFutexStressTest, NotifyAllStorm) {
  constexpr int N = 16;
  NotifyAllCtx ctx;
  HANDLE threads[N];

  for (int i = 0; i < N; i++)
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(notify_all_waiter, &ctx);

  // Wait for all threads to enter wait.
  while (ctx.arrived.load(MemoryOrder::ACQUIRE) < N)
    LIBC_NAMESPACE::test_support::sleep_ms(1);
  LIBC_NAMESPACE::test_support::sleep_ms(5); // Let them settle into kernel wait.

  // Wake all at once.
  ctx.ftx.store_and_notify_all(1);

  for (int i = 0; i < N; i++) {
    DWORD r =
        LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 10000);
    EXPECT_EQ(r,
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(threads[i]);
  }

  EXPECT_EQ(ctx.woke.load(MemoryOrder::RELAXED), N);
}

// ---------------------------------------------------------------------------
// 4. Rapid thread create/exit — stresses the WaitSlot FLS cleanup path.
//    Each thread does a single futex wait+timeout then exits. The slot
//    must be safely returned to the freelist despite concurrent wakers.
// ---------------------------------------------------------------------------

static Futex rapid_ftx(0);

static DWORD rapid_exit_thread(void *) {
  // Touch the futex subsystem (allocates a WaitSlot via FLS).
  auto timeout = LIBC_NAMESPACE::internal::AbsTimeout::from_timespec(
      {0, 100000}, /*is_realtime=*/false); // 0.1ms
  rapid_ftx.wait(0, timeout ? *timeout : LIBC_NAMESPACE::Futex::Timeout{});
  // Thread exits → FLS cleanup → slot_cleanup called.
  return 0;
}

TEST(LlvmLibcFutexStressTest, RapidThreadExitSlotRecycle) {
  constexpr int WAVES = 10;
  constexpr int THREADS_PER_WAVE = 32;

  for (int wave = 0; wave < WAVES; wave++) {
    HANDLE threads[THREADS_PER_WAVE];
    for (int i = 0; i < THREADS_PER_WAVE; i++)
      threads[i] =
          LIBC_NAMESPACE::test_support::create_thread(rapid_exit_thread,
                                                      nullptr);

    // Concurrently try to wake (stresses reclaim_slot path).
    rapid_ftx.notify_all();

    for (int i = 0; i < THREADS_PER_WAVE; i++) {
      LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 5000);
      ::NtClose(threads[i]);
    }
  }

  // All threads exited: every WaitSlot they claimed must have been
  // returned to the pool. If a slot leaked, has_waiters() would still
  // report a dangling entry even though no thread is parked on it.
  EXPECT_FALSE(rapid_ftx.has_waiters());
  EXPECT_EQ(rapid_ftx.load(MemoryOrder::ACQUIRE),
            static_cast<FutexValueType>(0));
}

// ---------------------------------------------------------------------------
// 5. Multi-futex contention: threads bounce between two futexes.
//    Tests that wait queue integrity is maintained when the same thread
//    uses different futexes in rapid succession.
// ---------------------------------------------------------------------------

struct MultiFutexCtx {
  Futex ftx_a{0};
  Futex ftx_b{0};
  Atomic<bool> done{false};
  Atomic<int> iterations{0};
};

static DWORD multi_futex_worker(void *arg) {
  auto *ctx = static_cast<MultiFutexCtx *>(arg);
  while (!ctx->done.load(MemoryOrder::ACQUIRE)) {
    // Wait on A, then B, alternating.
    auto timeout = LIBC_NAMESPACE::internal::AbsTimeout::from_timespec(
        {0, 200000}, /*is_realtime=*/false);
    ctx->ftx_a.wait(0, timeout ? *timeout : LIBC_NAMESPACE::Futex::Timeout{});
    ctx->ftx_b.wait(0, timeout ? *timeout : LIBC_NAMESPACE::Futex::Timeout{});
    ctx->iterations.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST(LlvmLibcFutexStressTest, MultiFutexBounce) {
  MultiFutexCtx ctx;
  constexpr int N = 8;
  HANDLE threads[N];

  for (int i = 0; i < N; i++)
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(multi_futex_worker, &ctx);

  // Randomly wake futex A and B for 300ms.
  for (int i = 0; i < 300; i++) {
    ctx.ftx_a.store_and_notify_all(1);
    ctx.ftx_a.store(0);
    ctx.ftx_b.store_and_notify_all(1);
    ctx.ftx_b.store(0);
    LIBC_NAMESPACE::test_support::sleep_ms(1);
  }

  ctx.done.store(true, MemoryOrder::RELEASE);
  ctx.ftx_a.store_and_notify_all(1);
  ctx.ftx_b.store_and_notify_all(1);

  for (int i = 0; i < N; i++) {
    LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 10000);
    ::NtClose(threads[i]);
  }

  EXPECT_GT(ctx.iterations.load(MemoryOrder::RELAXED), 0);
}
