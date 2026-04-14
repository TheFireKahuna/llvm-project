//===-- Implementation header for __pthread_cleanup_pop ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_PTHREAD___PTHREAD_CLEANUP_POP_H
#define LLVM_LIBC_SRC_PTHREAD___PTHREAD_CLEANUP_POP_H

#include "src/__support/macros/config.h"
#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

void __pthread_cleanup_pop(__pthread_cleanup_t *frame, int execute);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_PTHREAD___PTHREAD_CLEANUP_POP_H
