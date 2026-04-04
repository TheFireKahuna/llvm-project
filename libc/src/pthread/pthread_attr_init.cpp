//===-- Implementation of the pthread_attr_init ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_attr_init.h"

#include "hdr/sched_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/default_attr.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_attr_init, (pthread_attr_t * attr)) {
  // Stacksize and guardsize come from the process-wide defaults so that
  // pthread_setattr_default_np mutations are honored on subsequent attr
  // initializations. Other fields stay at the per-attr defaults — only
  // stacksize/guardsize are exposed through the default-attr API.
  *attr = pthread_attr_t{
      PTHREAD_CREATE_JOINABLE,             // Not detached
      nullptr,                             // Let the thread manage its stack
      internal::get_default_stacksize(),
      internal::get_default_guardsize(),
      SCHED_OTHER,                         // Default scheduling policy.
      {0},                                 // Default sched_param (priority 0).
      PTHREAD_INHERIT_SCHED,               // Inherit from creating thread.
  };
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
