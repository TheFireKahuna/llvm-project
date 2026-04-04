//===---------- Windows implementation of the POSIX lstat function --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::lstat() in stat_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/stat/lstat.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/lstat.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, lstat,
                   (const char *__restrict path,
                    struct stat *__restrict statbuf)) {
  auto result = windows_syscalls::lstat(path, statbuf);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
