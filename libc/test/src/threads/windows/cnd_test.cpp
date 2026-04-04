//===-- Unittests for Windows C11 cnd_* functions -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tests C11 <threads.h> condition variable functions:
//   cnd_init, cnd_destroy, cnd_signal, cnd_broadcast, cnd_wait, cnd_timedwait
//
//===----------------------------------------------------------------------===//

#include "src/threads/cnd_broadcast.h"
#include "src/threads/cnd_destroy.h"
#include "src/threads/cnd_init.h"
#include "src/threads/cnd_signal.h"
#include "src/threads/cnd_timedwait.h"
#include "src/threads/cnd_wait.h"
#include "src/threads/mtx_destroy.h"
#include "src/threads/mtx_init.h"
#include "src/threads/mtx_lock.h"
#include "src/threads/mtx_unlock.h"
#include "src/threads/thrd_create.h"
#include "src/threads/thrd_join.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/threads/sleep.h"
#include "src/__support/time/clock_gettime.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"

#include <threads.h>

// cnd_init returns thrd_success; cnd_destroy does not crash.
TEST(LlvmLibcWindowsCndTest, InitDestroy) {
  cnd_t cond;
  ASSERT_EQ(LIBC_NAMESPACE::cnd_init(&cond), static_cast<int>(thrd_success));
  LIBC_NAMESPACE::cnd_destroy(&cond);
}

// cnd_signal and cnd_broadcast on an empty queue are no-ops.
TEST(LlvmLibcWindowsCndTest, SignalBroadcastEmpty) {
  cnd_t cond;
  ASSERT_EQ(LIBC_NAMESPACE::cnd_init(&cond), static_cast<int>(thrd_success));
  LIBC_NAMESPACE::cnd_signal(&cond);
  LIBC_NAMESPACE::cnd_broadcast(&cond);
  LIBC_NAMESPACE::cnd_destroy(&cond);
}

// cnd_timedwait with an already-expired timeout returns thrd_timedout.
TEST(LlvmLibcWindowsCndTest, TimedwaitExpiredTimeout) {
  cnd_t cond;
  mtx_t mtx;
  ASSERT_EQ(LIBC_NAMESPACE::cnd_init(&cond), static_cast<int>(thrd_success));
  ASSERT_EQ(LIBC_NAMESPACE::mtx_init(&mtx, mtx_plain),
            static_cast<int>(thrd_success));
  ASSERT_EQ(LIBC_NAMESPACE::mtx_lock(&mtx), static_cast<int>(thrd_success));

  // Deadline one second in the past.
  struct timespec ts;
  LIBC_NAMESPACE::internal::clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec -= 1;

  int ret = LIBC_NAMESPACE::cnd_timedwait(&cond, &mtx, &ts);
  LIBC_NAMESPACE::mtx_unlock(&mtx);

  EXPECT_EQ(ret, static_cast<int>(thrd_timedout));

  LIBC_NAMESPACE::mtx_destroy(&mtx);
  LIBC_NAMESPACE::cnd_destroy(&cond);
}

// cnd_signal wakes one thread; cnd_broadcast wakes all.
struct CndSharedState {
  cnd_t cond;
  mtx_t mtx;
  LIBC_NAMESPACE::cpp::Atomic<int> woken{0};
  LIBC_NAMESPACE::cpp::Atomic<int> ready{0};
  bool predicate = false;
};

static int cnd_waiter(void *arg) {
  auto *s = static_cast<CndSharedState *>(arg);
  LIBC_NAMESPACE::mtx_lock(&s->mtx);
  s->ready.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
  while (!s->predicate)
    LIBC_NAMESPACE::cnd_wait(&s->cond, &s->mtx);
  s->woken.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
  LIBC_NAMESPACE::mtx_unlock(&s->mtx);
  return 0;
}

TEST(LlvmLibcWindowsCndTest, SignalWakesOne) {
  CndSharedState s;
  ASSERT_EQ(LIBC_NAMESPACE::cnd_init(&s.cond), static_cast<int>(thrd_success));
  ASSERT_EQ(LIBC_NAMESPACE::mtx_init(&s.mtx, mtx_plain),
            static_cast<int>(thrd_success));

  thrd_t t1, t2;
  ASSERT_EQ(LIBC_NAMESPACE::thrd_create(&t1, cnd_waiter, &s),
            static_cast<int>(thrd_success));
  ASSERT_EQ(LIBC_NAMESPACE::thrd_create(&t2, cnd_waiter, &s),
            static_cast<int>(thrd_success));

  while (s.ready.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) < 2)
    LIBC_NAMESPACE::sleep_briefly();

  // Wake one, let it exit, then wake the other.
  LIBC_NAMESPACE::mtx_lock(&s.mtx);
  s.predicate = true;
  LIBC_NAMESPACE::cnd_signal(&s.cond);
  LIBC_NAMESPACE::mtx_unlock(&s.mtx);

  while (s.woken.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) < 1)
    LIBC_NAMESPACE::sleep_briefly();

  LIBC_NAMESPACE::mtx_lock(&s.mtx);
  LIBC_NAMESPACE::cnd_signal(&s.cond);
  LIBC_NAMESPACE::mtx_unlock(&s.mtx);

  ASSERT_EQ(LIBC_NAMESPACE::thrd_join(t1, nullptr),
            static_cast<int>(thrd_success));
  ASSERT_EQ(LIBC_NAMESPACE::thrd_join(t2, nullptr),
            static_cast<int>(thrd_success));
  EXPECT_EQ(s.woken.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), 2);

  LIBC_NAMESPACE::mtx_destroy(&s.mtx);
  LIBC_NAMESPACE::cnd_destroy(&s.cond);
}

TEST(LlvmLibcWindowsCndTest, BroadcastWakesAll) {
  constexpr int N = 4;
  CndSharedState s;
  ASSERT_EQ(LIBC_NAMESPACE::cnd_init(&s.cond), static_cast<int>(thrd_success));
  ASSERT_EQ(LIBC_NAMESPACE::mtx_init(&s.mtx, mtx_plain),
            static_cast<int>(thrd_success));

  thrd_t threads[N];
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(LIBC_NAMESPACE::thrd_create(&threads[i], cnd_waiter, &s),
              static_cast<int>(thrd_success));

  while (s.ready.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) < N)
    LIBC_NAMESPACE::sleep_briefly();

  LIBC_NAMESPACE::mtx_lock(&s.mtx);
  s.predicate = true;
  LIBC_NAMESPACE::cnd_broadcast(&s.cond);
  LIBC_NAMESPACE::mtx_unlock(&s.mtx);

  for (int i = 0; i < N; ++i)
    ASSERT_EQ(LIBC_NAMESPACE::thrd_join(threads[i], nullptr),
              static_cast<int>(thrd_success));

  EXPECT_EQ(s.woken.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), N);

  LIBC_NAMESPACE::mtx_destroy(&s.mtx);
  LIBC_NAMESPACE::cnd_destroy(&s.cond);
}
