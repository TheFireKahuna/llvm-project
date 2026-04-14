//===-- Windows implementation of copy_file_range --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point -- delegates to internal::copy_file_range() in file_ops.cpp.
// Uses NtCopyFileChunk for kernel-mode file-to-file copy.
//
//===----------------------------------------------------------------------===//

#include "src/unistd/copy_file_range.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/copy_file_range.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/pthread/cancel_internal.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(ssize_t, copy_file_range,
                   (int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                    size_t len, unsigned int flags)) {
  cancel_check(); // copy_file_range is a blocking I/O cancellation point.

  auto result =
      windows_syscalls::copy_file_range(fd_in, off_in, fd_out, off_out,
                                        len, flags);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
