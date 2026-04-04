//===-- windows_syscalls::pread() wrapper ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_PREAD_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_PREAD_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/off_t.h"
#include "hdr/types/ssize_t.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include <stddef.h>

#include "src/__support/OSUtil/windows/io/file_ops.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

LIBC_INLINE ErrorOr<ssize_t> pread(int fd, void *buf, size_t count,
                                   off_t offset) {
  intptr_t ret = internal::pread(fd, buf, count, offset);
  if (ret < 0)
    return Error(-static_cast<int>(ret));
  return static_cast<ssize_t>(ret);
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_PREAD_H
