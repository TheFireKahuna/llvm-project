//===-- Definition of sem_t type ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES_SEM_T_H
#define LLVM_LIBC_TYPES_SEM_T_H

#include "__futex_word.h"

// Internal layout (all platforms):
//   byte 0: kind (0 = unnamed, 1 = named)
//   Unnamed: Futex (count) + padding.
//   Named:   platform handle (HANDLE on Windows, int fd on Linux).
//
// Windows: Futex is 64-bit → 1 (kind) + 7 (pad) + 8 (futex) = 16 bytes.
//          Named: 1 (kind) + 7 (pad) + 8 (HANDLE) = 16 bytes.
// Linux:   Futex is 32-bit → 1 (kind) + 3 (pad) + 4 (futex) = 8 bytes.
//          Named: 1 (kind) + 3 (pad) + 4 (fd) = 8 bytes.
#if defined(_WIN32) || defined(_WIN32_ITANIUM)
typedef struct {
  _Alignas(8) unsigned char __data[16];
} sem_t;
#else
typedef struct {
  _Alignas(4) unsigned char __data[8];
} sem_t;
#endif

#endif // LLVM_LIBC_TYPES_SEM_T_H
