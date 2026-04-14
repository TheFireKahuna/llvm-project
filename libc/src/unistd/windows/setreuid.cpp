//===-- Implementation of setreuid for Windows ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// STUB — not yet implemented. See syscall_wrappers/setreuid.h for plan.
//
//===----------------------------------------------------------------------===//

#include "src/unistd/setreuid.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/setreuid.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, setreuid, (uid_t ruid, uid_t euid)) {
  auto result = windows_syscalls::setreuid(ruid, euid);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
