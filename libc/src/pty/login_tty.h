//===-- Implementation header for login_tty --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_PTY_LOGIN_TTY_H
#define LLVM_LIBC_SRC_PTY_LOGIN_TTY_H

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

int login_tty(int fd);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_PTY_LOGIN_TTY_H
