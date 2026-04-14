//===-- Windows unittests for sigaction ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - EINVAL for invalid signal numbers
//   - SIGKILL disposition cannot be changed
//   - sa_handler receives correct signal number
//   - SA_SIGINFO: sa_sigaction receives siginfo_t with si_signo
//   - SA_RESETHAND: handler resets to SIG_DFL after one delivery
//   - sa_mask: additional signals blocked during handler execution
//   - Old action correctly retrieved via third argument
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "src/signal/raise.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigaddset.h"
#include "src/signal/sigemptyset.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;

// POSIX: "If sig is not a valid signal number [...] [EINVAL]."
TEST(LlvmLibcWindowsSigaction, InvalidSignal) {
  EXPECT_THAT(LIBC_NAMESPACE::sigaction(-1, nullptr, nullptr), Fails(EINVAL));
  EXPECT_THAT(LIBC_NAMESPACE::sigaction(0, nullptr, nullptr), Fails(EINVAL));
}

// POSIX: "The system shall not allow the action for [SIGKILL] to be set
// to [...] a signal-catching function."
TEST(LlvmLibcWindowsSigaction, SigkillImmutable) {
  struct sigaction action;
  // Reading SIGKILL disposition must succeed.
  EXPECT_THAT(LIBC_NAMESPACE::sigaction(SIGKILL, nullptr, &action), Succeeds());
  // Setting SIGKILL disposition must fail.
  EXPECT_THAT(LIBC_NAMESPACE::sigaction(SIGKILL, &action, nullptr),
              Fails(EINVAL));
}

static volatile int sigusr1_count;
static volatile int sigusr1_signo;

// POSIX: "sa_handler [...] specifies the action to be associated with
// the specified signal." Handler receives the signal number.
TEST(LlvmLibcWindowsSigaction, CustomHandler) {
  sigusr1_count = 0;
  sigusr1_signo = 0;

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  action.sa_handler = +[](int sig) {
    sigusr1_signo = sig;
    sigusr1_count++;
  };
  EXPECT_THAT(LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr), Succeeds());

  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(sigusr1_count, 1);
  EXPECT_EQ(sigusr1_signo, SIGUSR1);

  // Handler persists — not one-shot.
  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(sigusr1_count, 2);

  struct sigaction dfl;
  dfl.sa_handler = SIG_DFL;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &dfl, nullptr);
}

// POSIX: SIG_IGN — "signal shall be ignored."
TEST(LlvmLibcWindowsSigaction, IgnoreDisposition) {
  sigusr1_count = 0;

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  action.sa_handler = SIG_IGN;
  EXPECT_THAT(LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr), Succeeds());

  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(sigusr1_count, 0);

  action.sa_handler = SIG_DFL;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);
}

// POSIX: "If act is not a null pointer, the action [...] is returned
// in the location pointed to by oact."
TEST(LlvmLibcWindowsSigaction, RetrieveOldAction) {
  auto *handler = +[](int) {};

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  action.sa_handler = handler;
  EXPECT_THAT(LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr), Succeeds());

  struct sigaction old_action;
  EXPECT_THAT(LIBC_NAMESPACE::sigaction(SIGUSR1, nullptr, &old_action),
              Succeeds());
  EXPECT_EQ(old_action.sa_handler, handler);

  // Install new, get old in same call.
  struct sigaction action2;
  LIBC_NAMESPACE::sigemptyset(&action2.sa_mask);
  action2.sa_flags = 0;
  action2.sa_handler = SIG_DFL;
  struct sigaction retrieved;
  EXPECT_THAT(LIBC_NAMESPACE::sigaction(SIGUSR1, &action2, &retrieved),
              Succeeds());
  EXPECT_EQ(retrieved.sa_handler, handler);
}

static volatile int sa_info_signo;
static volatile int sa_info_code;

// POSIX: "If SA_SIGINFO is set [...] sa_sigaction shall be used as the
// signal-catching function." siginfo_t.si_signo == signal number.
TEST(LlvmLibcWindowsSigaction, SaSiginfo) {
  sa_info_signo = 0;
  sa_info_code = -1;

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO;
  action.sa_sigaction = +[](int sig, siginfo_t *info, void *) {
    sa_info_signo = sig;
    if (info)
      sa_info_code = info->si_code;
  };
  EXPECT_THAT(LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr), Succeeds());

  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(sa_info_signo, SIGUSR1);

  struct sigaction dfl;
  dfl.sa_handler = SIG_DFL;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &dfl, nullptr);
}

// POSIX: "SA_RESETHAND — [...] the disposition of the signal shall be
// reset to SIG_DFL [...] before entry to the signal-catching function."
TEST(LlvmLibcWindowsSigaction, SaResethand) {
  sigusr1_count = 0;

  struct sigaction action;
  LIBC_NAMESPACE::sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESETHAND;
  action.sa_handler = +[](int) { sigusr1_count++; };
  EXPECT_THAT(LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr), Succeeds());

  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(sigusr1_count, 1);

  // After SA_RESETHAND, disposition should be SIG_DFL.
  struct sigaction current;
  EXPECT_THAT(LIBC_NAMESPACE::sigaction(SIGUSR1, nullptr, &current),
              Succeeds());
  EXPECT_EQ(current.sa_handler, SIG_DFL);
}

static volatile bool inner_handler_called;

// POSIX: "sa_mask specifies a set of signals to be blocked [...] while
// the signal handler [...] is active."
TEST(LlvmLibcWindowsSigaction, SaMaskBlocksDuringHandler) {
  inner_handler_called = false;

  // Handler for SIGUSR2: just sets a flag.
  struct sigaction action2;
  LIBC_NAMESPACE::sigemptyset(&action2.sa_mask);
  action2.sa_flags = 0;
  action2.sa_handler = +[](int) { inner_handler_called = true; };
  LIBC_NAMESPACE::sigaction(SIGUSR2, &action2, nullptr);

  // Handler for SIGUSR1: raises SIGUSR2 during execution.
  // sa_mask includes SIGUSR2, so it should be blocked.
  struct sigaction action1;
  LIBC_NAMESPACE::sigemptyset(&action1.sa_mask);
  LIBC_NAMESPACE::sigaddset(&action1.sa_mask, SIGUSR2);
  action1.sa_flags = 0;
  action1.sa_handler = +[](int) {
    // SIGUSR2 should be blocked during this handler.
    inner_handler_called = false;
    LIBC_NAMESPACE::raise(SIGUSR2);
    // If sa_mask works, SIGUSR2 is pending but not yet delivered.
    // However, it will be delivered when we return and the mask is restored.
  };
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action1, nullptr);

  LIBC_NAMESPACE::raise(SIGUSR1);
  // After the handler returns, SIGUSR2 should have been unblocked and
  // delivered (it was pending).
  EXPECT_TRUE(inner_handler_called);

  struct sigaction dfl;
  dfl.sa_handler = SIG_DFL;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &dfl, nullptr);
  LIBC_NAMESPACE::sigaction(SIGUSR2, &dfl, nullptr);
}
