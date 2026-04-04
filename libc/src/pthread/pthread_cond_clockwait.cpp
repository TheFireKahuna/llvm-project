//===-- Implementation of pthread_cond_clockwait --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_cond_clockwait.h"

#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/CndVar.h"
#include "src/__support/threads/mutex.h"
#include "src/pthread/cancel_internal.h"

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "src/__support/time/windows/clock_conversion.h"
#else
#include "src/__support/time/abs_timeout.h"
#endif

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

static_assert(sizeof(CndVar) <= sizeof(pthread_cond_t),
              "The public pthread_cond_t type cannot accommodate the internal "
              "CndVar type.");

LLVM_LIBC_FUNCTION(int, pthread_cond_clockwait,
                   (pthread_cond_t *__restrict cond,
                    pthread_mutex_t *__restrict mutex, clockid_t clockid,
                    const struct timespec *__restrict abstime)) {
  cancel_check();

#ifdef LIBC_TARGET_OS_IS_WINDOWS
  if (!internal::is_valid_wait_clock(clockid))
    return EINVAL;
  auto timeout = internal::abs_timeout_from_clock(clockid, *abstime);
#else
  if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME)
    return EINVAL;
  bool is_realtime = (clockid == CLOCK_REALTIME);
  auto timeout = internal::AbsTimeout::from_timespec(
      *abstime, /*is_realtime=*/is_realtime);
#endif
  if (!timeout)
    return EINVAL;

  CndVar *cndvar = reinterpret_cast<CndVar *>(cond);
  Mutex *m = reinterpret_cast<Mutex *>(mutex);
  int ret = cndvar->wait(m, timeout.value());
  cancel_check();
  if (ret == ETIMEDOUT)
    return ETIMEDOUT;
  if (ret == 0)
    return 0;
  return EINVAL;
}

} // namespace LIBC_NAMESPACE_DECL
