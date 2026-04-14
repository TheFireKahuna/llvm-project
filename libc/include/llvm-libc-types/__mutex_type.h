//===-- Definition of a common mutex type ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES___MUTEX_TYPE_H
#define LLVM_LIBC_TYPES___MUTEX_TYPE_H

#include "__futex_word.h"

#if defined(__NTPOSIX__)
// NTPOSIX keeps pthread_mutex_t / mtx_t as opaque storage.
//
// The internal Windows Mutex currently composes:
//   - 5 one-byte policy flags
//   - 3 bytes of padding to re-align atomics
//   - Atomic<pid_t>
//   - Atomic<unsigned int>
//   - RobustRecord *
//   - RawMutex (8-byte Futex)
//
// That lands at 32 bytes on x86-64 with 8-byte alignment.
typedef struct {
  _Alignas(8) unsigned char __data[32];
} __mutex_type;

#else

typedef struct {
  unsigned char __timed;
  unsigned char __type;
  unsigned char __robust;

  void *__owner;
  unsigned long long __lock_count;

#ifdef __linux__
  __futex_word __ftxw;
#elif defined(__APPLE__)
  __futex_word __ftxw;
#else
#error "Mutex type not defined for the target platform."
#endif
} __mutex_type;
#endif

#endif // LLVM_LIBC_TYPES___MUTEX_TYPE_H
