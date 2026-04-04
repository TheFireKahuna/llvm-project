//===-- Windows unittests for pthread_cond_* ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - pthread_cond_init / pthread_cond_destroy smoke test
//   - pthread_cond_signal wakes exactly one waiter
//   - pthread_cond_broadcast wakes all waiters
//   - pthread_cond_timedwait returns ETIMEDOUT on expiry
//
//===----------------------------------------------------------------------===//

#include "src/pthread/pthread_cond_broadcast.h"
#include "src/pthread/pthread_cond_destroy.h"
#include "src/pthread/pthread_cond_init.h"
#include "src/pthread/pthread_cond_signal.h"
#include "src/pthread/pthread_cond_timedwait.h"
#include "src/pthread/pthread_cond_wait.h"
#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_join.h"
#include "src/pthread/pthread_mutex_destroy.h"
#include "src/pthread/pthread_mutex_init.h"
#include "src/pthread/pthread_mutex_lock.h"
#include "src/pthread/pthread_mutex_unlock.h"
#include "src/time/clock_gettime.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/threads/sleep.h"
#include "test/UnitTest/Test.h"

#include <pthread.h>
#include <time.h>

// Helper: build an absolute timeout N seconds in the past (immediately expired)
static struct timespec past_timeout() {
  struct timespec ts;
  LIBC_NAMESPACE::clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec -= 1;
  return ts;
}

// init and destroy without error.
TEST(LlvmLibcWindowsPthreadCondTest, InitDestroy) {
  pthread_cond_t cond;
  EXPECT_EQ(LIBC_NAMESPACE::pthread_cond_init(&cond, nullptr), 0);
  EXPECT_EQ(LIBC_NAMESPACE::pthread_cond_destroy(&cond), 0);
}

// pthread_cond_signal wakes exactly one waiter at a time. Verified by
// signalling once, waiting for exactly one thread to make progress, and
// only then signalling again — a broadcast-style implementation would
// wake both and fail the intermediate "woken == 1" check.
TEST(LlvmLibcWindowsPthreadCondTest, SignalWakesOne) {
  struct State {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    LIBC_NAMESPACE::cpp::Atomic<int> ready{0};
    LIBC_NAMESPACE::cpp::Atomic<int> woken{0};
    bool predicate = false;
  };
  State s;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutex_init(&s.mtx, nullptr), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_cond_init(&s.cond, nullptr), 0);

  auto waiter = [](void *arg) -> void * {
    auto *st = static_cast<State *>(arg);
    LIBC_NAMESPACE::pthread_mutex_lock(&st->mtx);
    st->ready.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
    while (!st->predicate)
      LIBC_NAMESPACE::pthread_cond_wait(&st->cond, &st->mtx);
    st->woken.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
    LIBC_NAMESPACE::pthread_mutex_unlock(&st->mtx);
    return nullptr;
  };

  pthread_t t1, t2;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&t1, nullptr, waiter, &s), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&t2, nullptr, waiter, &s), 0);

  while (s.ready.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) < 2)
    LIBC_NAMESPACE::sleep_briefly();

  // Set the predicate once, signal once — exactly one waiter wakes and
  // exits the loop. The second stays parked because pthread_cond_signal
  // may wake at most one thread (POSIX §2.9.4).
  LIBC_NAMESPACE::pthread_mutex_lock(&s.mtx);
  s.predicate = true;
  LIBC_NAMESPACE::pthread_cond_signal(&s.cond);
  LIBC_NAMESPACE::pthread_mutex_unlock(&s.mtx);

  while (s.woken.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) < 1)
    LIBC_NAMESPACE::sleep_briefly();
  EXPECT_EQ(s.woken.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE), 1);

  LIBC_NAMESPACE::pthread_mutex_lock(&s.mtx);
  LIBC_NAMESPACE::pthread_cond_signal(&s.cond);
  LIBC_NAMESPACE::pthread_mutex_unlock(&s.mtx);

  LIBC_NAMESPACE::pthread_join(t1, nullptr);
  LIBC_NAMESPACE::pthread_join(t2, nullptr);

  EXPECT_EQ(s.woken.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), 2);

  LIBC_NAMESPACE::pthread_cond_destroy(&s.cond);
  LIBC_NAMESPACE::pthread_mutex_destroy(&s.mtx);
}

// pthread_cond_broadcast wakes all waiters at once.
TEST(LlvmLibcWindowsPthreadCondTest, BroadcastWakesAll) {
  struct State {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    int             woken = 0;
    bool            go    = false;
  };
  State s;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutex_init(&s.mtx, nullptr), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_cond_init(&s.cond, nullptr), 0);

  auto waiter = [](void *arg) -> void * {
    auto *st = static_cast<State *>(arg);
    LIBC_NAMESPACE::pthread_mutex_lock(&st->mtx);
    while (!st->go)
      LIBC_NAMESPACE::pthread_cond_wait(&st->cond, &st->mtx);
    ++st->woken;
    LIBC_NAMESPACE::pthread_mutex_unlock(&st->mtx);
    return nullptr;
  };

  constexpr int N = 4;
  pthread_t threads[N];
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&threads[i], nullptr, waiter, &s), 0);

  // Set go and broadcast.
  LIBC_NAMESPACE::pthread_mutex_lock(&s.mtx);
  s.go = true;
  LIBC_NAMESPACE::pthread_cond_broadcast(&s.cond);
  LIBC_NAMESPACE::pthread_mutex_unlock(&s.mtx);

  for (int i = 0; i < N; ++i)
    LIBC_NAMESPACE::pthread_join(threads[i], nullptr);

  EXPECT_EQ(s.woken, N);

  LIBC_NAMESPACE::pthread_cond_destroy(&s.cond);
  LIBC_NAMESPACE::pthread_mutex_destroy(&s.mtx);
}

// pthread_cond_timedwait returns ETIMEDOUT when the deadline has passed.
TEST(LlvmLibcWindowsPthreadCondTest, TimedwaitTimeout) {
  pthread_mutex_t mtx;
  pthread_cond_t  cond;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_mutex_init(&mtx, nullptr), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_cond_init(&cond, nullptr), 0);

  // Deadline 1 second in the past.
  struct timespec ts;
  LIBC_NAMESPACE::clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec -= 1;

  LIBC_NAMESPACE::pthread_mutex_lock(&mtx);
  int ret = LIBC_NAMESPACE::pthread_cond_timedwait(&cond, &mtx, &ts);
  LIBC_NAMESPACE::pthread_mutex_unlock(&mtx);

  EXPECT_EQ(ret, ETIMEDOUT);

  LIBC_NAMESPACE::pthread_cond_destroy(&cond);
  LIBC_NAMESPACE::pthread_mutex_destroy(&mtx);
}
