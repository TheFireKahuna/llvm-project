//===-- Windows implementation of _open_osfhandle -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/_open_osfhandle.h"

#include "src/__support/OSUtil/windows/io/file_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// MSVC CRT compat: allocates a file descriptor for a Win32 HANDLE.
LLVM_LIBC_FUNCTION(int, _open_osfhandle, (intptr_t osfhandle, int flags)) {
  intptr_t ret = internal::open_osfhandle(osfhandle, flags);
  if (ret < 0) {
    libc_errno = static_cast<int>(-ret);
    return -1;
  }
  return static_cast<int>(ret);
}

} // namespace LIBC_NAMESPACE_DECL
