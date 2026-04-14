//===-- Windows implementation of cfsetispeed ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/termios/cfsetispeed.h"

#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#include <termios.h>

namespace LIBC_NAMESPACE_DECL {

// Sets the input baud rate in the CIBAUD bits of c_cflag. A speed of B0 clears
// the CIBAUD field, making input speed track the output speed (POSIX unified
// rate convention). Otherwise the rate is stored shifted into the CIBAUD
// position independently of the output speed.
LLVM_LIBC_FUNCTION(int, cfsetispeed, (struct termios * t, speed_t speed)) {
  constexpr speed_t VALID_SPEED_MASK = speed_t(CBAUD);
  if (!t || (speed & ~VALID_SPEED_MASK) != 0) {
    libc_errno = EINVAL;
    return -1;
  }

  // B0 means "same as output" — clear CIBAUD entirely.
  tcflag_t shifted = static_cast<tcflag_t>(speed) << IBSHIFT;
  t->c_cflag = static_cast<tcflag_t>((t->c_cflag & ~tcflag_t(CIBAUD)) | shifted);
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
