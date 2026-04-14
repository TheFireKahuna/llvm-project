//===-- Implementation header for getpwnam_r ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_PWD_GETPWNAM_R_H
#define LLVM_LIBC_SRC_PWD_GETPWNAM_R_H

#include "hdr/types/size_t.h"
#include "src/__support/macros/config.h"

struct passwd;

namespace LIBC_NAMESPACE_DECL {

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_PWD_GETPWNAM_R_H
