//===-- Implementation of pthread_tryjoin_np ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_tryjoin_np.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/thread.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

static_assert(sizeof(pthread_t) == sizeof(LIBC_NAMESPACE::Thread),
              "Mismatch between pthread_t and internal Thread.");

// Glibc's pthread_tryjoin_np is NOT a cancellation point — only the
// blocking joins (pthread_join, pthread_timedjoin_np, pthread_clockjoin_np)
// are. Match that here so a cancel pending at call time is not delivered
// inside what the caller intends as a non-blocking probe.
LLVM_LIBC_FUNCTION(int, pthread_tryjoin_np, (pthread_t th, void **retval)) {
  auto *thread = reinterpret_cast<Thread *>(&th);
  return thread->try_join(retval);
}

} // namespace LIBC_NAMESPACE_DECL
