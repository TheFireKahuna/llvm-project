//===-- Unittests for Windows Mutex and RawMutex ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/mutex.h"
#include "src/__support/threads/mutex_common.h"
#include "src/__support/threads/raw_mutex.h"
#include "src/__support/threads/sleep.h"
#include "src/__support/threads/thread.h"
#include "src/__support/time/clock_gettime.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"

// Smoke test mirrors the Darwin mutex_test.cpp exactly.
TEST(LlvmLibcWindowsMutexTest, SmokeTest) {
  LIBC_NAMESPACE::Mutex mutex(/*timed=*/false, /*recursive=*/false,
                              /*robust=*/false, /*pshared=*/false);
  ASSERT_EQ(mutex.lock(), LIBC_NAMESPACE::MutexError::NONE);
  ASSERT_EQ(mutex.unlock(), LIBC_NAMESPACE::MutexError::NONE);
  ASSERT_EQ(mutex.try_lock(), LIBC_NAMESPACE::MutexError::NONE);
  ASSERT_EQ(mutex.try_lock(), LIBC_NAMESPACE::MutexError::BUSY);
  ASSERT_EQ(mutex.unlock(), LIBC_NAMESPACE::MutexError::NONE);
  ASSERT_EQ(mutex.unlock(), LIBC_NAMESPACE::MutexError::UNLOCK_WITHOUT_LOCK);
}

TEST(LlvmLibcWindowsMutexTest, RecursiveLock) {
  LIBC_NAMESPACE::Mutex mutex(/*timed=*/false, /*recursive=*/true,
                              /*robust=*/false, /*pshared=*/false);
  ASSERT_EQ(mutex.lock(), LIBC_NAMESPACE::MutexError::NONE);
  ASSERT_EQ(mutex.lock(), LIBC_NAMESPACE::MutexError::NONE);
  ASSERT_EQ(mutex.lock(), LIBC_NAMESPACE::MutexError::NONE);
  ASSERT_EQ(mutex.unlock(), LIBC_NAMESPACE::MutexError::NONE);
  ASSERT_EQ(mutex.unlock(), LIBC_NAMESPACE::MutexError::NONE);
  ASSERT_EQ(mutex.unlock(), LIBC_NAMESPACE::MutexError::NONE);
}

TEST(LlvmLibcWindowsMutexTest, ErrorcheckSelfDeadlock) {
  LIBC_NAMESPACE::Mutex mutex(/*timed=*/false, /*recursive=*/false,
                              /*robust=*/false, /*pshared=*/false,
                              LIBC_NAMESPACE::PTHREAD_MUTEX_ERRORCHECK);
  ASSERT_EQ(mutex.lock(), LIBC_NAMESPACE::MutexError::NONE);
  // ERRORCHECK: second lock from same thread must return DEADLOCK, not hang.
  ASSERT_EQ(mutex.lock(), LIBC_NAMESPACE::MutexError::DEADLOCK);
  ASSERT_EQ(mutex.unlock(), LIBC_NAMESPACE::MutexError::NONE);
}

// RawMutex smoke and timeout — mirrors raw_mutex_test.cpp.
TEST(LlvmLibcWindowsRawMutexTest, SmokeTest) {
  LIBC_NAMESPACE::RawMutex mutex;
  ASSERT_TRUE(mutex.lock());
  ASSERT_TRUE(mutex.unlock());
  ASSERT_TRUE(mutex.try_lock());
  ASSERT_FALSE(mutex.try_lock());
  ASSERT_TRUE(mutex.unlock());
  ASSERT_FALSE(mutex.unlock());
}

TEST(LlvmLibcWindowsRawMutexTest, Timeout) {
  LIBC_NAMESPACE::RawMutex mutex;
  ASSERT_TRUE(mutex.lock());

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
  // Should timeout — same thread already holds the lock.
  ASSERT_FALSE(mutex.lock(*timeout));

  ASSERT_TRUE(mutex.unlock());
  // Expired timeout should not prevent acquiring an unlocked mutex.
  ASSERT_TRUE(mutex.lock(*timeout));
  ASSERT_TRUE(mutex.unlock());
}

// Cross-thread contention: two threads increment a counter under a mutex,
// verify no updates are lost.
static LIBC_NAMESPACE::Mutex *contention_mtx;
static LIBC_NAMESPACE::cpp::Atomic<int> contention_counter{0};
static constexpr int CONTENTION_ITERS = 1000;

static void *contention_func(void *) {
  for (int i = 0; i < CONTENTION_ITERS; ++i) {
    contention_mtx->lock();
    int v = contention_counter.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);
    contention_counter.store(v + 1, LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);
    contention_mtx->unlock();
  }
  return nullptr;
}

TEST(LlvmLibcWindowsMutexTest, CrossThreadContention) {
  LIBC_NAMESPACE::Mutex mutex(false, false, false, false);
  contention_mtx = &mutex;
  contention_counter.store(0, LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);

  LIBC_NAMESPACE::Thread t1, t2;
  ASSERT_EQ(t1.run(contention_func, nullptr), 0);
  ASSERT_EQ(t2.run(contention_func, nullptr), 0);

  void *r1, *r2;
  ASSERT_EQ(t1.join(&r1), 0);
  ASSERT_EQ(t2.join(&r2), 0);

  EXPECT_EQ(contention_counter.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED),
            2 * CONTENTION_ITERS);
}
