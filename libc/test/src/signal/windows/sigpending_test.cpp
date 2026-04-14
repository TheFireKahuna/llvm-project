//===-- Windows unittests for sigpending -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - Null set pointer returns EFAULT
//   - With no blocked signals, sigpending returns empty set
//   - After blocking and raising a signal, it appears in the pending set
//   - On unblock the pending signal is cleared from the set
//
//===----------------------------------------------------------------------===//

#include "src/signal/raise.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigaddset.h"
#include "src/signal/sigemptyset.h"
#include "src/signal/sigismember.h"
#include "src/signal/sigpending.h"
#include "src/signal/sigprocmask.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;

class LlvmLibcWindowsSigpendingTest
    : public LIBC_NAMESPACE::testing::ErrnoCheckingTest {
  sigset_t saved_mask;
  struct sigaction saved_usr1;

public:
  void SetUp() override {
    ErrnoCheckingTest::SetUp();
    LIBC_NAMESPACE::sigprocmask(0, nullptr, &saved_mask);
    LIBC_NAMESPACE::sigaction(SIGUSR1, nullptr, &saved_usr1);
    // Install SIG_IGN so the pending signal doesn't terminate us on unblock.
    struct sigaction ign;
    LIBC_NAMESPACE::sigemptyset(&ign.sa_mask);
    ign.sa_flags = 0;
    ign.sa_handler = SIG_IGN;
    LIBC_NAMESPACE::sigaction(SIGUSR1, &ign, nullptr);
  }

  void TearDown() override {
    LIBC_NAMESPACE::sigprocmask(SIG_SETMASK, &saved_mask, nullptr);
    LIBC_NAMESPACE::sigaction(SIGUSR1, &saved_usr1, nullptr);
    ErrnoCheckingTest::TearDown();
  }
};

// Null set pointer must return EFAULT.
TEST(LlvmLibcWindowsSigpendingBasic, NullSet) {
  EXPECT_THAT(LIBC_NAMESPACE::sigpending(nullptr), Fails(EFAULT));
}

// With nothing blocked, pending set is empty.
TEST_F(LlvmLibcWindowsSigpendingTest, EmptyWhenNothingBlocked) {
  sigset_t pending;
  ASSERT_THAT(LIBC_NAMESPACE::sigpending(&pending), Succeeds());
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&pending, SIGUSR1), 0);
}

// Block SIGUSR1, raise it — it must appear in the pending set.
TEST_F(LlvmLibcWindowsSigpendingTest, BlockedSignalAppearsPending) {
  sigset_t block;
  LIBC_NAMESPACE::sigemptyset(&block);
  LIBC_NAMESPACE::sigaddset(&block, SIGUSR1);
  ASSERT_THAT(LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &block, nullptr),
              Succeeds());

  LIBC_NAMESPACE::raise(SIGUSR1);

  sigset_t pending;
  ASSERT_THAT(LIBC_NAMESPACE::sigpending(&pending), Succeeds());
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&pending, SIGUSR1), 1);
}
