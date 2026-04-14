//===-- Definition of semaphore macros ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_SEMAPHORE_MACROS_H
#define LLVM_LIBC_MACROS_SEMAPHORE_MACROS_H

// Maximum value a semaphore may have (at least 32767 per POSIX).
#define SEM_VALUE_MAX 2147483647

// Returned by sem_open on failure.
#define SEM_FAILED ((sem_t *)0)

#endif // LLVM_LIBC_MACROS_SEMAPHORE_MACROS_H
