//===-- Implementation of pthread_setschedprio ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_setschedprio.h"

#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/sched_support.h"
#include "src/__support/threads/thread.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_setschedprio, (pthread_t th, int prio)) {
  auto *thread = reinterpret_cast<Thread *>(&th);
  auto *attrib = thread->attrib;
  if (!attrib || attrib->tid <= 0)
    return ESRCH;

  return sched_support::set_thread_prio(attrib, prio);
}

} // namespace LIBC_NAMESPACE_DECL
