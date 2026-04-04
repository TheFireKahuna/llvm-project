//===-- Unittests for Windows Barrier -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/barrier.h"
#include "src/__support/threads/thread.h"
#include "src/__support/CPP/atomic.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"
#include "hdr/pthread_macros.h"

// init with count=0 must fail.
TEST(LlvmLibcWindowsBarrierTest, InitZeroCount) {
  LIBC_NAMESPACE::Barrier b;
  EXPECT_NE(LIBC_NAMESPACE::Barrier::init(&b, nullptr, 0), 0);
}

// init with PTHREAD_PROCESS_SHARED must fail (not supported).
TEST(LlvmLibcWindowsBarrierTest, InitProcessSharedUnsupported) {
  LIBC_NAMESPACE::Barrier b;
  pthread_barrierattr_t attr;
  attr.pshared = PTHREAD_PROCESS_SHARED;
  EXPECT_NE(LIBC_NAMESPACE::Barrier::init(&b, &attr, 2), 0);
}

// Single-thread barrier with count=1 returns PTHREAD_BARRIER_SERIAL_THREAD.
TEST(LlvmLibcWindowsBarrierTest, SingleThread) {
  LIBC_NAMESPACE::Barrier b;
  ASSERT_EQ(LIBC_NAMESPACE::Barrier::init(&b, nullptr, 1), 0);
  int ret = b.wait();
  EXPECT_EQ(ret, PTHREAD_BARRIER_SERIAL_THREAD);
  LIBC_NAMESPACE::Barrier::destroy(&b);
}

// N threads all call wait(); exactly one gets PTHREAD_BARRIER_SERIAL_THREAD,
// the rest get 0. All must proceed only after all N have arrived.
struct BarrierState {
  LIBC_NAMESPACE::Barrier barrier;
  LIBC_NAMESPACE::cpp::Atomic<int> arrived{0};
  LIBC_NAMESPACE::cpp::Atomic<int> serial_count{0};
  LIBC_NAMESPACE::cpp::Atomic<int> post_barrier{0};
};

static void *barrier_thread(void *arg) {
  auto *s = static_cast<BarrierState *>(arg);
  s->arrived.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
  int ret = s->barrier.wait();
  if (ret == PTHREAD_BARRIER_SERIAL_THREAD)
    s->serial_count.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
  // After wait() returns, all N threads must have arrived.
  int n = s->arrived.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE);
  if (n == 4) // 4 = N threads
    s->post_barrier.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
  return nullptr;
}

TEST(LlvmLibcWindowsBarrierTest, NThreadsAllProceed) {
  constexpr int N = 4;
  BarrierState s;
  ASSERT_EQ(LIBC_NAMESPACE::Barrier::init(&s.barrier, nullptr, N), 0);

  LIBC_NAMESPACE::Thread threads[N];
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(threads[i].run(barrier_thread, &s), 0);
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(threads[i].join(static_cast<void **>(nullptr)), 0);

  // Exactly one thread gets the serial token.
  EXPECT_EQ(s.serial_count.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), 1);
  // All threads verified all N had arrived before returning from wait().
  EXPECT_EQ(s.post_barrier.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), N);

  LIBC_NAMESPACE::Barrier::destroy(&s.barrier);
}

// Reuse: barrier can be used for a second wave of N threads.
TEST(LlvmLibcWindowsBarrierTest, Reusable) {
  constexpr int N = 3;
  LIBC_NAMESPACE::Barrier b;
  ASSERT_EQ(LIBC_NAMESPACE::Barrier::init(&b, nullptr, N), 0);

  for (int wave = 0; wave < 2; ++wave) {
    LIBC_NAMESPACE::cpp::Atomic<int> count{0};
    struct WaveArg { LIBC_NAMESPACE::Barrier *b; LIBC_NAMESPACE::cpp::Atomic<int> *c; };
    WaveArg arg{&b, &count};
    LIBC_NAMESPACE::Thread threads[N];
    for (int i = 0; i < N; ++i) {
      ASSERT_EQ(threads[i].run(+[](void *p) -> void * {
        auto *a = static_cast<WaveArg *>(p);
        a->b->wait();
        a->c->fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
        return nullptr;
      }, &arg), 0);
    }
    for (int i = 0; i < N; ++i)
      ASSERT_EQ(threads[i].join(static_cast<void **>(nullptr)), 0);
    EXPECT_EQ(count.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), N);
  }

  LIBC_NAMESPACE::Barrier::destroy(&b);
}
