//===-- Windows implementation of getppid ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/getppid.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getppid.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(pid_t, getppid, ()) {
  // getppid cannot fail per POSIX. On NT query failure, the wrapper
  // returns Error(EIO); we return 0 to match the prior behavior.
  auto result = windows_syscalls::getppid();
  return result.has_value() ? result.value() : 0;
}

} // namespace LIBC_NAMESPACE_DECL
