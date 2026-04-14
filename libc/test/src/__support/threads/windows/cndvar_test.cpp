//===-- Unittests for Windows CndVar ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/CndVar.h"
#include "src/__support/threads/mutex.h"
#include "src/__support/threads/sleep.h"
#include "src/__support/threads/thread.h"
#include "src/__support/time/clock_gettime.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"

// Init and destroy don't crash.
TEST(LlvmLibcWindowsCndVarTest, InitDestroy) {
  LIBC_NAMESPACE::CndVar cv;
  ASSERT_EQ(LIBC_NAMESPACE::CndVar::init(&cv), 0);
  LIBC_NAMESPACE::CndVar::destroy(&cv);
}

// notify_one on empty queue is a no-op.
TEST(LlvmLibcWindowsCndVarTest, NotifyOneEmpty) {
  LIBC_NAMESPACE::CndVar cv;
  LIBC_NAMESPACE::CndVar::init(&cv);
  cv.notify_one();
  cv.notify_one();
  LIBC_NAMESPACE::CndVar::destroy(&cv);
}

// broadcast on empty queue is a no-op.
TEST(LlvmLibcWindowsCndVarTest, BroadcastEmpty) {
  LIBC_NAMESPACE::CndVar cv;
  LIBC_NAMESPACE::CndVar::init(&cv);
  cv.broadcast();
  LIBC_NAMESPACE::CndVar::destroy(&cv);
}

// notify_one wakes exactly one waiting thread.
struct NotifyOneState {
  LIBC_NAMESPACE::CndVar cv;
  LIBC_NAMESPACE::Mutex mtx{false, false, false, false};
  LIBC_NAMESPACE::cpp::Atomic<int> woken{0};
  LIBC_NAMESPACE::cpp::Atomic<int> ready{0};
  bool predicate = false;
};

static void *notify_one_waiter(void *arg) {
  auto *s = static_cast<NotifyOneState *>(arg);
  s->mtx.lock();
  s->ready.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
  while (!s->predicate)
    s->cv.wait(&s->mtx);
  s->woken.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
  s->mtx.unlock();
  return nullptr;
}

TEST(LlvmLibcWindowsCndVarTest, NotifyOneWakesOne) {
  NotifyOneState s;
  LIBC_NAMESPACE::CndVar::init(&s.cv);

  LIBC_NAMESPACE::Thread t1, t2;
  ASSERT_EQ(t1.run(notify_one_waiter, &s), 0);
  ASSERT_EQ(t2.run(notify_one_waiter, &s), 0);

  // Wait until both threads are in cv.wait().
  while (s.ready.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) < 2)
    LIBC_NAMESPACE::sleep_briefly();

  // Wake one.
  s.mtx.lock();
  s.predicate = true;
  s.cv.notify_one();
  s.mtx.unlock();

  // Let the woken thread run. Then wake the second.
  while (s.woken.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) < 1)
    LIBC_NAMESPACE::sleep_briefly();

  s.mtx.lock();
  s.cv.notify_one();
  s.mtx.unlock();

  ASSERT_EQ(t1.join(static_cast<void **>(nullptr)), 0);
  ASSERT_EQ(t2.join(static_cast<void **>(nullptr)), 0);
  EXPECT_EQ(s.woken.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), 2);

  LIBC_NAMESPACE::CndVar::destroy(&s.cv);
}

// broadcast wakes all waiting threads simultaneously.
struct BroadcastState {
  LIBC_NAMESPACE::CndVar cv;
  LIBC_NAMESPACE::Mutex mtx{false, false, false, false};
  LIBC_NAMESPACE::cpp::Atomic<int> woken{0};
  LIBC_NAMESPACE::cpp::Atomic<int> ready{0};
  bool predicate = false;
};

static void *broadcast_waiter(void *arg) {
  auto *s = static_cast<BroadcastState *>(arg);
  s->mtx.lock();
  s->ready.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
  while (!s->predicate)
    s->cv.wait(&s->mtx);
  s->woken.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
  s->mtx.unlock();
  return nullptr;
}

TEST(LlvmLibcWindowsCndVarTest, BroadcastWakesAll) {
  constexpr int N = 4;
  BroadcastState s;
  LIBC_NAMESPACE::CndVar::init(&s.cv);

  LIBC_NAMESPACE::Thread threads[N];
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(threads[i].run(broadcast_waiter, &s), 0);

  while (s.ready.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) < N)
    LIBC_NAMESPACE::sleep_briefly();

  s.mtx.lock();
  s.predicate = true;
  s.cv.broadcast();
  s.mtx.unlock();

  for (int i = 0; i < N; ++i)
    ASSERT_EQ(threads[i].join(static_cast<void **>(nullptr)), 0);

  EXPECT_EQ(s.woken.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), N);
  LIBC_NAMESPACE::CndVar::destroy(&s.cv);
}

// wait() with a timeout returns ETIMEDOUT.
TEST(LlvmLibcWindowsCndVarTest, WaitTimeout) {
  LIBC_NAMESPACE::CndVar cv;
  LIBC_NAMESPACE::CndVar::init(&cv);
  LIBC_NAMESPACE::Mutex mtx(false, false, false, false);

  timespec ts;
  LIBC_NAMESPACE::internal::clock_gettime(CLOCK_MONOTONIC, &ts);
  ts.tv_nsec += 5000000; // 5ms
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000;
  }
  auto timeout =
      LIBC_NAMESPACE::internal::AbsTimeout::from_timespec(ts, false);
  ASSERT_TRUE(timeout.has_value());

  mtx.lock();
  int ret = cv.wait(&mtx, *timeout);
  mtx.unlock();

  EXPECT_EQ(ret, ETIMEDOUT);
  LIBC_NAMESPACE::CndVar::destroy(&cv);
}
