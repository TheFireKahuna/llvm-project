//===-- Definition of struct sockaddr_storage -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES_STRUCT_SOCKADDR_STORAGE_H
#define LLVM_LIBC_TYPES_STRUCT_SOCKADDR_STORAGE_H

#include "sa_family_t.h"

// POSIX requires sockaddr_storage to be large enough to hold any
// protocol-specific address structure, and to be aligned suitably.
// 128 bytes with 8-byte alignment matches Linux.
struct sockaddr_storage {
  sa_family_t ss_family;
  char __ss_padding[128 - sizeof(sa_family_t) - sizeof(unsigned long long)];
  unsigned long long __ss_align;
};

#endif // LLVM_LIBC_TYPES_STRUCT_SOCKADDR_STORAGE_H
