//===-- Windows implementation of openpty -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/pty/openpty.h"

#include "src/__support/OSUtil/windows/process/vt_pty.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, openpty,
                   (int * master, int *slave, char *name,
                     const struct termios *termp,
                     const struct winsize *winp)) {
  auto result = internal::vt_pty::openpty(master, slave, name, termp, winp);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
