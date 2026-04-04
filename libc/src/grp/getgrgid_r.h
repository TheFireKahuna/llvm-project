//===-- Implementation header for getgrgid_r ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_GRP_GETGRGID_R_H
#define LLVM_LIBC_SRC_GRP_GETGRGID_R_H

#include "hdr/types/gid_t.h"
#include "hdr/types/size_t.h"
#include "src/__support/macros/config.h"

struct group;

namespace LIBC_NAMESPACE_DECL {

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen,
               struct group **result);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_GRP_GETGRGID_R_H
