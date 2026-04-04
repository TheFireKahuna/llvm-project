//===-- Windows unittests for sigtimedwait ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - sigtimedwait() returns the signal number on success
//   - siginfo_t is filled with si_signo and si_value
//   - Returns -1 with errno=EAGAIN on timeout
//   - Returns -1 with errno=EINVAL for invalid timespec
//   - Consumes a pending signal from the blocked mask
//   - Zero timeout performs a non-blocking poll
//
//===----------------------------------------------------------------------===//

#include "hdr/signal_macros.h"
#include "src/signal/raise.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigaddset.h"
#include "src/signal/sigemptyset.h"
#include "src/signal/sigprocmask.h"
#include "src/signal/sigqueue.h"
#include "src/signal/sigtimedwait.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include <unistd.h> // getpid

class LlvmLibcWindowsSigtimedwaitTest
    : public LIBC_NAMESPACE::testing::ErrnoCheckingTest {
  sigset_t saved_mask;
  struct sigaction saved_usr1;

public:
  void SetUp() override {
    ErrnoCheckingTest::SetUp();
    LIBC_NAMESPACE::sigprocmask(0, nullptr, &saved_mask);
    LIBC_NAMESPACE::sigaction(SIGUSR1, nullptr, &saved_usr1);
  }

  void TearDown() override {
    LIBC_NAMESPACE::sigprocmask(SIG_SETMASK, &saved_mask, nullptr);
    LIBC_NAMESPACE::sigaction(SIGUSR1, &saved_usr1, nullptr);
    ErrnoCheckingTest::TearDown();
  }
};

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;

// POSIX: "If [...] a signal [...] is pending, sigtimedwait() shall return
// immediately with the signal number as the function return value."
TEST_F(LlvmLibcWindowsSigtimedwaitTest, ConsumePendingSignal) {
  // Block SIGUSR1 so raise() makes it pending.
  sigset_t block;
  LIBC_NAMESPACE::sigemptyset(&block);
  LIBC_NAMESPACE::sigaddset(&block, SIGUSR1);
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &block, nullptr);

  LIBC_NAMESPACE::raise(SIGUSR1);

  // sigtimedwait should consume the pending SIGUSR1.
  sigset_t wait_set;
  LIBC_NAMESPACE::sigemptyset(&wait_set);
  LIBC_NAMESPACE::sigaddset(&wait_set, SIGUSR1);

  siginfo_t info;
  timespec timeout = {0, 0}; // Non-blocking poll.
  int sig = LIBC_NAMESPACE::sigtimedwait(&wait_set, &info, &timeout);
  EXPECT_EQ(sig, SIGUSR1);
  EXPECT_EQ(info.si_signo, SIGUSR1);
}

// POSIX: "If [...] no signal [...] is pending, sigtimedwait() shall wait
// for the time interval specified [...] If the timespec [...] is zero-valued
// [...] shall return immediately."
TEST_F(LlvmLibcWindowsSigtimedwaitTest, TimeoutEagain) {
  sigset_t block;
  LIBC_NAMESPACE::sigemptyset(&block);
  LIBC_NAMESPACE::sigaddset(&block, SIGUSR1);
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &block, nullptr);

  // No pending signal — zero timeout should return EAGAIN immediately.
  sigset_t wait_set;
  LIBC_NAMESPACE::sigemptyset(&wait_set);
  LIBC_NAMESPACE::sigaddset(&wait_set, SIGUSR1);

  timespec timeout = {0, 0};
  EXPECT_THAT(LIBC_NAMESPACE::sigtimedwait(&wait_set, nullptr, &timeout),
              Fails(EAGAIN));
}

// POSIX: "tv_nsec is [...] less than zero or greater than or equal to
// 1000 million" → EINVAL.
TEST_F(LlvmLibcWindowsSigtimedwaitTest, InvalidTimespec) {
  sigset_t wait_set;
  LIBC_NAMESPACE::sigemptyset(&wait_set);
  LIBC_NAMESPACE::sigaddset(&wait_set, SIGUSR1);

  // Negative tv_nsec.
  timespec bad_timeout = {0, -1};
  EXPECT_THAT(
      LIBC_NAMESPACE::sigtimedwait(&wait_set, nullptr, &bad_timeout),
      Fails(EINVAL));

  // tv_nsec >= 1e9.
  timespec bad_timeout2 = {0, 1000000000};
  EXPECT_THAT(
      LIBC_NAMESPACE::sigtimedwait(&wait_set, nullptr, &bad_timeout2),
      Fails(EINVAL));
}

// POSIX: sigtimedwait fills siginfo_t with si_value from sigqueue.
TEST_F(LlvmLibcWindowsSigtimedwaitTest, SiValueFromSigqueue) {
  // Block SIGUSR1.
  sigset_t block;
  LIBC_NAMESPACE::sigemptyset(&block);
  LIBC_NAMESPACE::sigaddset(&block, SIGUSR1);
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &block, nullptr);

  // Queue with a specific value.
  union sigval val;
  val.sival_int = 1234;
  LIBC_NAMESPACE::sigqueue(getpid(), SIGUSR1, val);

  // Consume via sigtimedwait.
  sigset_t wait_set;
  LIBC_NAMESPACE::sigemptyset(&wait_set);
  LIBC_NAMESPACE::sigaddset(&wait_set, SIGUSR1);

  siginfo_t info;
  timespec timeout = {0, 0};
  int sig = LIBC_NAMESPACE::sigtimedwait(&wait_set, &info, &timeout);
  EXPECT_EQ(sig, SIGUSR1);
  EXPECT_EQ(info.si_signo, SIGUSR1);
  EXPECT_EQ(info.si_value.sival_int, 1234);
}

// POSIX: sigtimedwait with a short timeout actually waits.
TEST_F(LlvmLibcWindowsSigtimedwaitTest, ShortTimeoutActuallyWaits) {
  sigset_t block;
  LIBC_NAMESPACE::sigemptyset(&block);
  LIBC_NAMESPACE::sigaddset(&block, SIGUSR1);
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &block, nullptr);

  sigset_t wait_set;
  LIBC_NAMESPACE::sigemptyset(&wait_set);
  LIBC_NAMESPACE::sigaddset(&wait_set, SIGUSR1);

  // 1ms timeout — no signal pending, should wait ~1ms then return EAGAIN.
  timespec timeout = {0, 1000000}; // 1ms
  EXPECT_THAT(LIBC_NAMESPACE::sigtimedwait(&wait_set, nullptr, &timeout),
              Fails(EAGAIN));
}
