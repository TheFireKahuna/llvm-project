//===-- Windows implementation of isatty ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/isatty.h"
#include "src/__support/OSUtil/windows/process/terminal_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, isatty, (int fd)) {
  int ret = internal::terminal_ops::is_terminal_fd(fd);
  if (ret < 0) {
    libc_errno = -ret;
    return 0;
  }
  return 1;
}

} // namespace LIBC_NAMESPACE_DECL
