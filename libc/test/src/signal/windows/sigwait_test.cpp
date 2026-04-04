//===-- Windows unittests for sigwait/sigwaitinfo/sigsuspend ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - sigwait() consumes a blocked pending signal; returns 0, sets *sig
//   - sigwait() returns EINVAL when the set pointer is null
//   - sigwaitinfo() consumes a blocked pending signal; fills siginfo_t
//   - sigwaitinfo() returns the signal number on success
//   - sigsuspend(NULL) returns -1, errno=EFAULT
//   - sigsuspend atomically installs mask and dispatches already-pending
//     signal that the new mask unblocks; returns -1, errno=EINTR
//
//===----------------------------------------------------------------------===//

#include "hdr/signal_macros.h"
#include "src/signal/raise.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigaddset.h"
#include "src/signal/sigemptyset.h"
#include "src/signal/sigprocmask.h"
#include "src/signal/sigqueue.h"
#include "src/signal/sigsuspend.h"
#include "src/signal/sigwait.h"
#include "src/signal/sigwaitinfo.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include <unistd.h> // getpid

class LlvmLibcWindowsSigwaitTest
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
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;

static volatile int g_handler_ran;
static void usr1_handler(int) { g_handler_ran = 1; }

// sigwait consumes a blocked pending signal; returns 0 and fills *sig.
TEST_F(LlvmLibcWindowsSigwaitTest, ConsumePendingSignal) {
  sigset_t block;
  LIBC_NAMESPACE::sigemptyset(&block);
  LIBC_NAMESPACE::sigaddset(&block, SIGUSR1);
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &block, nullptr);

  LIBC_NAMESPACE::raise(SIGUSR1);

  sigset_t wait_set;
  LIBC_NAMESPACE::sigemptyset(&wait_set);
  LIBC_NAMESPACE::sigaddset(&wait_set, SIGUSR1);

  int sig = -1;
  int ret = LIBC_NAMESPACE::sigwait(&wait_set, &sig);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(sig, SIGUSR1);
}

// sigwait returns EINVAL when either pointer is null.
TEST_F(LlvmLibcWindowsSigwaitTest, NullSetReturnsEinval) {
  int sig = -1;
  int ret = LIBC_NAMESPACE::sigwait(nullptr, &sig);
  EXPECT_EQ(ret, EINVAL);
}

// sigwaitinfo returns the signal number and fills siginfo_t.si_signo.
TEST_F(LlvmLibcWindowsSigwaitTest, SigwaitinfoFillsInfo) {
  sigset_t block;
  LIBC_NAMESPACE::sigemptyset(&block);
  LIBC_NAMESPACE::sigaddset(&block, SIGUSR1);
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &block, nullptr);

  LIBC_NAMESPACE::raise(SIGUSR1);

  sigset_t wait_set;
  LIBC_NAMESPACE::sigemptyset(&wait_set);
  LIBC_NAMESPACE::sigaddset(&wait_set, SIGUSR1);

  siginfo_t info;
  int sig = LIBC_NAMESPACE::sigwaitinfo(&wait_set, &info);
  EXPECT_EQ(sig, SIGUSR1);
  EXPECT_EQ(info.si_signo, SIGUSR1);
}

// sigwaitinfo fills si_value when the signal was sent via sigqueue.
TEST_F(LlvmLibcWindowsSigwaitTest, SigwaitinfoSiValue) {
  sigset_t block;
  LIBC_NAMESPACE::sigemptyset(&block);
  LIBC_NAMESPACE::sigaddset(&block, SIGUSR1);
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &block, nullptr);

  union sigval val;
  val.sival_int = 7777;
  LIBC_NAMESPACE::sigqueue(getpid(), SIGUSR1, val);

  sigset_t wait_set;
  LIBC_NAMESPACE::sigemptyset(&wait_set);
  LIBC_NAMESPACE::sigaddset(&wait_set, SIGUSR1);

  siginfo_t info;
  int sig = LIBC_NAMESPACE::sigwaitinfo(&wait_set, &info);
  EXPECT_EQ(sig, SIGUSR1);
  EXPECT_EQ(info.si_signo, SIGUSR1);
  EXPECT_EQ(info.si_value.sival_int, 7777);
}

// sigsuspend(nullptr) → -1, errno=EFAULT.
TEST_F(LlvmLibcWindowsSigwaitTest, SigsuspendNullMask) {
  EXPECT_THAT(LIBC_NAMESPACE::sigsuspend(nullptr), Fails(EFAULT));
}

// sigsuspend dispatches a signal that the temporary mask unblocks, then
// returns -1/EINTR. The pending signal was queued while blocked; sigsuspend
// installs an empty mask (nothing blocked) and immediately dispatches it.
TEST_F(LlvmLibcWindowsSigwaitTest, SigsuspendDeliversPending) {
  g_handler_ran = 0;

  // Install handler for SIGUSR1.
  struct sigaction sa;
  LIBC_NAMESPACE::sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = usr1_handler;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &sa, nullptr);

  // Block SIGUSR1 and make it pending.
  sigset_t block;
  LIBC_NAMESPACE::sigemptyset(&block);
  LIBC_NAMESPACE::sigaddset(&block, SIGUSR1);
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &block, nullptr);
  LIBC_NAMESPACE::raise(SIGUSR1);

  // sigsuspend with empty mask: SIGUSR1 is now unblocked → dispatched
  // immediately → handler runs → returns -1/EINTR.
  sigset_t empty;
  LIBC_NAMESPACE::sigemptyset(&empty);
  EXPECT_THAT(LIBC_NAMESPACE::sigsuspend(&empty), Fails(EINTR));
  EXPECT_EQ(g_handler_ran, 1);
}
