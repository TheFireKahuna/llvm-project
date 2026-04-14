//===-- Implementation of the pthread_barrierattr_getpshared -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_barrierattr_getpshared.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_barrierattr_getpshared,
                   (const pthread_barrierattr_t *__restrict attr,
                    int *__restrict pshared)) {
  *pshared =
      attr->pshared ? PTHREAD_PROCESS_SHARED : PTHREAD_PROCESS_PRIVATE;
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
