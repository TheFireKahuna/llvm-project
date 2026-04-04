//===-- POSIX syscall return conversion --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Converts a raw syscall result (value or -errno in a long) to the POSIX
// libc ABI (set libc_errno, return -1 on error). Used by POSIX entry points
// on all platforms to eliminate repeated boilerplate.
//
// Usage:
//   return syscall_return(syscall_impl(SYS_close, fd));
//   return syscall_return<off_t>(syscall_impl(SYS_lseek, fd, offset, whence));
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_SYSCALL_RETURN_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_SYSCALL_RETURN_H

#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

/// Convert a raw syscall return value to the POSIX libc convention.
/// On success (ret >= 0): returns the value cast to R.
/// On failure (ret < 0): sets libc_errno to the error code, returns -1.
template <typename R = int> LIBC_INLINE constexpr R syscall_return(long ret) {
  if (ret < 0) {
    libc_errno = static_cast<int>(-ret);
    return static_cast<R>(-1);
  }
  return static_cast<R>(ret);
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_SYSCALL_RETURN_H
