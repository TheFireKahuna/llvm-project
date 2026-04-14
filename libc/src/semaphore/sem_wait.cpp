//===-- Implementation of sem_wait ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "sem_wait.h"

#include "hdr/errno_macros.h"
#include "hdr/types/sem_t.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"
#if defined(__linux__)
#include "src/__support/threads/linux/semaphore.h"
#elif defined(LIBC_TARGET_OS_IS_WINDOWS)
#include "src/__support/threads/windows/semaphore.h"
#include "src/__support/threads/windows/named_semaphore.h"
#include "src/pthread/cancel_internal.h"
#endif

namespace LIBC_NAMESPACE_DECL {

// POSIX: sem_wait is a cancellation point.
LLVM_LIBC_FUNCTION(int, sem_wait, (sem_t * sem)) {
#ifdef LIBC_TARGET_OS_IS_WINDOWS
  cancel_check();
#endif
  int err;
  if (sem->__data[0] == SEM_KIND_UNNAMED) {
    auto *s = reinterpret_cast<Semaphore *>(&sem->__data[alignof(Semaphore)]);
    err = s->wait();
  } else {
#ifdef LIBC_TARGET_OS_IS_WINDOWS
    err = NamedSemaphore::wait(sem);
#else
    err = EINVAL;
#endif
  }
#ifdef LIBC_TARGET_OS_IS_WINDOWS
  cancel_check();
#endif
  if (err != 0) {
    libc_errno = err;
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
