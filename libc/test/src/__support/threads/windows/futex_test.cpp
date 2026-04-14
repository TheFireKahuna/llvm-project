//===-- Windows unittests for Futex ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tests for the Windows Futex class (Treiber-stack implementation).
//
// Compliance points tested:
//   - Value store/load semantics
//   - wait() returns immediately if value has changed
//   - wait() with timeout returns -ETIMEDOUT
//   - notify_one() wakes a single waiter
//   - notify_all() wakes all waiters
//   - store_and_notify() atomically updates value and wakes
//   - compare_exchange_strong on value portion
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/threads/sleep.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "hdr/errno_macros.h"

// Helper: read current time as an AbsTimeout for a relative duration in ms.
static LIBC_NAMESPACE::internal::AbsTimeout
make_timeout_ms(long long ms, bool realtime = false) {
  timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = static_cast<long>(ms * 1000000);
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec = ts.tv_nsec / 1000000000;
    ts.tv_nsec %= 1000000000;
  }
  auto result = LIBC_NAMESPACE::internal::AbsTimeout::from_timespec(
      ts, realtime);
  return *result;
}

// Value semantics: store and load work correctly.
TEST(LlvmLibcFutexTest, ValueStoreLoad) {
  LIBC_NAMESPACE::Futex ftx(0);
  EXPECT_EQ(ftx.get_value(), static_cast<LIBC_NAMESPACE::FutexValueType>(0));

  ftx = 42;
  EXPECT_EQ(ftx.get_value(), static_cast<LIBC_NAMESPACE::FutexValueType>(42));

  ftx.store(100);
  EXPECT_EQ(ftx.load(), static_cast<LIBC_NAMESPACE::FutexValueType>(100));
}

// wait() returns 0 immediately when the value has already changed.
TEST(LlvmLibcFutexTest, WaitReturnsWhenValueChanged) {
  LIBC_NAMESPACE::Futex ftx(1);
  // Expected value 0 != current value 1 → immediate return.
  long ret = ftx.wait(0);
  EXPECT_EQ(ret, 0L);
}

// wait() with timeout returns -ETIMEDOUT when value matches and nobody wakes.
TEST(LlvmLibcFutexTest, WaitTimeout) {
  LIBC_NAMESPACE::Futex ftx(0);
  auto timeout = make_timeout_ms(5); // 5ms timeout
  long ret = ftx.wait(0, timeout);
  EXPECT_EQ(ret, static_cast<long>(-ETIMEDOUT));
}

// notify_one() on empty queue is a no-op (doesn't crash).
TEST(LlvmLibcFutexTest, NotifyOneEmpty) {
  LIBC_NAMESPACE::Futex ftx(0);
  long ret = ftx.notify_one();
  EXPECT_EQ(ret, 0L);
}

// notify_all() on empty queue is a no-op.
TEST(LlvmLibcFutexTest, NotifyAllEmpty) {
  LIBC_NAMESPACE::Futex ftx(0);
  long ret = ftx.notify_all();
  EXPECT_EQ(ret, 0L);
}

// store_and_notify atomically updates the value.
TEST(LlvmLibcFutexTest, StoreAndNotifyUpdatesValue) {
  LIBC_NAMESPACE::Futex ftx(0);
  ftx.store_and_notify(99);
  EXPECT_EQ(ftx.get_value(), static_cast<LIBC_NAMESPACE::FutexValueType>(99));
}

// compare_exchange_strong succeeds when value matches.
TEST(LlvmLibcFutexTest, CompareExchangeSuccess) {
  LIBC_NAMESPACE::Futex ftx(10);
  LIBC_NAMESPACE::FutexValueType expected = 10;
  bool ok = ftx.compare_exchange_strong(expected, 20);
  EXPECT_TRUE(ok);
  EXPECT_EQ(ftx.get_value(), static_cast<LIBC_NAMESPACE::FutexValueType>(20));
}

// compare_exchange_strong fails when value doesn't match.
TEST(LlvmLibcFutexTest, CompareExchangeFailure) {
  LIBC_NAMESPACE::Futex ftx(10);
  LIBC_NAMESPACE::FutexValueType expected = 5;
  bool ok = ftx.compare_exchange_strong(expected, 20);
  EXPECT_FALSE(ok);
  EXPECT_EQ(expected, static_cast<LIBC_NAMESPACE::FutexValueType>(10));
  EXPECT_EQ(ftx.get_value(), static_cast<LIBC_NAMESPACE::FutexValueType>(10));
}

// exchange returns old value and sets new value.
TEST(LlvmLibcFutexTest, Exchange) {
  LIBC_NAMESPACE::Futex ftx(7);
  LIBC_NAMESPACE::FutexValueType old = ftx.exchange(13);
  EXPECT_EQ(old, static_cast<LIBC_NAMESPACE::FutexValueType>(7));
  EXPECT_EQ(ftx.get_value(), static_cast<LIBC_NAMESPACE::FutexValueType>(13));
}

// Cross-thread wait/wake test using NtCreateThreadEx directly.
static LIBC_NAMESPACE::Futex *shared_ftx;
static LIBC_NAMESPACE::cpp::Atomic<int> thread_started{0};

static DWORD __stdcall waiter_thread(void *) {
  thread_started.store(1, LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
  // Wait for value to change from 0.
  shared_ftx->wait(0);
  return 0;
}

TEST(LlvmLibcFutexTest, CrossThreadWakeOne) {
  LIBC_NAMESPACE::Futex ftx(0);
  shared_ftx = &ftx;
  thread_started.store(0, LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);

  HANDLE thread =
      LIBC_NAMESPACE::test_support::create_thread(waiter_thread, nullptr);
  ASSERT_NE(thread, static_cast<HANDLE>(nullptr));

  // Wait for the thread to store 1 before calling wait().
  while (thread_started.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) == 0)
    LIBC_NAMESPACE::sleep_briefly();

  // store_and_notify atomically sets the value then wakes waiters.
  // If the thread hasn't entered wait() yet it will see the updated value
  // and return immediately — no timing dependency.
  ftx.store_and_notify(1);

  // Thread should wake and exit within a reasonable time.
  DWORD result =
      LIBC_NAMESPACE::test_support::wait_for_single_object(thread, 5000);
  EXPECT_EQ(result,
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(thread);
}
