//===-- Implementation of the pthread_barrierattr_setpshared -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_barrierattr_setpshared.h"

#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_barrierattr_setpshared,
                   (pthread_barrierattr_t * attr, int pshared)) {
  if (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED)
    return EINVAL;

  attr->pshared = (pshared == PTHREAD_PROCESS_SHARED);
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
