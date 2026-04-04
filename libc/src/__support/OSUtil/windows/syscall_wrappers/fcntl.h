//===-- windows_syscalls::fcntl() wrapper ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_FCNTL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_FCNTL_H

#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

namespace internal {
// Already returns ErrorOr<int> — declared in __support/OSUtil/fcntl.h.
ErrorOr<int> fcntl(int fd, int cmd, void *arg);
} // namespace internal

namespace windows_syscalls {

// fcntl's internal:: already returns ErrorOr<int>, so this is a direct
// passthrough. Exists for namespace consistency with other wrappers.
LIBC_INLINE ErrorOr<int> fcntl(int fd, int cmd, void *arg) {
  return internal::fcntl(fd, cmd, arg);
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_FCNTL_H
