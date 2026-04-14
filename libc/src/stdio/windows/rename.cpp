//===---------- Windows implementation of the POSIX rename function -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::rename_file() in rename_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/stdio/rename.h"

#include "src/__support/OSUtil/windows/io/rename_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, rename, (const char *oldpath, const char *newpath)) {
  intptr_t ret = internal::rename_file(oldpath, newpath);
  if (ret < 0) {
    libc_errno = static_cast<int>(-ret);
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
