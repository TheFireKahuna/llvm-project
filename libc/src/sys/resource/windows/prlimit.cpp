//===-- Windows implementation of prlimit ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::prlimit() in resource_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/resource/prlimit.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/prlimit.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, prlimit,
                   (int pid, int resource, const struct rlimit *new_limit,
                    struct rlimit *old_limit)) {
  auto result = windows_syscalls::prlimit(pid, resource, new_limit, old_limit);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
