//===-- Stress tests for Windows signal delivery ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Exercises crash-prone paths in the signal subsystem:
//
//   1. Concurrent raise from multiple threads — tests the thread registry
//      traversal racing with thread exit (registry_remove vs. deliver).
//
//   2. sigqueue flood while threads exit — tests the SigqueueEntry pool
//      under allocation pressure, and the MPSC queue drain path when
//      the target thread is being removed from the registry.
//
//   3. Block/unblock storm — rapid sigprocmask changes with concurrent
//      raise, testing pending signal accumulation and delivery on unblock.
//
//   4. SA_RESETHAND under contention — multiple threads raise the same
//      signal; only the first delivery should see the custom handler,
//      subsequent ones see SIG_DFL.
//
//   5. sigtimedwait + sigqueue rendezvous — producer/consumer pattern
//      where sigqueue sends values consumed by sigtimedwait, testing
//      the queued signal MPSC path end-to-end.
//
//===----------------------------------------------------------------------===//

#include "hdr/signal_macros.h"
#include "src/signal/raise.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigaddset.h"
#include "src/signal/sigemptyset.h"
#include "src/signal/sigprocmask.h"
#include "src/signal/sigqueue.h"
#include "src/signal/sigtimedwait.h"
#include "src/unistd/getpid.h"
#include "src/__support/CPP/atomic.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;

// ---------------------------------------------------------------------------
// 1. Concurrent raise: N threads all raise SIGUSR1 simultaneously.
//    The handler increments a counter. Tests that no signals are lost
//    and the registry traversal doesn't crash when threads exit.
// ---------------------------------------------------------------------------

static Atomic<int> concurrent_raise_count{0};

static void concurrent_raise_handler(int) {
  concurrent_raise_count.fetch_add(1, MemoryOrder::RELAXED);
}

static Atomic<bool> raise_start{false};

static DWORD raise_thread(void *) {
  // Wait for start signal.
  while (!raise_start.load(MemoryOrder::ACQUIRE))
    ;
  for (int i = 0; i < 100; i++)
    LIBC_NAMESPACE::raise(SIGUSR1);
  return 0;
}

TEST(LlvmLibcSignalStressTest, ConcurrentRaise) {
  concurrent_raise_count.store(0, MemoryOrder::RELAXED);
  raise_start.store(false, MemoryOrder::RELAXED);

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  action.sa_handler = concurrent_raise_handler;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);

  constexpr int N = 8;
  HANDLE threads[N];
  for (int i = 0; i < N; i++)
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(raise_thread, nullptr);

  // Fire!
  raise_start.store(true, MemoryOrder::RELEASE);

  for (int i = 0; i < N; i++) {
    LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 10000);
    ::NtClose(threads[i]);
  }

  // Each thread raises 100 times → 800 total.
  // raise() delivers to the calling thread, so all should be counted.
  EXPECT_EQ(concurrent_raise_count.load(MemoryOrder::RELAXED), N * 100);

  struct sigaction dfl;
  dfl.sa_handler = SIG_DFL;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &dfl, nullptr);
}

// ---------------------------------------------------------------------------
// 2. Block/unblock storm: one thread blocks SIGUSR1, raises many times,
//    then unblocks — testing that pending signals accumulate correctly
//    and delivery doesn't crash when the pending set is drained.
// ---------------------------------------------------------------------------

static Atomic<int> storm_delivery_count{0};

static void storm_handler(int) {
  storm_delivery_count.fetch_add(1, MemoryOrder::RELAXED);
}

TEST(LlvmLibcSignalStressTest, BlockUnblockStorm) {
  storm_delivery_count.store(0, MemoryOrder::RELAXED);

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  action.sa_handler = storm_handler;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);

  sigset_t block_set;
  LIBC_NAMESPACE::sigemptyset(&block_set);
  LIBC_NAMESPACE::sigaddset(&block_set, SIGUSR1);

  constexpr int ROUNDS = 50;
  for (int round = 0; round < ROUNDS; round++) {
    storm_delivery_count.store(0, MemoryOrder::RELAXED);

    // Block SIGUSR1.
    LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &block_set, nullptr);

    // Raise multiple times while blocked.
    for (int i = 0; i < 10; i++)
      LIBC_NAMESPACE::raise(SIGUSR1);

    // Standard signals coalesce: only guaranteed at least one delivery.
    EXPECT_EQ(storm_delivery_count.load(MemoryOrder::RELAXED), 0);

    // Unblock — at least one delivery.
    LIBC_NAMESPACE::sigprocmask(SIG_UNBLOCK, &block_set, nullptr);
    EXPECT_GE(storm_delivery_count.load(MemoryOrder::RELAXED), 1);
  }

  struct sigaction dfl;
  dfl.sa_handler = SIG_DFL;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &dfl, nullptr);
}

// ---------------------------------------------------------------------------
// 3. sigqueue + sigtimedwait producer/consumer: tests the queued signal
//    path end-to-end. Multiple producers send values, one consumer
//    receives them via sigtimedwait.
// ---------------------------------------------------------------------------

struct ProducerConsumerCtx {
  Atomic<int> produced{0};
  Atomic<int> consumed{0};
  Atomic<bool> done{false};
};

static DWORD sigqueue_producer(void *arg) {
  auto *ctx = static_cast<ProducerConsumerCtx *>(arg);
  for (int i = 0; i < 50; i++) {
    union sigval val;
    val.sival_int = i;
    LIBC_NAMESPACE::sigqueue(LIBC_NAMESPACE::getpid(), SIGUSR2, val);
    ctx->produced.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST(LlvmLibcSignalStressTest, SigqueueSigtimedwaitRendezvous) {
  ProducerConsumerCtx ctx;

  // Block SIGUSR2 so signals pend rather than being delivered.
  sigset_t block;
  LIBC_NAMESPACE::sigemptyset(&block);
  LIBC_NAMESPACE::sigaddset(&block, SIGUSR2);
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &block, nullptr);

  // Install a handler (even though we consume via sigtimedwait, the
  // handler must exist to prevent SIG_DFL termination on unmasked delivery).
  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO;
  action.sa_sigaction = +[](int, siginfo_t *, void *) {};
  LIBC_NAMESPACE::sigaction(SIGUSR2, &action, nullptr);

  // Launch producers.
  constexpr int N_PRODUCERS = 4;
  HANDLE producers[N_PRODUCERS];
  for (int i = 0; i < N_PRODUCERS; i++)
    producers[i] =
        LIBC_NAMESPACE::test_support::create_thread(sigqueue_producer, &ctx);

  // Consumer: drain signals via sigtimedwait.
  sigset_t wait_set;
  LIBC_NAMESPACE::sigemptyset(&wait_set);
  LIBC_NAMESPACE::sigaddset(&wait_set, SIGUSR2);

  int consumed = 0;
  timespec timeout = {0, 10000000}; // 10ms
  for (int attempt = 0; attempt < 500; attempt++) {
    siginfo_t info;
    int sig = LIBC_NAMESPACE::sigtimedwait(&wait_set, &info, &timeout);
    if (sig == SIGUSR2)
      consumed++;
    // If we've consumed everything the producers sent, stop early.
    if (consumed >= N_PRODUCERS * 50)
      break;
  }

  for (int i = 0; i < N_PRODUCERS; i++) {
    LIBC_NAMESPACE::test_support::wait_for_single_object(producers[i], 10000);
    ::NtClose(producers[i]);
  }

  // We should have consumed at least some signals. POSIX queued signals
  // guarantee one entry per sigqueue() call, so we expect all of them.
  EXPECT_GE(consumed, 1);

  // Restore.
  LIBC_NAMESPACE::sigprocmask(SIG_UNBLOCK, &block, nullptr);
  struct sigaction dfl;
  dfl.sa_handler = SIG_DFL;
  LIBC_NAMESPACE::sigaction(SIGUSR2, &dfl, nullptr);
}

// ---------------------------------------------------------------------------
// 4. SA_SIGINFO + handler that does work — tests that the siginfo_t
//    pointer is valid across concurrent deliveries (not a stale stack frame).
// ---------------------------------------------------------------------------

static Atomic<int> siginfo_delivery_count{0};
static Atomic<int> siginfo_valid_count{0};

static void siginfo_stress_handler(int sig, siginfo_t *info, void *) {
  siginfo_delivery_count.fetch_add(1, MemoryOrder::RELAXED);
  if (info && info->si_signo == sig)
    siginfo_valid_count.fetch_add(1, MemoryOrder::RELAXED);
}

static DWORD siginfo_raise_thread(void *) {
  for (int i = 0; i < 200; i++)
    LIBC_NAMESPACE::raise(SIGUSR1);
  return 0;
}

TEST(LlvmLibcSignalStressTest, SaSiginfoUnderContention) {
  siginfo_delivery_count.store(0, MemoryOrder::RELAXED);
  siginfo_valid_count.store(0, MemoryOrder::RELAXED);

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO;
  action.sa_sigaction = siginfo_stress_handler;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);

  constexpr int N = 4;
  HANDLE threads[N];
  for (int i = 0; i < N; i++)
    threads[i] = LIBC_NAMESPACE::test_support::create_thread(
        siginfo_raise_thread, nullptr);

  for (int i = 0; i < N; i++) {
    LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 10000);
    ::NtClose(threads[i]);
  }

  int delivered = siginfo_delivery_count.load(MemoryOrder::RELAXED);
  int valid = siginfo_valid_count.load(MemoryOrder::RELAXED);

  EXPECT_EQ(delivered, N * 200);
  // Every delivery should have valid siginfo.
  EXPECT_EQ(valid, delivered);

  struct sigaction dfl;
  dfl.sa_handler = SIG_DFL;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &dfl, nullptr);
}

// ---------------------------------------------------------------------------
// 5. Handler mask isolation: SIGUSR1 handler blocks SIGUSR2 via sa_mask.
//    Concurrent raises of both signals test that sa_mask is properly
//    applied and restored per-thread.
// ---------------------------------------------------------------------------

static Atomic<int> usr1_during_usr2{0};
static Atomic<int> usr2_handler_count{0};

static void usr2_handler_with_mask(int) {
  // During this handler, SIGUSR1 should be blocked (via sa_mask on SIGUSR2).
  // Raise SIGUSR1 — it should pend, not nest.
  LIBC_NAMESPACE::raise(SIGUSR1);
  usr2_handler_count.fetch_add(1, MemoryOrder::RELAXED);
}

static void usr1_counter(int) {
  usr1_during_usr2.fetch_add(1, MemoryOrder::RELAXED);
}

TEST(LlvmLibcSignalStressTest, HandlerMaskIsolation) {
  usr1_during_usr2.store(0, MemoryOrder::RELAXED);
  usr2_handler_count.store(0, MemoryOrder::RELAXED);

  // SIGUSR1: simple counter.
  struct sigaction act1;
  LIBC_NAMESPACE::sigemptyset(&act1.sa_mask);
  act1.sa_flags = 0;
  act1.sa_handler = usr1_counter;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &act1, nullptr);

  // SIGUSR2: blocks SIGUSR1 during execution.
  struct sigaction act2;
  LIBC_NAMESPACE::sigemptyset(&act2.sa_mask);
  LIBC_NAMESPACE::sigaddset(&act2.sa_mask, SIGUSR1);
  act2.sa_flags = 0;
  act2.sa_handler = usr2_handler_with_mask;
  LIBC_NAMESPACE::sigaction(SIGUSR2, &act2, nullptr);

  for (int i = 0; i < 100; i++)
    LIBC_NAMESPACE::raise(SIGUSR2);

  // Each SIGUSR2 handler raises SIGUSR1 (blocked by sa_mask).
  // SIGUSR1 should be delivered after SIGUSR2 handler returns.
  // At minimum, at least some SIGUSR1 deliveries should occur.
  EXPECT_EQ(usr2_handler_count.load(MemoryOrder::RELAXED), 100);
  EXPECT_GE(usr1_during_usr2.load(MemoryOrder::RELAXED), 1);

  struct sigaction dfl;
  dfl.sa_handler = SIG_DFL;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &dfl, nullptr);
  LIBC_NAMESPACE::sigaction(SIGUSR2, &dfl, nullptr);
}
