//===-- Windows implementation of cfgetispeed ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/termios/cfgetispeed.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include <termios.h>

namespace LIBC_NAMESPACE_DECL {

// Returns the input baud rate from c_cflag. When CIBAUD bits are zero, the
// input speed equals the output speed (POSIX convention for unified rates).
LLVM_LIBC_FUNCTION(speed_t, cfgetispeed, (const struct termios *t)) {
  if (!t)
    return 0;
  speed_t ispeed = static_cast<speed_t>((t->c_cflag & CIBAUD) >> IBSHIFT);
  if (ispeed == 0)
    return static_cast<speed_t>(t->c_cflag & CBAUD);
  return ispeed;
}

} // namespace LIBC_NAMESPACE_DECL
