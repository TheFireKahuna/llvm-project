//===-- Windows implementation of tcsendbreak ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/termios/tcsendbreak.h"

#include "src/__support/OSUtil/windows/process/terminal_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// POSIX.1-2017: "If the terminal is not using asynchronous serial data
// transmission, tcsendbreak() shall return without taking any action."
// The duration parameter is implementation-defined when non-zero; we ignore
// it unconditionally — ConDrv/ConPTY have no physical serial line.
LLVM_LIBC_FUNCTION(int, tcsendbreak, (int fd, int)) {
  int ret = internal::terminal_ops::send_break(fd, 0);
  if (ret < 0) {
    libc_errno = -ret;
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
