//===-- windows_syscalls::msync() wrapper -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_MSYNC_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_MSYNC_H

#include "hdr/stdint_proxy.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include "src/__support/OSUtil/windows/memory/posix/msync.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

LIBC_INLINE ErrorOr<int> msync(void *addr, size_t length, int flags) {
  intptr_t ret = internal::msync(addr, length, flags);
  if (ret < 0)
    return Error(-static_cast<int>(ret));
  return 0;
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_MSYNC_H
