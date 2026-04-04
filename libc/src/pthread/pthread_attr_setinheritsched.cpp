//===-- Implementation of the pthread_attr_setinheritsched ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_attr_setinheritsched.h"

#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_attr_setinheritsched,
                   (pthread_attr_t *attr, int inheritsched)) {
  if (inheritsched != PTHREAD_INHERIT_SCHED &&
      inheritsched != PTHREAD_EXPLICIT_SCHED)
    return EINVAL;
  attr->__inheritsched = inheritsched;
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
