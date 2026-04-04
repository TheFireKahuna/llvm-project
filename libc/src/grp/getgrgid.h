//===-- Implementation header for getgrgid -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_GRP_GETGRGID_H
#define LLVM_LIBC_SRC_GRP_GETGRGID_H

#include "hdr/types/gid_t.h"
#include "src/__support/macros/config.h"

struct group;

namespace LIBC_NAMESPACE_DECL {

struct group *getgrgid(gid_t gid);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_GRP_GETGRGID_H
