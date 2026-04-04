//===-- Windows implementation of sendfile ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point -- delegates to internal::sendfile() in sendfile_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/sendfile/sendfile.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/sendfile.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/pthread/cancel_internal.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(ssize_t, sendfile,
                   (int out_fd, int in_fd, off_t *offset, size_t count)) {
  cancel_check(); // sendfile is a blocking I/O operation.

  auto result = windows_syscalls::sendfile(out_fd, in_fd, offset, count);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
