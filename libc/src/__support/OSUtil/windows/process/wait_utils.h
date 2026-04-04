//===-- Windows wait helpers -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_WAIT_UTILS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_WAIT_UTILS_H

#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/process/windows/child_table.h"

#include "hdr/signal_macros.h"
#include "hdr/sys_wait_macros.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

LIBC_INLINE int validate_waitid_options(int options) {
  constexpr int SUPPORTED_OPTIONS =
      WEXITED | WSTOPPED | WCONTINUED | WNOHANG | WNOWAIT;
  if ((options & ~SUPPORTED_OPTIONS) != 0)
    return EINVAL;
  if ((options & (WEXITED | WSTOPPED | WCONTINUED)) == 0)
    return EINVAL;
  return 0;
}

LIBC_INLINE int waitpid_options_to_waitid_options(int options,
                                                  int &waitid_options) {
  constexpr int SUPPORTED_OPTIONS = WNOHANG | WUNTRACED | WCONTINUED;
  if ((options & ~SUPPORTED_OPTIONS) != 0)
    return EINVAL;

  waitid_options = WEXITED;
  if (options & WNOHANG)
    waitid_options |= WNOHANG;
  if (options & WUNTRACED)
    waitid_options |= WSTOPPED;
  if (options & WCONTINUED)
    waitid_options |= WCONTINUED;
  return 0;
}

LIBC_INLINE int waitpid_pid_to_waitid_selector(pid_t pid, idtype_t &idtype,
                                               id_t &id) {
  if (pid > 0) {
    idtype = P_PID;
    id = static_cast<id_t>(pid);
    return 0;
  }
  if (pid == -1) {
    idtype = P_ALL;
    id = 0;
    return 0;
  }

  // waitpid(0, ...): wait for any child in the caller's process group.
  if (pid == 0) {
    idtype = P_PGID;
    id = static_cast<id_t>(process::get_self_pgid());
    return 0;
  }

  // waitpid(-pgid, ...): wait for any child in the specified group.
  // pid is negative, negate to get the pgid.
  idtype = P_PGID;
  id = static_cast<id_t>(-pid);
  return 0;
}

LIBC_INLINE int siginfo_to_waitstatus(const siginfo_t &info) {
  switch (info.si_code) {
  case CLD_EXITED:
    return W_EXITCODE(info.si_status, 0);
  case CLD_DUMPED:
    return info.si_status | WCOREFLAG;
  case CLD_KILLED:
    return info.si_status;
  case CLD_TRAPPED:
  case CLD_STOPPED:
    return W_STOPCODE(info.si_status);
  case CLD_CONTINUED:
    return 0xFFFF;
  default:
    return 0;
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_WAIT_UTILS_H
