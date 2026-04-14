//===-- Linux implementation of shmat -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/shm/shmat.h"

#include "src/__support/OSUtil/syscall.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include <sys/syscall.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void *, shmat, (int shmid, const void *shmaddr, int shmflg)) {
  long ret = LIBC_NAMESPACE::syscall_impl<long>(SYS_shmat, shmid, shmaddr,
                                                shmflg);
  // shmat returns an address on success; on error the kernel returns a
  // negative error code in the range [-4095, -1].
  if (ret < 0 && ret >= -4095) {
    libc_errno = static_cast<int>(-ret);
    return reinterpret_cast<void *>(-1);
  }
  return reinterpret_cast<void *>(ret);
}

} // namespace LIBC_NAMESPACE_DECL
