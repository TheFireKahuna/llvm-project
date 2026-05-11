//===-- windows_syscalls::brk()/sbrk() wrappers ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_BRK_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_BRK_H

#include "hdr/errno_macros.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include "src/__support/OSUtil/windows/memory/legacy/brk_state.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

// POSIX brk(2): set program break to addr.
// Returns 0 on success, Error(ENOMEM) on failure.
LIBC_INLINE ErrorOr<int> brk(void *addr) {
  intptr_t result = internal::sys_brk(addr);
  // Linux brk returns the current break on failure (unchanged).
  // POSIX brk returns 0 on success, -1 on failure.
  if (result != reinterpret_cast<intptr_t>(addr))
    return Error(ENOMEM);
  return 0;
}

// POSIX sbrk(2): adjust program break by increment.
// Returns the previous break on success, Error(ENOMEM) on failure.
LIBC_INLINE ErrorOr<void *> sbrk(intptr_t increment) {
  intptr_t old_brk = internal::sys_brk(nullptr);
  if (old_brk == 0)
    return Error(ENOMEM); // brk subsystem not initialized.

  if (increment == 0)
    return reinterpret_cast<void *>(old_brk);

  // Compute target address. intptr_t addition handles both positive and
  // negative increments correctly (pointer arithmetic on the break address).
  auto *target = reinterpret_cast<void *>(old_brk + increment);
  intptr_t new_brk = internal::sys_brk(target);

  if (new_brk != reinterpret_cast<intptr_t>(target))
    return Error(ENOMEM);

  return reinterpret_cast<void *>(old_brk);
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_BRK_H
