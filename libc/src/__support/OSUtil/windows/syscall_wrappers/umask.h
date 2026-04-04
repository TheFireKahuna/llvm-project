//===-- windows_syscalls::umask() wrapper ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_UMASK_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_UMASK_H

#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include "src/__support/OSUtil/windows/security/security.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

LIBC_INLINE ErrorOr<mode_t> umask(mode_t mask) {
  return windows_sec::set_umask(mask);
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_UMASK_H
