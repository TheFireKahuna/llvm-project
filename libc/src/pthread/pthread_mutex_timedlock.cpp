//===-- Implementation of pthread_mutex_timedlock -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_mutex_timedlock.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/mutex.h"
#include "src/__support/time/abs_timeout.h"

#include <errno.h>
#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_mutex_timedlock,
                   (pthread_mutex_t *__restrict mutex,
                    const struct timespec *__restrict abstime)) {
  // POSIX: EINVAL if abstime is invalid.
  auto timeout = internal::AbsTimeout::from_timespec(
      *abstime, /*is_realtime=*/true);
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
