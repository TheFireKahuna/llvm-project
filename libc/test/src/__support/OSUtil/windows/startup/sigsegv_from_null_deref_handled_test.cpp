//===-- SIGSEGV from null deref end-to-end test --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// End-to-end verification that the master VEH translates a hardware
// access-violation at address 0 into a POSIX SIGSEGV delivery, with siginfo
// populated correctly, and that the default disposition (no handler installed)
// terminates the process by signal.
//
// Two cases:
//
//   1. HandlerInstalledRecoversViaLongjmp
//        - sigaction(SIGSEGV, SA_SIGINFO, ...) installs a handler that
//          records siginfo and longjmps out.
//        - Trigger a null deref: `*(volatile int *)0 = 0xCAFE;`
//        - Assert the handler ran (volatile flag set).
//        - Assert siginfo.si_signo == SIGSEGV.
//        - Assert siginfo.si_code is the correct synchronous-fault code
//          for an access violation (SEGV_ACCERR per build_si_code in
//          signal/transport/veh_transport.cpp; SEGV_MAPERR is also accepted
//          per the POSIX spec).
//        - After longjmp out, run a benign POSIX op (write+read on a pipe)
//          to prove the process is still functional.
//
//   2. NoHandlerProcessIsKilledBySigsegv
//        - Fork a child that performs a null deref with no SIGSEGV handler
//          installed.
//        - Parent waitpids and asserts WIFSIGNALED + WTERMSIG == SIGSEGV.
//        - With no custom handler installed, signal_veh_transport returns
//          EXCEPTION_CONTINUE_SEARCH, the OS unhandled-exception path runs,
//          and the process terminates with the appropriate signal.
//
// This is a cdll test: it links c.dll and uses only the public POSIX
// surface (<signal.h>, <setjmp.h>, <sys/wait.h>, <unistd.h>). It does
// not poke any internal libc symbols.
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
#include "src/unistd/close.h"
#include "src/unistd/fork.h"
#include "src/unistd/pipe.h"
#include "src/unistd/read.h"
#include "src/unistd/write.h"

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Recovery state for the SIGSEGV handler. All fields are volatile or
// trivially copyable; the handler writes them, the test thread reads them
// after longjmp returns from siglongjmp's nonzero arm.
volatile sig_atomic_t g_handler_ran = 0;
volatile int g_captured_signo = 0;
volatile int g_captured_code = 0;
volatile void *g_captured_addr = nullptr;
sigjmp_buf g_recover_jmp;

// SA_SIGINFO handler. Records the signal number, si_code and si_addr,
// then longjmps back to the test body. Using siglongjmp + sigsetjmp so
// the signal mask is restored cleanly on return.
void segv_recover_handler(int signo, siginfo_t *info, void * /*uctx*/) {
  g_handler_ran = 1;
  g_captured_signo = signo;
  if (info) {
    g_captured_code = info->si_code;
    g_captured_addr = info->si_addr;
  }
  LIBC_NAMESPACE::siglongjmp(g_recover_jmp, 1);
}

// Force a null-pointer write. `volatile` prevents the compiler from
// folding away the dead store. Returning the loaded value through a sink
// also prevents the load form being elided (used by the no-handler path).
[[gnu::noinline]] void do_null_write() {
  *reinterpret_cast<volatile int *>(0) = 0xCAFE;
}

} // namespace

// ===----------------------------------------------------------------------===
// Case 1: handler installed → SIGSEGV delivered, siginfo populated, recover.
// ===----------------------------------------------------------------------===

TEST(LlvmLibcSigsegvFromNullDeref, HandlerInstalledRecoversViaLongjmp) {
  // Install SA_SIGINFO handler for SIGSEGV.
  struct sigaction sa = {};
  sa.sa_sigaction = segv_recover_handler;
  sa.sa_flags = SA_SIGINFO;
  LIBC_NAMESPACE::sigemptyset(&sa.sa_mask);

  struct sigaction old = {};
  ASSERT_EQ(LIBC_NAMESPACE::sigaction(SIGSEGV, &sa, &old), 0);

  g_handler_ran = 0;
  g_captured_signo = 0;
  g_captured_code = 0;
  g_captured_addr = reinterpret_cast<void *>(static_cast<uintptr_t>(0xDEAD));

  // sigsetjmp: zero on the initial call, nonzero after siglongjmp.
  if (sigsetjmp(g_recover_jmp, 1) == 0) {
    // First pass: trigger the fault. Should not return normally.
    do_null_write();
    // If we get here, the master VEH did NOT route to our handler.
    ASSERT_TRUE(false);
  }
  // Second pass: arrived here via siglongjmp from segv_recover_handler.

  // 1. Handler ran.
  EXPECT_EQ(static_cast<int>(g_handler_ran), 1);

  // 2. siginfo fields:
  //    - si_signo == SIGSEGV
  //    - si_code is the access-violation synchronous-fault code. The
  //      current signal_veh_transport build_si_code() returns SEGV_ACCERR
  //      for EXCEPTION_ACCESS_VIOLATION (and SEGV_MAPERR only for
  //      EXCEPTION_STACK_OVERFLOW). POSIX permits either for an
  //      unmapped-page fault, so accept both.
  EXPECT_EQ(g_captured_signo, SIGSEGV);
  const int code = g_captured_code;
  EXPECT_TRUE(code == SEGV_ACCERR || code == SEGV_MAPERR);

  // Restore previous disposition before continuing — the process must
  // remain usable for the post-recovery sanity check.
  ASSERT_EQ(LIBC_NAMESPACE::sigaction(SIGSEGV, &old, nullptr), 0);

  // 3. Process is still usable: round-trip a small payload through a
  //    pipe. This exercises file descriptor allocation, the I/O engine,
  //    and the per-thread signal state — all of which would be wedged
  //    if the SIGSEGV handling had left the process in a broken state.
  int pipefd[2] = {-1, -1};
  ASSERT_EQ(LIBC_NAMESPACE::pipe(pipefd), 0);

  const char msg[] = "post-segv-ok";
  const ssize_t msg_len = static_cast<ssize_t>(sizeof(msg));
  ASSERT_EQ(LIBC_NAMESPACE::write(pipefd[1], msg, sizeof(msg)), msg_len);

  char buf[sizeof(msg)] = {};
  ASSERT_EQ(LIBC_NAMESPACE::read(pipefd[0], buf, sizeof(buf)), msg_len);
  EXPECT_EQ(memcmp(buf, msg, sizeof(msg)), 0);

  LIBC_NAMESPACE::close(pipefd[0]);
  LIBC_NAMESPACE::close(pipefd[1]);
}

// ===----------------------------------------------------------------------===
// Case 2: no handler installed → process terminates by SIGSEGV signal.
// ===----------------------------------------------------------------------===

TEST(LlvmLibcSigsegvFromNullDeref, NoHandlerProcessIsKilledBySigsegv) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);

  if (pid == 0) {
    // Child: ensure SIGSEGV disposition is the default. Parent did not
    // install one (it was restored at the end of case 1), but be explicit
    // — a stray inherited handler would mask the regression we're testing.
    struct sigaction dfl = {};
    dfl.sa_handler = SIG_DFL;
    LIBC_NAMESPACE::sigemptyset(&dfl.sa_mask);
    LIBC_NAMESPACE::sigaction(SIGSEGV, &dfl, nullptr);

    // Trigger the fault. With no custom handler installed,
    // signal_veh_transport returns EXCEPTION_CONTINUE_SEARCH and the
    // OS-level unhandled-exception path terminates this process.
    do_null_write();

    // If we somehow survived the fault, exit with a sentinel so the
    // parent's WIFEXITED arm fires and the test fails loudly.
    LIBC_NAMESPACE::_exit(123);
  }

  int status = 0;
  ASSERT_GT(LIBC_NAMESPACE::waitpid(pid, &status, 0), 0);

  // The child must have died by signal (not exited normally with our
  // sentinel), and the signal must be SIGSEGV.
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_FALSE(WIFEXITED(status));
  if (WIFSIGNALED(status))
    EXPECT_EQ(WTERMSIG(status), SIGSEGV);
}
