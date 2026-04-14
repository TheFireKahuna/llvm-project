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
#include "src/__support/threads/mutex.h"
#include "src/__support/threads/sleep.h"
#include "src/__support/threads/thread.h"
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
  ASSERT_EQ(LIBC_NAMESPACE::cnd_init(&cond), static_cast<int>(thrd_success));

  // Use a Mutex (C++ layer) reinterpreted as mtx_t for the timeout test.
  LIBC_NAMESPACE::Mutex m(false, false, false, false);
  mtx_t *mtx = reinterpret_cast<mtx_t *>(&m);
  m.lock();

  // Set a timeout in the past.
  struct timespec ts;
  LIBC_NAMESPACE::internal::clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec -= 1; // 1 second in the past.

  int ret = LIBC_NAMESPACE::cnd_timedwait(&cond, mtx, &ts);
  m.unlock();

  EXPECT_EQ(ret, static_cast<int>(thrd_timedout));
  LIBC_NAMESPACE::cnd_destroy(&cond);
}

// cnd_signal wakes one thread; cnd_broadcast wakes all.
struct CndSharedState {
  cnd_t cond;
  LIBC_NAMESPACE::Mutex mtx{false, false, false, false};
  LIBC_NAMESPACE::cpp::Atomic<int> woken{0};
  LIBC_NAMESPACE::cpp::Atomic<int> ready{0};
  bool predicate = false;
};

static void *cnd_waiter(void *arg) {
  auto *s = static_cast<CndSharedState *>(arg);
  mtx_t *mtx = reinterpret_cast<mtx_t *>(&s->mtx);
  s->mtx.lock();
  s->ready.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
  while (!s->predicate)
    LIBC_NAMESPACE::cnd_wait(&s->cond, mtx);
  s->woken.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
  s->mtx.unlock();
  return nullptr;
}

TEST(LlvmLibcWindowsCndTest, SignalWakesOne) {
  CndSharedState s;
  ASSERT_EQ(LIBC_NAMESPACE::cnd_init(&s.cond), static_cast<int>(thrd_success));

  LIBC_NAMESPACE::Thread t1, t2;
  ASSERT_EQ(t1.run(cnd_waiter, &s), 0);
  ASSERT_EQ(t2.run(cnd_waiter, &s), 0);

  while (s.ready.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) < 2)
    LIBC_NAMESPACE::sleep_briefly();

  // Wake one, let it exit, then wake the other.
  s.mtx.lock();
  s.predicate = true;
  LIBC_NAMESPACE::cnd_signal(&s.cond);
  s.mtx.unlock();

  while (s.woken.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) < 1)
    LIBC_NAMESPACE::sleep_briefly();

  s.mtx.lock();
  LIBC_NAMESPACE::cnd_signal(&s.cond);
  s.mtx.unlock();

  ASSERT_EQ(t1.join(static_cast<void **>(nullptr)), 0);
  ASSERT_EQ(t2.join(static_cast<void **>(nullptr)), 0);
  EXPECT_EQ(s.woken.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), 2);
  LIBC_NAMESPACE::cnd_destroy(&s.cond);
}

TEST(LlvmLibcWindowsCndTest, BroadcastWakesAll) {
  constexpr int N = 4;
  CndSharedState s;
  ASSERT_EQ(LIBC_NAMESPACE::cnd_init(&s.cond), static_cast<int>(thrd_success));

  LIBC_NAMESPACE::Thread threads[N];
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(threads[i].run(cnd_waiter, &s), 0);

  while (s.ready.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) < N)
    LIBC_NAMESPACE::sleep_briefly();

  s.mtx.lock();
  s.predicate = true;
  LIBC_NAMESPACE::cnd_broadcast(&s.cond);
  s.mtx.unlock();

  for (int i = 0; i < N; ++i)
    ASSERT_EQ(threads[i].join(static_cast<void **>(nullptr)), 0);

  EXPECT_EQ(s.woken.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), N);
  LIBC_NAMESPACE::cnd_destroy(&s.cond);
}
