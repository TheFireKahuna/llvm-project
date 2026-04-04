//===-- Windows implementation of preadv -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point -- delegates to internal::preadv() in scatter_io_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/uio/preadv.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/preadv.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/pthread/cancel_internal.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(ssize_t, preadv,
                   (int fd, const iovec *iov, int iovcnt, off_t offset)) {
  cancel_check(); // POSIX cancellation point.

  auto result = windows_syscalls::preadv(fd, iov, iovcnt, offset);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
