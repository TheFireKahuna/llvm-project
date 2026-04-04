//===-- Implementation of pthread_getcpuclockid ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_getcpuclockid.h"

#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/cpuclock_support.h"
#include "src/__support/threads/thread.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_getcpuclockid,
                   (pthread_t th, clockid_t *clock_id)) {
  if (!clock_id)
    return EINVAL;

  auto *thread = reinterpret_cast<Thread *>(&th);
  auto *attrib = thread->attrib;
  if (!attrib || attrib->tid <= 0)
    return ESRCH;

  return cpuclock_support::get_thread_cpuclockid(attrib->tid, clock_id);
}

} // namespace LIBC_NAMESPACE_DECL
