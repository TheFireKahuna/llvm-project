//===-- Implementation of sem_open ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "sem_open.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/types/mode_t.h"
#include "hdr/types/sem_t.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"

#include <stdarg.h>

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "src/__support/threads/windows/named_semaphore.h"
#endif

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(sem_t *, sem_open, (const char *name, int oflag, ...)) {
  mode_t mode = 0;
  unsigned value = 0;

  if (oflag & O_CREAT) {
    va_list ap;
    va_start(ap, oflag);
    mode = static_cast<mode_t>(va_arg(ap, unsigned));
    value = va_arg(ap, unsigned);
    va_end(ap);
  }

#ifdef LIBC_TARGET_OS_IS_WINDOWS
  // Windows-backed llvm-libc supports both POSIX slash names and an
  // NTPOSIX-specific slashless open-only path for existing native NT named
  // semaphores. Future work belongs in NamedSemaphore rather than adding more
  // policy here.
  int err = 0;
  sem_t *result = NamedSemaphore::open(name, oflag, mode, value, &err);
  if (!result) {
    libc_errno = err;
    return nullptr; // SEM_FAILED
  }
  return result;
#else
  // Linux named semaphores via /dev/shm not yet implemented.
  libc_errno = ENOSYS;
  return nullptr;
#endif
}

} // namespace LIBC_NAMESPACE_DECL
