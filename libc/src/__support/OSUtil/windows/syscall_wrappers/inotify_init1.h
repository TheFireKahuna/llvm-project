//===-- windows_syscalls::inotify_init1() wrapper ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_INOTIFY_INIT1_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_INOTIFY_INIT1_H

#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include "src/__support/OSUtil/windows/ipc/inotify_ops.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

LIBC_INLINE ErrorOr<int> inotify_init1(int flags) {
  intptr_t ret = internal::inotify_init1(flags);
  if (ret < 0)
    return Error(-static_cast<int>(ret));
  return static_cast<int>(ret);
}

LIBC_INLINE ErrorOr<int> inotify_init() { return inotify_init1(0); }

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_INOTIFY_INIT1_H
