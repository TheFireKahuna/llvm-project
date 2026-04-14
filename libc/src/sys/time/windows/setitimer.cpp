//===-- Windows implementation of setitimer --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::setitimer() in itimer_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/time/setitimer.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/setitimer.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, setitimer,
                   (int which, const struct itimerval *new_value,
                    struct itimerval *old_value)) {
  auto result = windows_syscalls::setitimer(which, new_value, old_value);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
