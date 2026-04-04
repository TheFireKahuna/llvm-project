//===-- Windows implementation of execveat ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::execveat() in exec_ops.cpp.
// Linux semantics: dirfd-relative exec with AT_EMPTY_PATH and
// AT_SYMLINK_NOFOLLOW flag support.
//
//===----------------------------------------------------------------------===//

#include "src/unistd/execveat.h"

#include "src/__support/OSUtil/windows/process/exec_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#include "hdr/stdint_proxy.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, execveat,
                   (int dirfd, const char *pathname, char *const argv[],
                    char *const envp[], int flags)) {
  intptr_t ret = internal::execveat(dirfd, pathname, argv, envp, flags);
  // execveat only returns on error.
  libc_errno = static_cast<int>(-ret);
  return -1;
}

} // namespace LIBC_NAMESPACE_DECL
