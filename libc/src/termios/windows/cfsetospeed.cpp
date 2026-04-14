//===-- Windows implementation of cfsetospeed ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/termios/cfsetospeed.h"

#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#include <termios.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, cfsetospeed, (struct termios * t, speed_t speed)) {
  constexpr speed_t NOT_SPEED_MASK = ~speed_t(CBAUD);
  if (!t || ((speed & NOT_SPEED_MASK) != 0)) {
    libc_errno = EINVAL;
    return -1;
  }

  t->c_cflag = static_cast<tcflag_t>((t->c_cflag & ~tcflag_t(CBAUD)) | speed);
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
