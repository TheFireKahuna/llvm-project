//===-- Implementation of pthread_cond_timedwait --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_cond_timedwait.h"

#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/pthread/cancel_internal.h"
#include "src/__support/threads/CndVar.h"
#include "src/__support/threads/mutex.h"
#include "src/__support/time/abs_timeout.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

static_assert(sizeof(CndVar) <= sizeof(pthread_cond_t),
              "The public pthread_cond_t type cannot accommodate the internal "
              "CndVar type.");

LLVM_LIBC_FUNCTION(int, pthread_cond_timedwait,
                   (pthread_cond_t *__restrict cond,
                    pthread_mutex_t *__restrict mutex,
                    const struct timespec *__restrict abstime)) {
  cancel_check(); // POSIX cancellation point (pre-wait check).
  // POSIX: default clock is CLOCK_REALTIME unless condattr set otherwise.
  // TODO: read clock from condattr if stored in CndVar.
  auto timeout = internal::AbsTimeout::from_timespec(
      *abstime, /*is_realtime=*/true);
  if (!timeout)
    return EINVAL;

  CndVar *cndvar = reinterpret_cast<CndVar *>(cond);
  Mutex *m = reinterpret_cast<Mutex *>(mutex);
  int ret = cndvar->wait(m, timeout.value());
  // POSIX §2.9.5: mutex must be re-acquired before cleanup handlers run.
  // CndVar::wait always re-locks before returning. Check cancel now.
  cancel_check();
  if (ret == ETIMEDOUT)
    return ETIMEDOUT;
  if (ret == 0)
    return 0;
  return EINVAL;
}

} // namespace LIBC_NAMESPACE_DECL
