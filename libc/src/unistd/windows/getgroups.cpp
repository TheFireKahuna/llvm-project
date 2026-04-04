//===-- Implementation of getgroups for Windows -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// STUB — not yet implemented. See syscall_wrappers/getgroups.h for plan.
//
//===----------------------------------------------------------------------===//

#include "src/unistd/getgroups.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getgroups.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, getgroups, (int size, gid_t list[])) {
  auto result = windows_syscalls::getgroups(size, list);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
