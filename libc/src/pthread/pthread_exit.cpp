//===-- Implementation of the pthread_exit function -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_exit.h"

#include "cancel_internal.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/thread.h"

#include <pthread.h> // For pthread_* type definitions.

namespace LIBC_NAMESPACE_DECL {

static_assert(sizeof(pthread_t) == sizeof(LIBC_NAMESPACE::Thread),
              "Mismatch between pthread_t and internal Thread.");

LLVM_LIBC_FUNCTION(void, pthread_exit, (void *retval)) {
  // Use forced unwind so C++ destructors for automatic objects fire as
  // the stack unwinds. Falls back to direct thread_exit if unwind fails.
  LIBC_NAMESPACE::exit_act(retval);
}

} // namespace LIBC_NAMESPACE_DECL
