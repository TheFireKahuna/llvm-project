//===-- Definition of __barrier_type type ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES__BARRIER_TYPE_H
#define LLVM_LIBC_TYPES__BARRIER_TYPE_H

// Internal layout: unsigned expected + Futex sense + Atomic<unsigned> count.
// Futex is 32-bit on Linux/Darwin (12 bytes total) and 64-bit on Windows
// (24 bytes with alignment padding).
//
// Linux/Darwin keep the historical 80-byte size to avoid an ABI change.
// Windows (NTPOSIX) uses the tighter 24-byte layout since it has no prior ABI.
#if defined(__NTPOSIX__)
typedef struct {
  _Alignas(8) unsigned char __data[24];
} __barrier_type;
#else
typedef struct {
  unsigned char __data[80];
} __barrier_type;
#endif

#endif // LLVM_LIBC_TYPES__BARRIER_TYPE_H
