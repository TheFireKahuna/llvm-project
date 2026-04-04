//===-- Implementation of sem_unlink --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "sem_unlink.h"

#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "src/__support/threads/windows/named_semaphore.h"
#endif

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, sem_unlink, (const char *name)) {
#ifdef LIBC_TARGET_OS_IS_WINDOWS
  int err = NamedSemaphore::unlink(name);
  if (err != 0) {
    libc_errno = err;
    return -1;
  }
  return 0;
#else
  (void)name;
  libc_errno = ENOSYS;
  return -1;
#endif
}

} // namespace LIBC_NAMESPACE_DECL
