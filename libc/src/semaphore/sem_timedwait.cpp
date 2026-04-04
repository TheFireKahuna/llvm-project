//===-- Implementation of sem_timedwait -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "sem_timedwait.h"

#include "hdr/errno_macros.h"
#include "hdr/types/sem_t.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"
#include "src/__support/time/abs_timeout.h"
#if defined(__linux__)
#include "src/__support/threads/linux/semaphore.h"
#elif defined(LIBC_TARGET_OS_IS_WINDOWS)
#include "src/__support/threads/windows/semaphore.h"
#include "src/__support/threads/windows/named_semaphore.h"
#include "src/pthread/cancel_internal.h"
#endif

namespace LIBC_NAMESPACE_DECL {

// POSIX: sem_timedwait is a cancellation point.
LLVM_LIBC_FUNCTION(int, sem_timedwait,
                   (sem_t *__restrict sem,
                    const struct timespec *__restrict abstime)) {
  auto timeout = internal::AbsTimeout::from_timespec(
      *abstime, /*is_realtime=*/true);
  if (!timeout) {
    libc_errno = EINVAL;
    return -1;
  }

#ifdef LIBC_TARGET_OS_IS_WINDOWS
  cancel_check();
#endif

  int err;
  if (sem->__data[0] == SEM_KIND_UNNAMED) {
    auto *s = reinterpret_cast<Semaphore *>(&sem->__data[alignof(Semaphore)]);
    err = s->timedwait(timeout.value());
  } else {
#ifdef LIBC_TARGET_OS_IS_WINDOWS
    err = NamedSemaphore::timedwait(sem, timeout.value());
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
