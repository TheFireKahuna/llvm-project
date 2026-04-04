//===---------- Windows implementation of the POSIX waitpid function ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::waitpid() in wait_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/wait/waitpid.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/waitpid.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(pid_t, waitpid, (pid_t pid, int *wstatus, int options)) {
  auto result = windows_syscalls::waitpid(pid, wstatus, options);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
