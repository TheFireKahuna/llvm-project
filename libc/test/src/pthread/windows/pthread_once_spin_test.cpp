//===-- Windows unittests for pthread_once and pthread_spin_* -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// pthread_once:
//   - The init function is called exactly once even from multiple threads
//
// pthread_spinlock:
//   - init / lock / unlock / trylock / destroy smoke test
//   - trylock returns EBUSY when already locked
//
//===----------------------------------------------------------------------===//

#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_join.h"
#include "src/pthread/pthread_once.h"
#include "src/pthread/pthread_spin_destroy.h"
#include "src/pthread/pthread_spin_init.h"
#include "src/pthread/pthread_spin_lock.h"
#include "src/pthread/pthread_spin_trylock.h"
#include "src/pthread/pthread_spin_unlock.h"
#include "src/__support/CPP/atomic.h"
#include "test/UnitTest/Test.h"

#include <pthread.h>

// ── pthread_once ──────────────────────────────────────────────────────────────

// The init function must run exactly once regardless of how many threads call
// pthread_once concurrently.
TEST(LlvmLibcWindowsPthreadOnceTest, CalledExactlyOnce) {
  static LIBC_NAMESPACE::cpp::Atomic<int> call_count{0};
  static pthread_once_t flag = PTHREAD_ONCE_INIT;

  struct Args {
    pthread_once_t *flag;
  };
  Args args = {&flag};

  auto worker = [](void *arg) -> void * {
    auto *a = static_cast<Args *>(arg);
    LIBC_NAMESPACE::pthread_once(
        a->flag,
        []() { call_count.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED); });
    return nullptr;
  };

  constexpr int N = 8;
  pthread_t threads[N];
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&threads[i], nullptr, worker, &args), 0);
  for (int i = 0; i < N; ++i)
    LIBC_NAMESPACE::pthread_join(threads[i], nullptr);

  EXPECT_EQ(call_count.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), 1);
}

// ── pthread_spinlock ──────────────────────────────────────────────────────────

TEST(LlvmLibcWindowsPthreadSpinTest, SmokeTest) {
  pthread_spinlock_t lock;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_spin_lock(&lock), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_spin_unlock(&lock), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_spin_destroy(&lock), 0);
}

// trylock succeeds when unlocked; EBUSY when already locked.
TEST(LlvmLibcWindowsPthreadSpinTest, Trylock) {
  pthread_spinlock_t lock;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE), 0);

  EXPECT_EQ(LIBC_NAMESPACE::pthread_spin_trylock(&lock), 0);
  EXPECT_EQ(LIBC_NAMESPACE::pthread_spin_trylock(&lock), EBUSY);
  LIBC_NAMESPACE::pthread_spin_unlock(&lock);

  LIBC_NAMESPACE::pthread_spin_destroy(&lock);
}
