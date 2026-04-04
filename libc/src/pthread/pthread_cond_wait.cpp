//===-- Implementation of the pthread_cond_wait function -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_cond_wait.h"

#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/pthread/cancel_internal.h"
#include "src/__support/threads/CndVar.h"
#include "src/__support/threads/mutex.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

static_assert(sizeof(CndVar) <= sizeof(pthread_cond_t),
              "The public pthread_cond_t type cannot accommodate the internal "
              "CndVar type.");
static_assert(sizeof(Mutex) <= sizeof(pthread_mutex_t),
              "The public pthread_mutex_t type cannot accommodate the internal "
              "mutex type.");

LLVM_LIBC_FUNCTION(int, pthread_cond_wait,
                   (pthread_cond_t * cond, pthread_mutex_t *mutex)) {
  cancel_check(); // POSIX cancellation point (pre-wait check).
  CndVar *cndvar = reinterpret_cast<CndVar *>(cond);
  Mutex *m = reinterpret_cast<Mutex *>(mutex);
  int ret = cndvar->wait(m);
  // POSIX §2.9.5: when acting on cancellation during a condition wait, the
  // mutex must be re-acquired before the first cleanup handler runs.
  // CndVar::wait always re-locks the mutex before returning, so the mutex
  // is held here. Check cancel now — if pending, cancel_act will unwind
  // with the mutex held, and the user's cleanup handler can unlock it.
  cancel_check();
  return ret ? EINVAL : 0;
}

} // namespace LIBC_NAMESPACE_DECL
