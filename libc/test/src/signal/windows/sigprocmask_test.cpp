//===-- Windows unittests for sigprocmask ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - EINVAL for invalid 'how' values
//   - SIG_BLOCK adds to the current mask
//   - SIG_UNBLOCK removes from the current mask
//   - SIG_SETMASK replaces the mask entirely
//   - Pending signals delivered on unblock
//   - Old mask returned via oldset
//   - Null set with non-null oldset just reads current mask
//
//===----------------------------------------------------------------------===//

#include "hdr/signal_macros.h"
#include "src/signal/raise.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigaddset.h"
#include "src/signal/sigemptyset.h"
#include "src/signal/sigismember.h"
#include "src/signal/sigprocmask.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

class LlvmLibcWindowsSigprocmaskTest
    : public LIBC_NAMESPACE::testing::ErrnoCheckingTest {
  sigset_t saved_mask;
  struct sigaction saved_usr1;
  struct sigaction saved_usr2;

public:
  void SetUp() override {
    ErrnoCheckingTest::SetUp();
    LIBC_NAMESPACE::sigprocmask(0, nullptr, &saved_mask);
    LIBC_NAMESPACE::sigaction(SIGUSR1, nullptr, &saved_usr1);
    LIBC_NAMESPACE::sigaction(SIGUSR2, nullptr, &saved_usr2);
  }

  void TearDown() override {
    LIBC_NAMESPACE::sigprocmask(SIG_SETMASK, &saved_mask, nullptr);
    LIBC_NAMESPACE::sigaction(SIGUSR1, &saved_usr1, nullptr);
    LIBC_NAMESPACE::sigaction(SIGUSR2, &saved_usr2, nullptr);
    ErrnoCheckingTest::TearDown();
  }
};

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;

static volatile int handler_called;

static void test_handler(int) { handler_called++; }

// POSIX: "If how has a value other than those [...] sigprocmask() shall
// return -1 with errno set to [EINVAL]."
TEST_F(LlvmLibcWindowsSigprocmaskTest, InvalidHow) {
  sigset_t valid;
  LIBC_NAMESPACE::sigemptyset(&valid);
  EXPECT_THAT(LIBC_NAMESPACE::sigprocmask(17, &valid, nullptr), Fails(EINVAL));
  EXPECT_THAT(LIBC_NAMESPACE::sigprocmask(-4, &valid, nullptr), Fails(EINVAL));
}

// POSIX: "SIG_BLOCK — The resulting set shall be the union of the
// current set and the signal set pointed to by set."
// Also: pending signals are delivered when unblocked.
TEST_F(LlvmLibcWindowsSigprocmaskTest, BlockThenUnblockDelivers) {
  handler_called = 0;

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  action.sa_handler = test_handler;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);

  // Block SIGUSR1.
  sigset_t sigset;
  LIBC_NAMESPACE::sigemptyset(&sigset);
  LIBC_NAMESPACE::sigaddset(&sigset, SIGUSR1);
  EXPECT_EQ(LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &sigset, nullptr), 0);

  // Signal while blocked — handler must not run yet.
  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(handler_called, 0);

  // POSIX: "If there are any pending unblocked signals [...] at least
  // one of those signals shall be delivered before [sigprocmask] returns."
  EXPECT_EQ(LIBC_NAMESPACE::sigprocmask(SIG_UNBLOCK, &sigset, nullptr), 0);
  EXPECT_EQ(handler_called, 1);
}

// POSIX: "SIG_SETMASK — The resulting set shall be the signal set
// pointed to by set."
TEST_F(LlvmLibcWindowsSigprocmaskTest, SetmaskReplacesEntireMask) {
  handler_called = 0;

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  action.sa_handler = test_handler;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);

  // Block SIGUSR1 via SIG_SETMASK.
  sigset_t block_set;
  LIBC_NAMESPACE::sigemptyset(&block_set);
  LIBC_NAMESPACE::sigaddset(&block_set, SIGUSR1);
  EXPECT_EQ(LIBC_NAMESPACE::sigprocmask(SIG_SETMASK, &block_set, nullptr), 0);

  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(handler_called, 0);

  // Replace with empty set — all signals unblocked, pending delivered.
  sigset_t empty;
  LIBC_NAMESPACE::sigemptyset(&empty);
  EXPECT_EQ(LIBC_NAMESPACE::sigprocmask(SIG_SETMASK, &empty, nullptr), 0);
  EXPECT_EQ(handler_called, 1);
}

// POSIX: "If set is a null pointer, the value of how is not significant
// and the thread's signal mask shall not be changed [...] thus the call
// can be used to enquire about currently blocked signals."
TEST_F(LlvmLibcWindowsSigprocmaskTest, NullSetReadsCurrentMask) {
  sigset_t block_set;
  LIBC_NAMESPACE::sigemptyset(&block_set);
  LIBC_NAMESPACE::sigaddset(&block_set, SIGUSR2);
  EXPECT_EQ(LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &block_set, nullptr), 0);

  sigset_t current;
  EXPECT_EQ(LIBC_NAMESPACE::sigprocmask(0, nullptr, &current), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&current, SIGUSR2), 1);
}

// POSIX: Old mask returned in oldset when both set and oldset are non-null.
TEST_F(LlvmLibcWindowsSigprocmaskTest, OldMaskReturned) {
  // Start with empty mask.
  sigset_t empty;
  LIBC_NAMESPACE::sigemptyset(&empty);
  LIBC_NAMESPACE::sigprocmask(SIG_SETMASK, &empty, nullptr);

  // Block SIGUSR1, get old mask.
  sigset_t new_set, old_set;
  LIBC_NAMESPACE::sigemptyset(&new_set);
  LIBC_NAMESPACE::sigaddset(&new_set, SIGUSR1);
  EXPECT_EQ(LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &new_set, &old_set), 0);

  // Old mask should not contain SIGUSR1.
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&old_set, SIGUSR1), 0);

  // Block SIGUSR2, get old mask — should now contain SIGUSR1.
  sigset_t new_set2, old_set2;
  LIBC_NAMESPACE::sigemptyset(&new_set2);
  LIBC_NAMESPACE::sigaddset(&new_set2, SIGUSR2);
  EXPECT_EQ(LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &new_set2, &old_set2), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&old_set2, SIGUSR1), 1);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&old_set2, SIGUSR2), 0);
}

// POSIX: Multiple raises of the same signal while blocked need deliver
// at least once on unblock (standard says "at least one").
TEST_F(LlvmLibcWindowsSigprocmaskTest, MultipleRaisesWhileBlocked) {
  handler_called = 0;

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  action.sa_handler = test_handler;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);

  sigset_t sigset;
  LIBC_NAMESPACE::sigemptyset(&sigset);
  LIBC_NAMESPACE::sigaddset(&sigset, SIGUSR1);
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &sigset, nullptr);

  // Raise multiple times while blocked.
  LIBC_NAMESPACE::raise(SIGUSR1);
  LIBC_NAMESPACE::raise(SIGUSR1);
  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(handler_called, 0);

  // POSIX: at least one delivery on unblock.
  LIBC_NAMESPACE::sigprocmask(SIG_UNBLOCK, &sigset, nullptr);
  EXPECT_GE(handler_called, 1);
}
