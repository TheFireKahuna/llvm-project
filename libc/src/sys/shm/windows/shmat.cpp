//===---------- Windows implementation of shmat ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/shm/shmat.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/shmat.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void *, shmat, (int shmid, const void *shmaddr, int shmflg)) {
  auto result = windows_syscalls::shmat(shmid, shmaddr, shmflg);
  if (!result.has_value()) {
    libc_errno = result.error();
    return reinterpret_cast<void *>(-1);
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
