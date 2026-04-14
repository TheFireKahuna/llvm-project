//===-- Implementation header for cfmakeraw ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_TERMIOS_CFMAKERAW_H
#define LLVM_LIBC_SRC_TERMIOS_CFMAKERAW_H

#include "src/__support/macros/config.h"

struct termios;

namespace LIBC_NAMESPACE_DECL {

void cfmakeraw(struct termios *t);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_TERMIOS_CFMAKERAW_H
