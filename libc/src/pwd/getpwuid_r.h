//===-- Implementation header for getpwuid_r ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_PWD_GETPWUID_R_H
#define LLVM_LIBC_SRC_PWD_GETPWUID_R_H

#include "hdr/types/size_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/macros/config.h"

struct passwd;

namespace LIBC_NAMESPACE_DECL {

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_PWD_GETPWUID_R_H
