//===-- Implementation of pthread_setschedparam ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_setschedparam.h"

#include "hdr/errno_macros.h"
#include "hdr/sched_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/sched_support.h"
#include "src/__support/threads/thread.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_setschedparam,
                   (pthread_t th, int policy,
                    const struct sched_param *param)) {
  if (!param)
    return EINVAL;
  if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR)
    return EINVAL;

  // Validate priority range for the requested policy.
  if (policy == SCHED_OTHER) {
    if (param->sched_priority != 0)
      return EINVAL;
  } else {
    if (param->sched_priority < 1 || param->sched_priority > 99)
      return EINVAL;
  }

  auto *thread = reinterpret_cast<Thread *>(&th);
  auto *attrib = thread->attrib;
  if (!attrib || attrib->tid <= 0)
    return ESRCH;

  return sched_support::set_thread_sched(attrib, policy, param);
}

} // namespace LIBC_NAMESPACE_DECL
