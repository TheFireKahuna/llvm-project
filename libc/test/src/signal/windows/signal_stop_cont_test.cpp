//===-- Windows SIGSTOP / SIGCONT three-phase protocol tests -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Verifies the SIGSTOP/SIGCONT cooperative three-phase stop protocol from
// the observable end: a child that receives SIGSTOP must transition to
// the stopped state (visible via waitpid WUNTRACED / WIFSTOPPED), and a
// subsequent SIGCONT must resume it (visible via WIFCONTINUED when the
// parent waited with WCONTINUED).
//
// The implementation's three phases (cooperative APC + alert broadcast,
// second APC round, suspend-APC-resume + hard-suspend fallback) are
// exercised by varying what the child is doing at the moment SIGSTOP
// arrives:
//
//   Scenario A — child spinning in user code.
//     Phase 1 cooperative APC should catch it; no kernel involvement
//     beyond alert delivery.
//
//   Scenario B — child parked in a kernel wait (nanosleep).
//     Phase 1 spin doesn't terminate — the thread is inside the kernel.
//     Phase 2 second APC round must either interrupt the sleep via APC
//     or, failing that, Phase 3 suspend-APC-resume delivers. Either way
//     WIFSTOPPED must be observable within our timeout.
//
//   Scenario C — SIGCONT before SIGSTOP is a no-op.
//     POSIX: SIGCONT targeting a non-stopped process discards pending
//     SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU but has no other effect.
//
//   Scenario D — two stop/cont cycles on the same child.
//     Exercises the coord-death recovery + state machine re-entry.
//
//===----------------------------------------------------------------------===//

#include "hdr/signal_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/signal/kill.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/fork.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "include/llvm-libc-macros/windows/sys-wait-macros.h"

namespace {

// Poll waitpid with WNOHANG until it returns non-zero, bounded by timeout.
static int poll_waitpid(pid_t pid, int options, int timeout_ms) {
  int status = 0;
  for (int i = 0; i < timeout_ms; ++i) {
    pid_t r = LIBC_NAMESPACE::waitpid(pid, &status, options | WNOHANG);
    if (r == pid)
      return status;
    LIBC_NAMESPACE::test_support::sleep_ms(1);
  }
  return -1; // sentinel for "timed out"
}

// Child body: spin forever in user code. Parent stops + terminates.
[[noreturn]] static void child_spin() {
  for (volatile uint64_t i = 0;; ++i) {
    // Prevent the optimizer from folding this loop.
    if (i == 0xFFFFFFFFFFFFFFFFull)
      break;
  }
  ::NtTerminateProcess(NtCurrentProcess(), 0);
  __builtin_unreachable();
}

// Child body: kernel-sleep forever (5-minute intervals; never wakes on its own).
[[noreturn]] static void child_sleep() {
  for (;;) {
    LARGE_INTEGER interval = {};
    interval.QuadPart = -10000LL * 300'000LL; // 5 minutes
    ::NtDelayExecution(FALSE, &interval);
  }
}

} // namespace

// ---------------------------------------------------------------------------
// A. SIGSTOP/SIGCONT on a spinning child (user-mode thread).
// ---------------------------------------------------------------------------

TEST(LlvmLibcWindowsSignalStopCont, SpinningChildStopsAndContinues) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) child_spin();

  // Small delay to let the child enter its spin loop.
  LIBC_NAMESPACE::test_support::sleep_ms(20);

  EXPECT_EQ(LIBC_NAMESPACE::kill(pid, SIGSTOP), 0);
  int s = poll_waitpid(pid, WUNTRACED, 3000);
  ASSERT_NE(s, -1);
  EXPECT_TRUE(WIFSTOPPED(s));
  EXPECT_EQ(WSTOPSIG(s), SIGSTOP);

  EXPECT_EQ(LIBC_NAMESPACE::kill(pid, SIGCONT), 0);
  s = poll_waitpid(pid, WCONTINUED, 3000);
  ASSERT_NE(s, -1);
  EXPECT_TRUE(WIFCONTINUED(s));

  // Reap with SIGKILL.
  EXPECT_EQ(LIBC_NAMESPACE::kill(pid, SIGKILL), 0);
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));
}

// ---------------------------------------------------------------------------
// B. SIGSTOP on a child blocked in a kernel wait — exercises Phase 2/3.
// ---------------------------------------------------------------------------

TEST(LlvmLibcWindowsSignalStopCont, SleepingChildStopsViaDeepPhase) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) child_sleep();

  // Give the child time to enter the kernel wait.
  LIBC_NAMESPACE::test_support::sleep_ms(50);

  EXPECT_EQ(LIBC_NAMESPACE::kill(pid, SIGSTOP), 0);
  // Generous timeout: Phase 3 hard-suspend fallback can take noticeably
  // longer than Phase 1.
  int s = poll_waitpid(pid, WUNTRACED, 5000);
  ASSERT_NE(s, -1);
  EXPECT_TRUE(WIFSTOPPED(s));
  EXPECT_EQ(WSTOPSIG(s), SIGSTOP);

  EXPECT_EQ(LIBC_NAMESPACE::kill(pid, SIGCONT), 0);
  s = poll_waitpid(pid, WCONTINUED, 5000);
  ASSERT_NE(s, -1);
  EXPECT_TRUE(WIFCONTINUED(s));

  EXPECT_EQ(LIBC_NAMESPACE::kill(pid, SIGKILL), 0);
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));
}

// ---------------------------------------------------------------------------
// C. SIGCONT on a not-yet-stopped child must not surface WIFCONTINUED.
//    (POSIX: "If there is a pending stop signal [...] discard it", but
//    otherwise no visible effect.)
// ---------------------------------------------------------------------------

TEST(LlvmLibcWindowsSignalStopCont, ContBeforeStopIsInert) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) child_spin();

  LIBC_NAMESPACE::test_support::sleep_ms(20);
  EXPECT_EQ(LIBC_NAMESPACE::kill(pid, SIGCONT), 0);

  // 100ms grace: there must be NO continuation notification (child never
  // stopped).
  int status = 0;
  for (int i = 0; i < 100; ++i) {
    pid_t r = LIBC_NAMESPACE::waitpid(pid, &status,
                                      WCONTINUED | WUNTRACED | WNOHANG);
    ASSERT_NE(r, pid); // must not surface any state change
    LIBC_NAMESPACE::test_support::sleep_ms(1);
  }

  EXPECT_EQ(LIBC_NAMESPACE::kill(pid, SIGKILL), 0);
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));
}

// ---------------------------------------------------------------------------
// D. Two stop/cont cycles — state machine survives re-entry.
// ---------------------------------------------------------------------------

TEST(LlvmLibcWindowsSignalStopCont, TwoStopContCycles) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) child_spin();

  LIBC_NAMESPACE::test_support::sleep_ms(20);

  for (int round = 0; round < 2; ++round) {
    EXPECT_EQ(LIBC_NAMESPACE::kill(pid, SIGSTOP), 0);
    int s = poll_waitpid(pid, WUNTRACED, 3000);
    ASSERT_NE(s, -1);
    EXPECT_TRUE(WIFSTOPPED(s));
    EXPECT_EQ(WSTOPSIG(s), SIGSTOP);

    EXPECT_EQ(LIBC_NAMESPACE::kill(pid, SIGCONT), 0);
    s = poll_waitpid(pid, WCONTINUED, 3000);
    ASSERT_NE(s, -1);
    EXPECT_TRUE(WIFCONTINUED(s));
  }

  EXPECT_EQ(LIBC_NAMESPACE::kill(pid, SIGKILL), 0);
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));
}
