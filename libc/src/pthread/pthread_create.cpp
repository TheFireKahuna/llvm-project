//===-- Implementation of the pthread_create function ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_create.h"

#include "pthread_attr_destroy.h"
#include "pthread_attr_getdetachstate.h"
#include "pthread_attr_getguardsize.h"
#include "pthread_attr_getinheritsched.h"
#include "pthread_attr_getschedparam.h"
#include "pthread_attr_getschedpolicy.h"
#include "pthread_attr_getstack.h"
#include "pthread_attr_init.h"
#include "pthread_setschedparam.h"

#include "src/__support/common.h"
#include "src/__support/OSUtil/windows/resource/rlimit_data_guard.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/thread.h"

#include <pthread.h> // For pthread_* type definitions.

namespace LIBC_NAMESPACE_DECL {

static_assert(sizeof(pthread_t) == sizeof(LIBC_NAMESPACE::Thread),
              "Mismatch between pthread_t and internal Thread.");

LLVM_LIBC_FUNCTION(int, pthread_create,
                   (pthread_t *__restrict th,
                    const pthread_attr_t *__restrict attr,
                    __pthread_start_t func, void *arg)) {
  pthread_attr_t default_attr;
  if (attr == nullptr) {
    // We failed to initialize attributes (should be impossible)
    if (LIBC_UNLIKELY(LIBC_NAMESPACE::pthread_attr_init(&default_attr) != 0))
      return EINVAL;

    attr = &default_attr;
  }

  void *stack;
  size_t stacksize, guardsize;
  int detachstate;

  // As of writing this all the `pthread_attr_get*` functions always succeed.
  if (LIBC_UNLIKELY(
          LIBC_NAMESPACE::pthread_attr_getstack(attr, &stack, &stacksize) != 0))
    return EINVAL;

  if (LIBC_UNLIKELY(
          LIBC_NAMESPACE::pthread_attr_getguardsize(attr, &guardsize) != 0))
    return EINVAL;

  if (LIBC_UNLIKELY(
          LIBC_NAMESPACE::pthread_attr_getdetachstate(attr, &detachstate) != 0))
    return EINVAL;

  if (attr == &default_attr)
    // Should we fail here? Its non-issue as the moment as pthread_attr_destroy
    // can only succeed.
    if (LIBC_UNLIKELY(LIBC_NAMESPACE::pthread_attr_destroy(&default_attr) != 0))
      return EINVAL;

  if (stacksize && stacksize < PTHREAD_STACK_MIN)
    return EINVAL;

  if (guardsize % EXEC_PAGESIZE != 0)
    return EINVAL;

  if (detachstate != PTHREAD_CREATE_DETACHED &&
      detachstate != PTHREAD_CREATE_JOINABLE)
    return EINVAL;

  int inheritsched;
  LIBC_NAMESPACE::pthread_attr_getinheritsched(attr, &inheritsched);

  int schedpolicy;
  struct sched_param schedparam;
  if (inheritsched == PTHREAD_EXPLICIT_SCHED) {
    LIBC_NAMESPACE::pthread_attr_getschedpolicy(attr, &schedpolicy);
    LIBC_NAMESPACE::pthread_attr_getschedparam(attr, &schedparam);
  }

  windows::ScopedRlimitDataPublicCall rlimit_scope;
  auto *thread = reinterpret_cast<LIBC_NAMESPACE::Thread *>(th);
  int result = thread->run(func, arg, stack, stacksize, guardsize,
                           detachstate == PTHREAD_CREATE_DETACHED);
  if (result != 0 && result != EPERM && result != EINVAL)
    return EAGAIN;
  if (result != 0)
    return result;

  // Apply explicit scheduling attributes after creation. For
  // PTHREAD_INHERIT_SCHED (the default), the OS already inherited the
  // creating thread's priority; the sched_policy field in ThreadSignalState
  // is inherited via StartArgs in the platform thread.cpp.
  if (inheritsched == PTHREAD_EXPLICIT_SCHED) {
    pthread_t created = *th;
    LIBC_NAMESPACE::pthread_setschedparam(created, schedpolicy, &schedparam);
  }

  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
