//===-- ExecuteFunction implementation for NT-POSIX ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Mirror of ExecuteFunctionUnix.cpp that calls LIBC_NAMESPACE:: directly.
// Under LIBC_UNIT_TEST_LINK_FREESTANDING the test link line carries no
// foreign libc, so the extern-C POSIX aliases used by the Unix file are
// unresolved (the __internal__ variants of the libc objects only emit
// C++-mangled symbols). Reuse the exact same fork/pipe/poll/waitpid
// protocol, just via the C++-mangled names.
//
//===----------------------------------------------------------------------===//

#include "ExecuteFunction.h"
#include "src/__support/macros/config.h"

#include "hdr/signal_macros.h"    // SIGKILL
#include "hdr/sys_wait_macros.h"  // WIFEXITED/WEXITSTATUS/WTERMSIG
#include "hdr/types/pid_t.h"
#include "hdr/types/struct_pollfd.h"
#include "src/poll/poll.h"
#include "src/signal/kill.h"
#include "src/stdlib/_Exit.h"
#include "src/string/strsignal.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/close.h"
#include "src/unistd/fork.h"
#include "src/unistd/pipe.h"

#ifndef POLLIN
#define POLLIN 0x0001
#endif
#ifndef POLLHUP
#define POLLHUP 0x0010
#endif

namespace LIBC_NAMESPACE_DECL {
namespace testutils {

bool ProcessStatus::exited_normally() { return WIFEXITED(platform_defined); }

int ProcessStatus::get_exit_code() {
  // Abnormal termination → no exit code; caller must guard via exited_normally.
  return WEXITSTATUS(platform_defined);
}

int ProcessStatus::get_fatal_signal() {
  if (exited_normally())
    return 0;
  return WTERMSIG(platform_defined);
}

ProcessStatus invoke_in_subprocess(FunctionCaller *func, int timeout_ms) {
  int pipe_fds[2];
  if (LIBC_NAMESPACE::pipe(pipe_fds) == -1) {
    delete func;
    return ProcessStatus::error("pipe(2) failed");
  }

  pid_t pid = LIBC_NAMESPACE::fork();
  if (pid == -1) {
    delete func;
    return ProcessStatus::error("fork(2) failed");
  }

  if (!pid) {
    (*func)();
    delete func;
    LIBC_NAMESPACE::_Exit(0);
  }
  LIBC_NAMESPACE::close(pipe_fds[1]);

  pollfd poll_fd{pipe_fds[0], POLLIN, 0};
  // No events requested — returns on timeout or when the child-side pipe end
  // is closed (i.e. the process has exited).
  if (LIBC_NAMESPACE::poll(&poll_fd, 1, timeout_ms) == -1) {
    delete func;
    return ProcessStatus::error("poll(2) failed");
  }
  if (!(poll_fd.revents & POLLHUP)) {
    LIBC_NAMESPACE::kill(pid, SIGKILL);
    delete func;
    return ProcessStatus::timed_out_ps();
  }

  int wstatus = 0;
  pid_t status = LIBC_NAMESPACE::waitpid(pid, &wstatus, 0);
  if (status == -1) {
    delete func;
    return ProcessStatus::error("waitpid(2) failed");
  }
  delete func;
  return {wstatus};
}

const char *signal_as_string(int signum) {
  return LIBC_NAMESPACE::strsignal(signum);
}

} // namespace testutils
} // namespace LIBC_NAMESPACE_DECL
