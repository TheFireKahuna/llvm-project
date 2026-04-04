//===-- Implementation of sem_init ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "sem_init.h"

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

LLVM_LIBC_FUNCTION(int, sem_init, (sem_t * sem, int pshared, unsigned value)) {
  // Store the kind byte first.
  sem->__data[0] = SEM_KIND_UNNAMED;

  // The Semaphore object starts at the next aligned offset after the kind byte.
  auto *s = reinterpret_cast<Semaphore *>(&sem->__data[alignof(Semaphore)]);
  int err = Semaphore::init(s, pshared != 0, value);
  if (err != 0) {
    libc_errno = err;
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
