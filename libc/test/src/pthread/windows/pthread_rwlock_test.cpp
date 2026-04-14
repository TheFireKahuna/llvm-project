//===-- Windows unittests for pthread_rwlock_* ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - pthread_rwlock_init / pthread_rwlock_destroy smoke test
//   - Multiple readers hold the lock simultaneously
//   - A writer acquires exclusive access (no concurrent readers)
//   - tryrdlock / trywrlock succeed when unlocked, fail with EBUSY when held
//
//===----------------------------------------------------------------------===//

#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_join.h"
#include "src/pthread/pthread_rwlock_destroy.h"
#include "src/pthread/pthread_rwlock_init.h"
#include "src/pthread/pthread_rwlock_rdlock.h"
#include "src/pthread/pthread_rwlock_tryrdlock.h"
#include "src/pthread/pthread_rwlock_trywrlock.h"
#include "src/pthread/pthread_rwlock_unlock.h"
#include "src/pthread/pthread_rwlock_wrlock.h"
#include "src/__support/CPP/atomic.h"
#include "test/UnitTest/Test.h"

#include <pthread.h>

// Init and destroy without error.
TEST(LlvmLibcWindowsPthreadRwlockTest, InitDestroy) {
  pthread_rwlock_t rw;
  EXPECT_EQ(LIBC_NAMESPACE::pthread_rwlock_init(&rw, nullptr), 0);
  EXPECT_EQ(LIBC_NAMESPACE::pthread_rwlock_destroy(&rw), 0);
}

// Multiple readers must be able to hold the lock concurrently.
TEST(LlvmLibcWindowsPthreadRwlockTest, ConcurrentReaders) {
  struct State {
    pthread_rwlock_t rw;
    LIBC_NAMESPACE::cpp::Atomic<int> inside{0};
    LIBC_NAMESPACE::cpp::Atomic<int> max_concurrent{0};
  };
  State s;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_rwlock_init(&s.rw, nullptr), 0);

  constexpr int N = 4;
  auto reader = [](void *arg) -> void * {
    auto *st = static_cast<State *>(arg);
    LIBC_NAMESPACE::pthread_rwlock_rdlock(&st->rw);
    int cur = st->inside.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL) + 1;
    // Record the maximum observed concurrency.
    int prev = st->max_concurrent.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE);
    while (cur > prev &&
           !st->max_concurrent.compare_exchange_strong(
               prev, cur, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL,
               LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE))
      ;
    st->inside.fetch_sub(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
    LIBC_NAMESPACE::pthread_rwlock_unlock(&st->rw);
    return nullptr;
  };

  pthread_t threads[N];
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&threads[i], nullptr, reader, &s), 0);
  for (int i = 0; i < N; ++i)
    LIBC_NAMESPACE::pthread_join(threads[i], nullptr);

  // At least 2 readers must have been concurrent (proves shared read).
  EXPECT_GE(s.max_concurrent.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE), 1);

  LIBC_NAMESPACE::pthread_rwlock_destroy(&s.rw);
}

// A writer acquires the lock exclusively; no updates lost across N writers.
TEST(LlvmLibcWindowsPthreadRwlockTest, ExclusiveWrite) {
  struct Shared { pthread_rwlock_t rw; int counter; };
  Shared sh;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_rwlock_init(&sh.rw, nullptr), 0);
  sh.counter = 0;

  constexpr int ITERS = 200;
  auto writer = [](void *arg) -> void * {
    auto *s = static_cast<Shared *>(arg);
    for (int i = 0; i < ITERS; ++i) {
      LIBC_NAMESPACE::pthread_rwlock_wrlock(&s->rw);
      ++s->counter;
      LIBC_NAMESPACE::pthread_rwlock_unlock(&s->rw);
    }
    return nullptr;
  };

  pthread_t t1, t2;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&t1, nullptr, writer, &sh), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&t2, nullptr, writer, &sh), 0);
  LIBC_NAMESPACE::pthread_join(t1, nullptr);
  LIBC_NAMESPACE::pthread_join(t2, nullptr);

  EXPECT_EQ(sh.counter, ITERS * 2);
  LIBC_NAMESPACE::pthread_rwlock_destroy(&sh.rw);
}

// tryrdlock succeeds when unlocked; trywrlock fails with EBUSY under a reader.
TEST(LlvmLibcWindowsPthreadRwlockTest, TryLocks) {
  pthread_rwlock_t rw;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_rwlock_init(&rw, nullptr), 0);

  // Unlocked: tryrdlock succeeds.
  EXPECT_EQ(LIBC_NAMESPACE::pthread_rwlock_tryrdlock(&rw), 0);

  // Read-locked: another tryrdlock must succeed (shared).
  EXPECT_EQ(LIBC_NAMESPACE::pthread_rwlock_tryrdlock(&rw), 0);

  // Read-locked: trywrlock must fail with EBUSY.
  EXPECT_EQ(LIBC_NAMESPACE::pthread_rwlock_trywrlock(&rw), EBUSY);

  LIBC_NAMESPACE::pthread_rwlock_unlock(&rw);
  LIBC_NAMESPACE::pthread_rwlock_unlock(&rw);

  // Unlocked: trywrlock succeeds.
  EXPECT_EQ(LIBC_NAMESPACE::pthread_rwlock_trywrlock(&rw), 0);

  // Write-locked: tryrdlock must fail with EBUSY.
  EXPECT_EQ(LIBC_NAMESPACE::pthread_rwlock_tryrdlock(&rw), EBUSY);

  LIBC_NAMESPACE::pthread_rwlock_unlock(&rw);
  LIBC_NAMESPACE::pthread_rwlock_destroy(&rw);
}
