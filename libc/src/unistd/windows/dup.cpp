//===-- Windows implementation of dup -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/dup.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/dup.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, dup, (int fd)) {
  // Direct route through internal::dup → fd_table.dup(fd, 0, 0), skipping
  // the prior fcntl(F_DUPFD) virtual-op indirection.
  auto result = windows_syscalls::dup(fd);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
