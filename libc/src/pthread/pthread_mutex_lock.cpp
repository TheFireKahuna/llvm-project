//===-- Linux implementation of the pthread_mutex_lock function -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_mutex_lock.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/mutex.h"

#include <errno.h>
#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_mutex_lock, (pthread_mutex_t * mutex)) {
  auto err = reinterpret_cast<Mutex *>(mutex)->lock();
  switch (err) {
  case MutexError::NONE:
    return 0;
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
