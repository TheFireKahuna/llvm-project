//===-- Definition of fpos_t type -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES_FPOS_T_H
#define LLVM_LIBC_TYPES_FPOS_T_H

// C11 §7.21.9.1 requires fpos_t to hold both the file position and the
// wide-stream parse state so that fgetpos/fsetpos can restore both. The
// layout mirrors glibc's size (16 bytes on 64-bit), enabling a future
// internal mbstate growth up to 8 bytes without an ABI break.
//
// __pos and __state are treated as opaque by callers; only fgetpos and
// fsetpos may read or write them.
typedef struct {
  __INT64_TYPE__ __pos;
  unsigned char __state[8];
} fpos_t;

#endif // LLVM_LIBC_TYPES_FPOS_T_H
