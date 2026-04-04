//===-- Unittests for SIGPIPE generation on write -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "hdr/signal_macros.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigaddset.h"
#include "src/signal/sigemptyset.h"
#include "src/signal/sigismember.h"
#include "src/signal/sigpending.h"
#include "src/signal/sigprocmask.h"
#include "src/unistd/close.h"
#include "src/unistd/pipe.h"
#include "src/unistd/write.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

using LlvmLibcWriteSigpipeTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;

static volatile int sigpipe_count;

static void sigpipe_handler(int) { ++sigpipe_count; }

class LlvmLibcWriteSigpipeFixture : public LlvmLibcWriteSigpipeTest {
  sigset_t saved_mask;
  struct sigaction saved_sigpipe;

public:
  void SetUp() override {
    LlvmLibcWriteSigpipeTest::SetUp();
    ASSERT_EQ(LIBC_NAMESPACE::sigprocmask(0, nullptr, &saved_mask), 0);
    ASSERT_EQ(LIBC_NAMESPACE::sigaction(SIGPIPE, nullptr, &saved_sigpipe), 0);
  }

  void TearDown() override {
    struct sigaction ignore = {};
    ASSERT_EQ(LIBC_NAMESPACE::sigemptyset(&ignore.sa_mask), 0);
    ignore.sa_handler = SIG_IGN;
    ignore.sa_flags = 0;
    ASSERT_EQ(LIBC_NAMESPACE::sigaction(SIGPIPE, &ignore, nullptr), 0);
    ASSERT_EQ(LIBC_NAMESPACE::sigprocmask(SIG_SETMASK, &saved_mask, nullptr),
              0);
    ASSERT_EQ(LIBC_NAMESPACE::sigaction(SIGPIPE, &saved_sigpipe, nullptr), 0);
    LlvmLibcWriteSigpipeTest::TearDown();
  }
};

TEST_F(LlvmLibcWriteSigpipeFixture, BlockedWritePendsSigpipeUntilUnblock) {
  sigpipe_count = 0;

  struct sigaction action = {};
  ASSERT_EQ(LIBC_NAMESPACE::sigemptyset(&action.sa_mask), 0);
  action.sa_handler = sigpipe_handler;
  action.sa_flags = 0;
  ASSERT_EQ(LIBC_NAMESPACE::sigaction(SIGPIPE, &action, nullptr), 0);

  int pipefd[2];
  ASSERT_THAT(LIBC_NAMESPACE::pipe(pipefd), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::close(pipefd[0]), Succeeds(0));

  sigset_t sigpipe_set;
  ASSERT_EQ(LIBC_NAMESPACE::sigemptyset(&sigpipe_set), 0);
  ASSERT_EQ(LIBC_NAMESPACE::sigaddset(&sigpipe_set, SIGPIPE), 0);
  ASSERT_EQ(LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &sigpipe_set, nullptr), 0);

  constexpr char BYTE = 'x';
  EXPECT_THAT(LIBC_NAMESPACE::write(pipefd[1], &BYTE, 1),
              Fails<ssize_t>(EPIPE));
  EXPECT_EQ(sigpipe_count, 0);

  sigset_t pending;
  ASSERT_EQ(LIBC_NAMESPACE::sigpending(&pending), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&pending, SIGPIPE), 1);

  ASSERT_EQ(LIBC_NAMESPACE::sigprocmask(SIG_UNBLOCK, &sigpipe_set, nullptr), 0);
  EXPECT_EQ(sigpipe_count, 1);

  ASSERT_EQ(LIBC_NAMESPACE::sigpending(&pending), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&pending, SIGPIPE), 0);

  ASSERT_THAT(LIBC_NAMESPACE::close(pipefd[1]), Succeeds(0));
}

TEST_F(LlvmLibcWriteSigpipeFixture, IgnoredSigpipeIsNotLeftPendingWhenBlocked) {
  struct sigaction action = {};
  ASSERT_EQ(LIBC_NAMESPACE::sigemptyset(&action.sa_mask), 0);
  action.sa_handler = SIG_IGN;
  action.sa_flags = 0;
  ASSERT_EQ(LIBC_NAMESPACE::sigaction(SIGPIPE, &action, nullptr), 0);

  int pipefd[2];
  ASSERT_THAT(LIBC_NAMESPACE::pipe(pipefd), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::close(pipefd[0]), Succeeds(0));

  sigset_t sigpipe_set;
  ASSERT_EQ(LIBC_NAMESPACE::sigemptyset(&sigpipe_set), 0);
  ASSERT_EQ(LIBC_NAMESPACE::sigaddset(&sigpipe_set, SIGPIPE), 0);
  ASSERT_EQ(LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &sigpipe_set, nullptr), 0);

  constexpr char BYTE = 'y';
  EXPECT_THAT(LIBC_NAMESPACE::write(pipefd[1], &BYTE, 1),
              Fails<ssize_t>(EPIPE));

  sigset_t pending;
  ASSERT_EQ(LIBC_NAMESPACE::sigpending(&pending), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&pending, SIGPIPE), 0);

  ASSERT_THAT(LIBC_NAMESPACE::close(pipefd[1]), Succeeds(0));
}
