//===-- windows_syscalls::execveat() wrapper ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_EXECVEAT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_EXECVEAT_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/process/exec_ops.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

// Linux execveat semantics: returns Error on failure, does not return on
// success. The ErrorOr<int> shape matches the rest of the wrapper layer
// for namespace consistency; on success the value is unreachable.
LIBC_INLINE ErrorOr<int> execveat(int dirfd, const char *pathname,
                                  char *const argv[], char *const envp[],
                                  int flags) {
  intptr_t ret = internal::execveat(dirfd, pathname, argv, envp, flags);
  // Reachable only on failure.
  return Error(-static_cast<int>(ret));
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_EXECVEAT_H
