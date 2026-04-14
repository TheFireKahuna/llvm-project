//===-- Implementation of sem_destroy -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "sem_destroy.h"

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
#endif

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, sem_destroy, (sem_t * sem)) {
  if (sem->__data[0] != SEM_KIND_UNNAMED) {
    libc_errno = EINVAL;
    return -1;
  }
  auto *s = reinterpret_cast<Semaphore *>(&sem->__data[alignof(Semaphore)]);
  int err = Semaphore::destroy(s);
  if (err != 0) {
    libc_errno = err;
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
