//===-- Implementation header for getgrnam_r ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_GRP_GETGRNAM_R_H
#define LLVM_LIBC_SRC_GRP_GETGRNAM_R_H

#include "hdr/types/size_t.h"
#include "src/__support/macros/config.h"

struct group;

namespace LIBC_NAMESPACE_DECL {

int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen,
               struct group **result);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_GRP_GETGRNAM_R_H
