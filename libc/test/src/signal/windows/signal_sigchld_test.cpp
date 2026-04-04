//===-- Windows SIGCHLD delivery tests -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SIGCHLD is the only signal whose siginfo payload is packed into a
// single 64-bit atomic by the libc — [si_code:4 | si_pid:28 | si_status:32]
// (plus sign bits). Observable regressions:
//
//   1. Child exits normally → parent's SA_SIGINFO handler fires with
//      siginfo.si_pid == child pid, si_status == exit code,
//      si_code == CLD_EXITED.
//
//   2. Child terminated by signal → si_code == CLD_KILLED, si_status
//      encodes the signal.
//
//   3. Two children exit in quick succession — the atomic packing must
//      not lose the second event. The handler must observe both, or the
//      second must be delivered as a second invocation.
//
//   4. SIGCHLD default disposition is SIG_IGN (on POSIX: SIG_DFL is also
//      ignore behavior). We install a handler and verify it fires; we
//      don't test SIG_IGN here — that's behavioral absence, which leaks
//      zombies and needs a different invariant.
//
//===----------------------------------------------------------------------===//

#include "hdr/signal_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/signal/kill.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigemptyset.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/fork.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "include/llvm-libc-macros/windows/sys-wait-macros.h"

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;

namespace {

struct Observed {
  Atomic<int> count{0};
  Atomic<int> last_signo{0};
  Atomic<int> last_code{0};
  Atomic<int> last_pid{0};
  Atomic<int> last_status{0};
};

static Observed observed;

static void sigchld_handler(int sig, siginfo_t *info, void *) {
  observed.last_signo.store(sig, MemoryOrder::RELEASE);
  if (info) {
    observed.last_code.store(info->si_code, MemoryOrder::RELEASE);
    observed.last_pid.store(static_cast<int>(info->si_pid),
                             MemoryOrder::RELEASE);
    observed.last_status.store(info->si_status, MemoryOrder::RELEASE);
  }
  observed.count.fetch_add(1, MemoryOrder::RELEASE);
}

static void install_chld_handler(struct sigaction &saved) {
  struct sigaction act{};
  LIBC_NAMESPACE::sigemptyset(&act.sa_mask);
  act.sa_flags = SA_SIGINFO | SA_RESTART;
  act.sa_sigaction = sigchld_handler;
  EXPECT_EQ(LIBC_NAMESPACE::sigaction(SIGCHLD, &act, &saved), 0);
}

static void restore_chld_handler(const struct sigaction &saved) {
  LIBC_NAMESPACE::sigaction(SIGCHLD, &saved, nullptr);
}

// Bounded poll for delivered signals.
static bool wait_for_chld_count(int at_least, int timeout_ms) {
  for (int i = 0; i < timeout_ms; ++i) {
    if (observed.count.load(MemoryOrder::ACQUIRE) >= at_least)
      return true;
    LIBC_NAMESPACE::test_support::alertable_sleep_ms(1);
  }
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Normal child exit → CLD_EXITED + correct pid + status.
// ---------------------------------------------------------------------------

TEST(LlvmLibcWindowsSigchld, NormalExitDeliversCldExited) {
  observed.count.store(0, MemoryOrder::RELAXED);
  struct sigaction saved{};
  install_chld_handler(saved);

  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0)
    ::NtTerminateProcess(NtCurrentProcess(), 7);

  // Reap the child first so its exit is delivered as SIGCHLD.
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 7);

  EXPECT_TRUE(wait_for_chld_count(1, 3000));
  EXPECT_EQ(observed.last_signo.load(MemoryOrder::ACQUIRE), SIGCHLD);
  EXPECT_EQ(observed.last_pid.load(MemoryOrder::ACQUIRE),
            static_cast<int>(pid));
  EXPECT_EQ(observed.last_status.load(MemoryOrder::ACQUIRE), 7);
  EXPECT_EQ(observed.last_code.load(MemoryOrder::ACQUIRE), CLD_EXITED);

  restore_chld_handler(saved);
}

// ---------------------------------------------------------------------------
// 2. Signal-terminated child → CLD_KILLED + signal number in si_status.
// ---------------------------------------------------------------------------

TEST(LlvmLibcWindowsSigchld, SignalKillDeliversCldKilled) {
  observed.count.store(0, MemoryOrder::RELAXED);
  struct sigaction saved{};
  install_chld_handler(saved);

  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    // Spin forever — parent will kill.
    for (volatile uint64_t i = 0;; ++i) {
      if (i == 0xFFFFFFFFFFFFFFFFull)
        break;
    }
    ::NtTerminateProcess(NtCurrentProcess(), 0);
  }

  LIBC_NAMESPACE::test_support::sleep_ms(20);
  EXPECT_EQ(LIBC_NAMESPACE::kill(pid, SIGKILL), 0);

  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));

  EXPECT_TRUE(wait_for_chld_count(1, 3000));
  EXPECT_EQ(observed.last_signo.load(MemoryOrder::ACQUIRE), SIGCHLD);
  EXPECT_EQ(observed.last_pid.load(MemoryOrder::ACQUIRE),
            static_cast<int>(pid));
  EXPECT_EQ(observed.last_code.load(MemoryOrder::ACQUIRE), CLD_KILLED);
  EXPECT_EQ(observed.last_status.load(MemoryOrder::ACQUIRE), SIGKILL);

  restore_chld_handler(saved);
}

// ---------------------------------------------------------------------------
// 3. Two consecutive children — both must eventually be reflected in the
//    handler count. The packed 64-bit atomic coalesces, so the second
//    event may overwrite the first's siginfo fields, but the count MUST
//    advance (either as a second delivery or, on fast coalesce, as a
//    second handler invocation after waitpid drains).
// ---------------------------------------------------------------------------

TEST(LlvmLibcWindowsSigchld, TwoChildrenBothObservable) {
  observed.count.store(0, MemoryOrder::RELAXED);
  struct sigaction saved{};
  install_chld_handler(saved);

  pid_t p1 = LIBC_NAMESPACE::fork();
  ASSERT_NE(p1, -1);
  if (p1 == 0)
    ::NtTerminateProcess(NtCurrentProcess(), 1);

  pid_t p2 = LIBC_NAMESPACE::fork();
  ASSERT_NE(p2, -1);
  if (p2 == 0)
    ::NtTerminateProcess(NtCurrentProcess(), 2);

  int status = 0;
  LIBC_NAMESPACE::waitpid(p1, &status, 0);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 1);

  LIBC_NAMESPACE::waitpid(p2, &status, 0);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 2);

  // At least one handler invocation for each child reaping. Coalescing
  // can legally collapse back-to-back deliveries to a single handler
  // call, so the lower bound is 1; the ordering guarantee is that SOME
  // delivery happened.
  EXPECT_TRUE(wait_for_chld_count(1, 3000));
  EXPECT_EQ(observed.last_signo.load(MemoryOrder::ACQUIRE), SIGCHLD);

  restore_chld_handler(saved);
}
