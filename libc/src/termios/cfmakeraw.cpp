//===-- Implementation of cfmakeraw --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/termios/cfmakeraw.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include <termios.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void, cfmakeraw, (struct termios * t)) {
  if (!t)
    return;

  t->c_iflag &= static_cast<tcflag_t>(
      ~(BRKINT | ICRNL | IGNCR | INLCR | ISTRIP | IXON | PARMRK));
  t->c_oflag &= static_cast<tcflag_t>(~OPOST);
  t->c_lflag &=
      static_cast<tcflag_t>(~(ECHO | ECHONL | ICANON | IEXTEN | ISIG));
  t->c_cflag &= static_cast<tcflag_t>(~(CSIZE | PARENB));
  t->c_cflag |= CS8;
  t->c_cc[VMIN] = 1;
  t->c_cc[VTIME] = 0;
}

} // namespace LIBC_NAMESPACE_DECL
