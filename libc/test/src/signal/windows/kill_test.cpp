//===-- Windows unittests for kill ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - kill(getpid(), SIGUSR1) delivers signal to self
//   - kill(getpid(), 0) succeeds (existence check)
//   - kill with invalid signal number returns EINVAL
//   - kill with invalid (nonexistent) pid returns ESRCH
//   - kill with negative pid returns ESRCH (process groups unsupported)
//
//===----------------------------------------------------------------------===//

#include "src/signal/kill.h"
#include "src/signal/raise.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigemptyset.h"
#include "src/unistd/getpid.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/types/pid_t.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;

class LlvmLibcWindowsKillTest
    : public LIBC_NAMESPACE::testing::ErrnoCheckingTest {
  struct sigaction saved_usr1;

public:
  void SetUp() override {
    ErrnoCheckingTest::SetUp();
    LIBC_NAMESPACE::sigaction(SIGUSR1, nullptr, &saved_usr1);
  }

  void TearDown() override {
    LIBC_NAMESPACE::sigaction(SIGUSR1, &saved_usr1, nullptr);
    ErrnoCheckingTest::TearDown();
  }
};

// Invalid signal number returns EINVAL.
TEST(LlvmLibcWindowsKillBasic, InvalidSignal) {
  EXPECT_THAT(LIBC_NAMESPACE::kill(1, -1), Fails(EINVAL));
  EXPECT_THAT(LIBC_NAMESPACE::kill(1, NSIG + 1), Fails(EINVAL));
}

// Nonexistent PID returns ESRCH.
TEST(LlvmLibcWindowsKillBasic, NonexistentPid) {
  // PID 0x7FFFFFFF is extremely unlikely to exist.
  EXPECT_THAT(LIBC_NAMESPACE::kill(0x7FFFFFFF, 0), Fails(ESRCH));
}

// Negative PID (process group) returns ESRCH — not supported on Windows.
TEST(LlvmLibcWindowsKillBasic, NegativePid) {
  EXPECT_THAT(LIBC_NAMESPACE::kill(-2, SIGUSR1), Fails(ESRCH));
}

// kill(getpid(), 0) is a self-existence check — must succeed.
TEST_F(LlvmLibcWindowsKillTest, SelfExistenceCheck) {
  pid_t self = LIBC_NAMESPACE::getpid();
  EXPECT_THAT(LIBC_NAMESPACE::kill(self, 0), Succeeds());
}

// kill(getpid(), SIGUSR1) delivers the signal to self.
TEST_F(LlvmLibcWindowsKillTest, SelfSignalDelivered) {
  static volatile int count;
  count = 0;

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  action.sa_handler = +[](int) { count++; };
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);

  pid_t self = LIBC_NAMESPACE::getpid();
  ASSERT_THAT(LIBC_NAMESPACE::kill(self, SIGUSR1), Succeeds());
  EXPECT_EQ(count, 1);
}
