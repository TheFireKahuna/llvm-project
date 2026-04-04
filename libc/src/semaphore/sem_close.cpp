//===-- Implementation of sem_close ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "sem_close.h"

#include "hdr/errno_macros.h"
#include "hdr/types/sem_t.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "src/__support/threads/windows/named_semaphore.h"
#include "src/__support/threads/windows/semaphore.h"
#endif

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, sem_close, (sem_t * sem)) {
#ifdef LIBC_TARGET_OS_IS_WINDOWS
  if (sem->__data[0] != SEM_KIND_NAMED) {
    libc_errno = EINVAL;
    return -1;
  }
  int err = NamedSemaphore::close(sem);
  if (err != 0) {
    libc_errno = err;
    return -1;
  }
  return 0;
#else
  (void)sem;
  libc_errno = ENOSYS;
  return -1;
#endif
}

} // namespace LIBC_NAMESPACE_DECL
