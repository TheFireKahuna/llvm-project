//===-- Definition of type struct group ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES_STRUCT_GROUP_H
#define LLVM_LIBC_TYPES_STRUCT_GROUP_H

#include "gid_t.h"

struct group {
  char *gr_name;
  char *gr_passwd;
  gid_t gr_gid;
  char **gr_mem;
};

#endif // LLVM_LIBC_TYPES_STRUCT_GROUP_H
