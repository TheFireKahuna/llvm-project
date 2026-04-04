//===-- Implementation header for posix_openpt -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_FCNTL_POSIX_OPENPT_H
#define LLVM_LIBC_SRC_FCNTL_POSIX_OPENPT_H

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

int posix_openpt(int flags);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_FCNTL_POSIX_OPENPT_H
