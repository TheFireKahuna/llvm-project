//===---------- Windows implementation of the POSIX fstatat function ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::fstatat() in stat_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/stat/fstatat.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/fstatat.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, fstatat,
                   (int dfd, const char *__restrict path,
                    struct stat *__restrict statbuf, int flags)) {
  auto result = windows_syscalls::fstatat(dfd, path, statbuf, flags);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
