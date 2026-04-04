//===-- Linux implementation of posix_fallocate ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/posix_fallocate.h"

#include "src/__support/OSUtil/syscall.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include <sys/syscall.h>

namespace LIBC_NAMESPACE_DECL {

// posix_fallocate returns an error number directly (not -1 + errno).
LLVM_LIBC_FUNCTION(int, posix_fallocate, (int fd, off_t offset, off_t len)) {
  // mode=0 means default allocation (no FALLOC_FL_* flags).
  int ret =
      LIBC_NAMESPACE::syscall_impl<int>(SYS_fallocate, fd, 0, offset, len);
  // The kernel returns a negative errno on failure.
  return ret < 0 ? -ret : 0;
}

} // namespace LIBC_NAMESPACE_DECL
