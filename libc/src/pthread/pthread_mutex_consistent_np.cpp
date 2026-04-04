//===-- Implementation of pthread_mutex_consistent_np ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_mutex_consistent_np.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/mutex.h"

#include <errno.h>
#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

// Pre-POSIX-2008 spelling of pthread_mutex_consistent. Glibc keeps it
// as a strict alias. Drives the same Mutex::make_consistent path so
// behavior on robust mutexes is identical to the standard name.
LLVM_LIBC_FUNCTION(int, pthread_mutex_consistent_np,
                   (pthread_mutex_t * mutex)) {
  auto err = reinterpret_cast<Mutex *>(mutex)->make_consistent();
  if (err == MutexError::NONE)
    return 0;
  return EINVAL;
}

} // namespace LIBC_NAMESPACE_DECL
