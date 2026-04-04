//===-- Implementation of pthread_mutex_clocklock -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_mutex_clocklock.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/mutex.h"

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "src/__support/time/windows/clock_conversion.h"
#else
#include "src/__support/time/abs_timeout.h"
#endif

#include <errno.h>
#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_mutex_clocklock,
                   (pthread_mutex_t *__restrict mutex, clockid_t clockid,
                    const struct timespec *__restrict abstime)) {
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

  auto err = reinterpret_cast<Mutex *>(mutex)->timed_lock(timeout.value());
  switch (err) {
  case MutexError::NONE:
    return 0;
  case MutexError::TIMEOUT:
    return ETIMEDOUT;
  case MutexError::OWNER_DEAD:
    return EOWNERDEAD;
  case MutexError::NOT_RECOVERABLE:
    return ENOTRECOVERABLE;
  case MutexError::BAD_LOCK_STATE:
    return EDEADLK;
  default:
    return EINVAL;
  }
}

} // namespace LIBC_NAMESPACE_DECL
