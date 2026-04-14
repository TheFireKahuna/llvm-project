//===-- Windows implementation of _get_osfhandle --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/_get_osfhandle.h"

#include "src/__support/OSUtil/windows/io/file_ops.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// MSVC CRT compat: returns the Win32 HANDLE for a file descriptor.
LLVM_LIBC_FUNCTION(intptr_t, _get_osfhandle, (int fd)) {
  intptr_t ret = internal::get_osfhandle(fd);
  if (ret < 0)
    return -1;
  return ret;
}

} // namespace LIBC_NAMESPACE_DECL
