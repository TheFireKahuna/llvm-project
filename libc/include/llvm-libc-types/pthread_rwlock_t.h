//===-- Definition of pthread_rwlock_t type -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES_PTHREAD_RWLOCK_T_H
#define LLVM_LIBC_TYPES_PTHREAD_RWLOCK_T_H

#if defined(__NTPOSIX__)
// NTPOSIX keeps pthread_rwlock_t as opaque storage.
//
// The internal Windows RwLock currently composes:
//   - RawRwLock:
//       - 2 one-bit policy fields packed into an unsigned storage unit
//       - Atomic<int> state
//       - WaitingQueue:
//           - RawMutex (8-byte Futex)
//           - 2 pending-count words
//           - 2 serialization Futex objects
//   - Atomic<pid_t> writer_tid
//
// That lands at 48 bytes on x86-64 with 8-byte alignment
// (Futex contains Atomic<uint64_t> for CAS-64).
typedef struct {
  _Alignas(8) unsigned char __data[48];
} pthread_rwlock_t;

#else

#include "__futex_word.h"
#include "pid_t.h"
typedef struct {
  struct {
    unsigned __is_pshared : 1;
    unsigned __preference : 1;
    int __state;
    __futex_word __wait_queue_mutex;
    __futex_word __pending_readers;
    __futex_word __pending_writers;
    __futex_word __reader_serialization;
    __futex_word __writer_serialization;
  } __raw;
  pid_t __writer_tid;
} pthread_rwlock_t;

#endif

#endif // LLVM_LIBC_TYPES_PTHREAD_RWLOCK_T_H
