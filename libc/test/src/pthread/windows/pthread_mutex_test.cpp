//===-- Windows unittests for pthread_mutex_* -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - pthread_mutex_init / pthread_mutex_destroy smoke test
//   - pthread_mutex_lock / pthread_mutex_unlock
//   - pthread_mutex_trylock succeeds when unlocked, fails with EBUSY when locked
//   - PTHREAD_MUTEX_ERRORCHECK detects self-deadlock (EDEADLK)
//   - Cross-thread contention: two threads increment a counter; no updates lost
//
//===----------------------------------------------------------------------===//

#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_join.h"
#include "src/pthread/pthread_mutex_destroy.h"
#include "src/pthread/pthread_mutex_init.h"
#include "src/pthread/pthread_mutex_lock.h"
#include "src/pthread/pthread_mutex_trylock.h"
#include "src/pthread/pthread_mutex_unlock.h"
#include "src/pthread/pthread_mutexattr_destroy.h"
#include "src/pthread/pthread_mutexattr_init.h"
#include "src/pthread/pthread_mutexattr_settype.h"
#include "test/UnitTest/Test.h"

#include <pthread.h>

// Basic init/lock/unlock/destroy.
TEST(LlvmLibcWindowsPthreadMutexTest, SmokeTest) {
  pthread_mutex_t mtx;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutex_init(&mtx, nullptr), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutex_lock(&mtx), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutex_unlock(&mtx), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutex_destroy(&mtx), 0);
}

// trylock succeeds when unlocked; EBUSY when already locked.
TEST(LlvmLibcWindowsPthreadMutexTest, Trylock) {
  pthread_mutex_t mtx;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutex_init(&mtx, nullptr), 0);

  EXPECT_EQ(LIBC_NAMESPACE::pthread_mutex_trylock(&mtx), 0);
  EXPECT_EQ(LIBC_NAMESPACE::pthread_mutex_trylock(&mtx), EBUSY);
  LIBC_NAMESPACE::pthread_mutex_unlock(&mtx);

  LIBC_NAMESPACE::pthread_mutex_destroy(&mtx);
}

// PTHREAD_MUTEX_ERRORCHECK: re-locking must return EDEADLK, not hang.
TEST(LlvmLibcWindowsPthreadMutexTest, ErrorcheckDeadlock) {
  pthread_mutexattr_t attr;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutexattr_init(&attr), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutexattr_settype(
                &attr, PTHREAD_MUTEX_ERRORCHECK), 0);

  pthread_mutex_t mtx;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutex_init(&mtx, &attr), 0);

  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutex_lock(&mtx), 0);
  EXPECT_EQ(LIBC_NAMESPACE::pthread_mutex_lock(&mtx), EDEADLK);
  LIBC_NAMESPACE::pthread_mutex_unlock(&mtx);

  LIBC_NAMESPACE::pthread_mutex_destroy(&mtx);
  LIBC_NAMESPACE::pthread_mutexattr_destroy(&attr);
}

// Two threads increment a shared int under mutex — no updates must be lost.
TEST(LlvmLibcWindowsPthreadMutexTest, CrossThreadContention) {
  struct Shared { pthread_mutex_t mtx; int counter; };
  Shared shared;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutex_init(&shared.mtx, nullptr), 0);
  shared.counter = 0;

  constexpr int INCREMENTS = 500;
  auto worker = [](void *arg) -> void * {
    auto *s = static_cast<Shared *>(arg);
    for (int i = 0; i < INCREMENTS; ++i) {
      LIBC_NAMESPACE::pthread_mutex_lock(&s->mtx);
      ++s->counter;
      LIBC_NAMESPACE::pthread_mutex_unlock(&s->mtx);
    }
    return nullptr;
  };

  pthread_t t1, t2;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&t1, nullptr, worker, &shared), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&t2, nullptr, worker, &shared), 0);
  LIBC_NAMESPACE::pthread_join(t1, nullptr);
  LIBC_NAMESPACE::pthread_join(t2, nullptr);

  EXPECT_EQ(shared.counter, INCREMENTS * 2);
  LIBC_NAMESPACE::pthread_mutex_destroy(&shared.mtx);
}
