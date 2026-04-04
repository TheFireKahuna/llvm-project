//===---------- Windows implementation of the POSIX wait4 function --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::wait4() in wait_ops.cpp.
// wait4 is implemented on top of the Windows waitid core so resource usage can
// come from the waited child instead of cumulative RUSAGE_CHILDREN.
//
//===----------------------------------------------------------------------===//

#include "src/sys/wait/wait4.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/wait4.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(pid_t, wait4,
                   (pid_t pid, int *wstatus, int options,
                    struct rusage *usage)) {
  auto result = windows_syscalls::wait4(pid, wstatus, options, usage);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
