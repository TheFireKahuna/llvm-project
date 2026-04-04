//===-- Fork-then-SEGV-in-child VEH re-registration test ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Witness for veh_core_fork_reinit() actually re-installing the master VEH
// in the forked child.
//
// NT does NOT inherit AddVectoredExceptionHandler registrations across
// RtlCloneUserProcess, the child gets a fresh TEB, and TLS slot contents are
// not preserved. The libc fork path therefore calls veh_core_fork_reinit()
// (see libc/src/__support/OSUtil/windows/veh/veh_core.cpp,
// veh_core_fork_reinit_impl) which:
//
//   1. RtlAddVectoredExceptionHandler(1, master_veh_handler) — re-front the
//      master handler on the (empty) child VEH chain.
//   2. Stores the new opaque handle into the COW'd Zone-0b slot, replacing
//      the parent's stale handle which would otherwise be a kernel object
//      that no longer matches the child's empty chain.
//   3. LdrRegisterDllNotification — re-arm the per-DLL load/unload callback
//      so subsequent loader activity in the child re-fronts the handler at
//      the head of the chain.
//
// If step (1) regressed (e.g. someone removed the RtlAdd call from the
// child reinit path because "it's CoW so the handle is still valid"), an
// AV in the child would propagate past the (non-existent) libc dispatch
// to the unhandled-exception filter and tear the process down with
// STATUS_ACCESS_VIOLATION translated to whatever default the kernel picks
// (typically a non-SIGSEGV exit code from our mapping). Either way the
// outcomes asserted below would change.
//
// We exercise the contract end-to-end through the public POSIX API
// surface — sigaction() / fork() / waitpid() — exactly as user code would,
// to ensure the whole chain (kernel exception → master VEH → signal_state
// VEH transport → pending → dispatch → user handler) survives fork.
//
// Two scenarios:
//
//   1. ChildHandlerCatchesSegv — parent installs a SIGSEGV handler with
//      SA_SIGINFO that uses siglongjmp() to escape the faulting context.
//      Disposition tables are copied across fork, so the child inherits the
//      handler. Child triggers a null-deref. If veh_core_fork_reinit()
//      did its job, the master VEH catches the AV in the child, the
//      signal subsystem maps it to SIGSEGV, the user handler runs, the
//      siglongjmp transfers control out of the faulting frame, and the
//      child exits with WEXITSTATUS == kHandlerExitCode. If VEH was not
//      re-registered, the AV is unhandled and the child dies with a
//      different status (or signal).
//
//   2. ChildDefaultDispositionTerminatesWithSigsegv — child sets the
//      disposition back to SIG_DFL before faulting. The libc default
//      action for SIGSEGV is core-dump-style termination. The kernel AV
//      must still flow through the libc VEH (re-registered in the child)
//      to be delivered as a SIGSEGV — not as raw STATUS_ACCESS_VIOLATION
//      escaping to the unhandled-exception filter. Parent asserts
//      WIFSIGNALED && WTERMSIG == SIGSEGV.
//
// Both scenarios fail in characteristic ways if veh_core_fork_reinit() is
// broken: scenario 1 would NOT report WEXITSTATUS == kHandlerExitCode
// (the handler never runs because no master VEH catches the AV), and
// scenario 2 would NOT report WTERMSIG == SIGSEGV (the AV propagates past
// the missing VEH to the kernel-default termination, which our mapping
// surfaces as a different status).
//
//===----------------------------------------------------------------------===//

#include "test/UnitTest/Test.h"

#include "src/__support/macros/config.h"
#include "src/setjmp/siglongjmp.h"
#include "src/setjmp/sigsetjmp.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigemptyset.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/_exit.h"
#include "src/unistd/fork.h"

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Distinct from any plausible default exit code so a regression that lets
// the child fall through (e.g. the handler never ran but we somehow exited
// 0) cannot be confused with success.
constexpr int kHandlerExitCode = 42;

// Set inside the child's SIGSEGV handler before siglongjmp. The parent
// never observes this directly — it only sees the child's exit status —
// but the variable is consulted in the child's post-longjmp epilogue to
// guarantee the handler actually ran (and the exit code is not just a
// stray value from an uninitialised path).
volatile sig_atomic_t g_handler_ran = 0;

// sigjmp_buf for the siglongjmp escape from the faulting frame. Must be
// process-global so the handler (which has no user_data plumbing on
// SA_SIGINFO without sigaltstack tricks) can find it.
sigjmp_buf g_segv_jmp;

// SIGSEGV handler installed by scenario 1. SA_SIGINFO so we get the
// si_signo / si_code in case future iterations of this test want to
// assert on them; current body just records the handler ran and jumps.
extern "C" void segv_handler(int sig, siginfo_t * /*info*/,
                             void * /*ucontext*/) {
  // The flag is the "handler actually ran" witness — exit code alone is
  // not enough because a stray exit(42) elsewhere would also set it.
  if (sig == SIGSEGV)
    g_handler_ran = 1;
  // siglongjmp out of the faulting frame. Returning normally from a
  // SIGSEGV handler that was triggered by an AV would re-execute the
  // faulting instruction and re-fault forever.
  LIBC_NAMESPACE::siglongjmp(g_segv_jmp, 1);
}

// Force the null deref through a function the optimiser cannot fold away.
// `volatile` on the dest pointer is the standard recipe; the noinline
// keeps the offending instruction at a single, debuggable callsite.
[[gnu::noinline]] void deref_null() {
  *static_cast<volatile int *>(nullptr) = 0;
}

} // namespace

// ===----------------------------------------------------------------------===
// Scenario 1: child inherits handler → handler runs → siglongjmp → exit 42.
// ===----------------------------------------------------------------------===

TEST(LlvmLibcForkThenSegvInChild, ChildHandlerCatchesSegv) {
  // Install the SIGSEGV handler in the parent so the disposition table
  // contains the registration at fork time. The child's libc fork path
  // copies dispositions, so the child inherits this same handler.
  struct sigaction sa = {};
  sa.sa_sigaction = segv_handler;
  sa.sa_flags = SA_SIGINFO;
  LIBC_NAMESPACE::sigemptyset(&sa.sa_mask);
  // Save the parent's old handler so we can restore on the way out and
  // not destabilise the test runner's own signal state if some sibling
  // test (or the framework) relies on the default disposition.
  struct sigaction old_sa = {};
  ASSERT_EQ(LIBC_NAMESPACE::sigaction(SIGSEGV, &sa, &old_sa), 0);

  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);

  if (pid == 0) {
    // ----- Child -----
    // The child has:
    //   * a fresh TEB (NT semantics) — TLS slots are zeroed
    //   * a CoW'd PEB / Zone 0 / Zone 0b — VEH dispatch table inherited
    //   * a fresh kernel VEH chain — handler handles do NOT survive
    //     RtlCloneUserProcess
    //
    // veh_core_fork_reinit() must have already re-registered the master
    // VEH before returning to user code. If not, the AV below escapes
    // and the child dies via the kernel default (NOT SIGSEGV through
    // our mapping), so the parent's WEXITSTATUS check trips.
    if (sigsetjmp(g_segv_jmp, 1) == 0) {
      deref_null();
      // Should be unreachable. If we get here, no AV was raised — that
      // would be a different (and even worse) regression.
      LIBC_NAMESPACE::_exit(99);
    }
    // ----- Post-longjmp epilogue -----
    // Witness: handler must have set g_handler_ran. Exit code alone is
    // not sufficient because an unrelated _exit(42) would mimic success.
    if (g_handler_ran != 1)
      LIBC_NAMESPACE::_exit(98);
    LIBC_NAMESPACE::_exit(kHandlerExitCode);
  }

  // ----- Parent -----
  int status = 0;
  pid_t reaped = LIBC_NAMESPACE::waitpid(pid, &status, 0);
  // Restore parent's old SIGSEGV disposition before any assert can early-out.
  LIBC_NAMESPACE::sigaction(SIGSEGV, &old_sa, nullptr);

  ASSERT_EQ(reaped, pid);
  // Child must have exited normally (handler ran, longjmp landed,
  // _exit(kHandlerExitCode) ran) — NOT been signalled to death.
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_FALSE(WIFSIGNALED(status));
  // The exact exit code: 42. Anything else is a regression:
  //   98  → handler ran but g_handler_ran witness was wrong (impossible
  //         unless the handler signature changed)
  //   99  → no AV was raised at all (deref_null was elided)
  //   any → master VEH was not re-registered in the child
  EXPECT_EQ(WEXITSTATUS(status), kHandlerExitCode);
}

// ===----------------------------------------------------------------------===
// Scenario 2: child resets disposition → AV → SIG_DFL action terminates
// the child with SIGSEGV (delivered through the re-registered VEH).
// ===----------------------------------------------------------------------===

TEST(LlvmLibcForkThenSegvInChild, ChildDefaultDispositionTerminatesWithSigsegv) {
  // Parent: install a handler that would catch a fault in the parent so
  // the test runner stays alive. We never expect this handler to fire in
  // the parent — we only ever fault in the child — but installing it
  // guarantees that if the test body itself faults for any reason we get
  // a clean siglongjmp instead of taking down the whole test binary.
  struct sigaction sa = {};
  sa.sa_sigaction = segv_handler;
  sa.sa_flags = SA_SIGINFO;
  LIBC_NAMESPACE::sigemptyset(&sa.sa_mask);
  struct sigaction old_sa = {};
  ASSERT_EQ(LIBC_NAMESPACE::sigaction(SIGSEGV, &sa, &old_sa), 0);

  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);

  if (pid == 0) {
    // ----- Child -----
    // Reset to SIG_DFL so the libc default action for SIGSEGV (terminate
    // with the signal as the wait status) takes effect when the AV is
    // mapped to SIGSEGV. This is the key signal: WTERMSIG == SIGSEGV
    // proves the libc VEH caught the AV and routed it through the signal
    // subsystem. If the VEH was not re-registered in the child, the
    // kernel-default unhandled-exception filter handles the AV and the
    // child's wait status would be WIFSIGNALED with a different signal
    // (or WIFEXITED with a status code), not WTERMSIG == SIGSEGV.
    struct sigaction dfl = {};
    dfl.sa_handler = SIG_DFL;
    LIBC_NAMESPACE::sigemptyset(&dfl.sa_mask);
    dfl.sa_flags = 0;
    if (LIBC_NAMESPACE::sigaction(SIGSEGV, &dfl, nullptr) != 0)
      LIBC_NAMESPACE::_exit(97);
    deref_null();
    // Unreachable on a correctly mapped SIGSEGV → SIG_DFL termination.
    LIBC_NAMESPACE::_exit(96);
  }

  // ----- Parent -----
  int status = 0;
  pid_t reaped = LIBC_NAMESPACE::waitpid(pid, &status, 0);
  LIBC_NAMESPACE::sigaction(SIGSEGV, &old_sa, nullptr);

  ASSERT_EQ(reaped, pid);
  // Must have been killed by a signal — NOT a normal exit.
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_FALSE(WIFEXITED(status));
  // The signal must be SIGSEGV. If the AV escaped the missing VEH, this
  // would surface as a different signal (or as WIFEXITED with one of
  // our 96/97 sentinels).
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);
}
