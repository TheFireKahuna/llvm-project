//===-- Windows implementation of fstatvfs --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::fstatvfs() in statvfs_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/statvfs/fstatvfs.h"

#include "src/__support/OSUtil/windows/io/statvfs_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, fstatvfs, (int fd, struct statvfs *buf)) {
  intptr_t ret = internal::fstatvfs(fd, buf);
  if (ret < 0) {
    libc_errno = static_cast<int>(-ret);
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
