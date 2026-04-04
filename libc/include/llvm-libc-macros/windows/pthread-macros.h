//===-- Windows pthread macros ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_WINDOWS_PTHREAD_MACROS_H
#define LLVM_LIBC_MACROS_WINDOWS_PTHREAD_MACROS_H

// Windows mutex type uses opaque 40-byte storage (see __mutex_type.h).
#define PTHREAD_MUTEX_INITIALIZER                                              \
  { {0} }

// Windows cond type uses opaque 32-byte storage.
#define PTHREAD_COND_INITIALIZER                                               \
  { {0} }

#endif // LLVM_LIBC_MACROS_WINDOWS_PTHREAD_MACROS_H
