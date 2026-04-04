//===-- Implementation of pthread_clockjoin_np ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_clockjoin_np.h"

#include "hdr/types/clockid_t.h"
#include "hdr/types/struct_timespec.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/thread.h"
#include "src/pthread/cancel_internal.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

static_assert(sizeof(pthread_t) == sizeof(LIBC_NAMESPACE::Thread),
              "Mismatch between pthread_t and internal Thread.");

// Glibc semantics: caller picks the clock domain (CLOCK_REALTIME or
// CLOCK_MONOTONIC); abstime is absolute in that domain. POSIX
// cancellation point.
LLVM_LIBC_FUNCTION(int, pthread_clockjoin_np,
                   (pthread_t th, void **retval, clockid_t clockid,
                    const struct timespec *abstime)) {
  cancel_check();
  auto *thread = reinterpret_cast<Thread *>(&th);
  return thread->timed_join(retval, clockid, abstime);
}

} // namespace LIBC_NAMESPACE_DECL
