//===-- Windows implementation of getcwd ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/getcwd.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/getcwd.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(char *, getcwd, (char *buf, size_t size)) {
  auto result = windows_syscalls::getcwd(buf, size);
  if (!result.has_value()) {
    libc_errno = result.error();
    return nullptr;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
