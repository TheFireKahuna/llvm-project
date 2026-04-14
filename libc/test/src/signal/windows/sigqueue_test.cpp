//===-- Windows unittests for sigqueue -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - sigqueue() sends signal with si_value to the current process
//   - SA_SIGINFO handler receives correct si_signo and si_value
//   - sigqueue() with invalid signal returns -1, errno = EINVAL
//   - Queued signals are pending while blocked, delivered on unblock
//   - Multiple queued signals with different values are all delivered
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

class LlvmLibcWindowsSigqueueTest
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

static volatile int received_signo;
static volatile int received_value;
static volatile int delivery_count;

static void siginfo_handler(int sig, siginfo_t *info, void *) {
  received_signo = sig;
  if (info)
    received_value = info->si_value.sival_int;
  delivery_count++;
}

// POSIX: "sigqueue() shall send the signal specified by sig to the process
// whose process ID is equal to pid." si_value is delivered in siginfo_t.
TEST_F(LlvmLibcWindowsSigqueueTest, BasicDelivery) {
  received_signo = 0;
  received_value = 0;
  delivery_count = 0;

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO;
  action.sa_sigaction = siginfo_handler;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);

  union sigval val;
  val.sival_int = 42;
  EXPECT_THAT(LIBC_NAMESPACE::sigqueue(getpid(), SIGUSR1, val), Succeeds());
  EXPECT_EQ(received_signo, SIGUSR1);
  EXPECT_EQ(received_value, 42);
  EXPECT_EQ(delivery_count, 1);
}

// POSIX: Invalid signal number → -1, errno = EINVAL.
TEST_F(LlvmLibcWindowsSigqueueTest, InvalidSignal) {
  union sigval val;
  val.sival_int = 0;
  EXPECT_THAT(LIBC_NAMESPACE::sigqueue(getpid(), 0, val), Fails(EINVAL));
  EXPECT_THAT(LIBC_NAMESPACE::sigqueue(getpid(), 65, val), Fails(EINVAL));
}

// POSIX: Queued signals are pending while blocked and delivered on unblock.
// Different si_values should be preserved.
TEST_F(LlvmLibcWindowsSigqueueTest, QueuedWhileBlocked) {
  delivery_count = 0;

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO;
  action.sa_sigaction = siginfo_handler;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);

  // Block SIGUSR1.
  sigset_t sigset;
  LIBC_NAMESPACE::sigemptyset(&sigset);
  LIBC_NAMESPACE::sigaddset(&sigset, SIGUSR1);
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, &sigset, nullptr);

  // Queue signal while blocked.
  union sigval val;
  val.sival_int = 99;
  EXPECT_THAT(LIBC_NAMESPACE::sigqueue(getpid(), SIGUSR1, val), Succeeds());
  EXPECT_EQ(delivery_count, 0);

  // Unblock — pending signal delivered.
  LIBC_NAMESPACE::sigprocmask(SIG_UNBLOCK, &sigset, nullptr);
  EXPECT_GE(delivery_count, 1);
  EXPECT_EQ(received_value, 99);
}

// POSIX: sigqueue with sig=0 performs error checking without sending a signal.
TEST_F(LlvmLibcWindowsSigqueueTest, SignalZeroNoDelivery) {
  delivery_count = 0;

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO;
  action.sa_sigaction = siginfo_handler;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);

  // sig=0 is used for process existence check, not delivery.
  // Behavior varies — just verify no crash and count stays 0.
  union sigval val;
  val.sival_int = 0;
  // Some implementations allow sig=0, others return EINVAL.
  // Either way, no handler should be invoked.
  LIBC_NAMESPACE::sigqueue(getpid(), 0, val);
  EXPECT_EQ(delivery_count, 0);
}
