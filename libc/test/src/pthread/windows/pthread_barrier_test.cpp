//===-- Windows unittests for pthread_barrier_* ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - pthread_barrier_init / pthread_barrier_destroy
//   - Exactly one thread gets PTHREAD_BARRIER_SERIAL_THREAD; the rest get 0
//   - All N threads proceed only after all N have called barrier_wait
//   - The barrier is reusable across multiple waves
//
//===----------------------------------------------------------------------===//

#include "src/pthread/pthread_barrier_destroy.h"
#include "src/pthread/pthread_barrier_init.h"
#include "src/pthread/pthread_barrier_wait.h"
#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_join.h"
#include "src/__support/CPP/atomic.h"
#include "test/UnitTest/Test.h"

#include <pthread.h>

// init with count=0 must fail.
TEST(LlvmLibcWindowsPthreadBarrierTest, ZeroCount) {
  pthread_barrier_t b;
  EXPECT_NE(LIBC_NAMESPACE::pthread_barrier_init(&b, nullptr, 0), 0);
}

// N threads all arrive; exactly one receives PTHREAD_BARRIER_SERIAL_THREAD.
TEST(LlvmLibcWindowsPthreadBarrierTest, NThreadsAllProceed) {
  constexpr int N = 4;
  struct State {
    pthread_barrier_t barrier;
    LIBC_NAMESPACE::cpp::Atomic<int> serial_count{0};
    LIBC_NAMESPACE::cpp::Atomic<int> arrived{0};
  };
  State s;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_barrier_init(&s.barrier, nullptr, N), 0);

  auto worker = [](void *arg) -> void * {
    auto *st = static_cast<State *>(arg);
    st->arrived.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);
    int ret = LIBC_NAMESPACE::pthread_barrier_wait(&st->barrier);
    if (ret == PTHREAD_BARRIER_SERIAL_THREAD)
      st->serial_count.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);
    return nullptr;
  };

  pthread_t threads[N];
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&threads[i], nullptr, worker, &s), 0);
  for (int i = 0; i < N; ++i)
    LIBC_NAMESPACE::pthread_join(threads[i], nullptr);

  EXPECT_EQ(s.arrived.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), N);
  EXPECT_EQ(s.serial_count.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), 1);

  LIBC_NAMESPACE::pthread_barrier_destroy(&s.barrier);
}

// The barrier resets and is usable for a second wave of N threads.
TEST(LlvmLibcWindowsPthreadBarrierTest, Reusable) {
  constexpr int N = 3;
  constexpr int WAVES = 2;

  struct State {
    pthread_barrier_t barrier;
    LIBC_NAMESPACE::cpp::Atomic<int> completions{0};
  };
  State s;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_barrier_init(&s.barrier, nullptr, N), 0);

  auto worker = [](void *arg) -> void * {
    auto *st = static_cast<State *>(arg);
    // Two waves.
    for (int w = 0; w < WAVES; ++w) {
      LIBC_NAMESPACE::pthread_barrier_wait(&st->barrier);
      st->completions.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);
    }
    return nullptr;
  };

  pthread_t threads[N];
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&threads[i], nullptr, worker, &s), 0);
  for (int i = 0; i < N; ++i)
    LIBC_NAMESPACE::pthread_join(threads[i], nullptr);

  EXPECT_EQ(s.completions.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), N * WAVES);
  LIBC_NAMESPACE::pthread_barrier_destroy(&s.barrier);
}
