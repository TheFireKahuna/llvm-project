//===---------- Windows implementation of the POSIX pclose function -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::pclose_impl() in stdio_file_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/stdio/pclose.h"
#include "hdr/types/FILE.h"
#include "src/__support/OSUtil/windows/io/stdio_file_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pclose, (::FILE * stream)) {
  intptr_t ret = internal::pclose_impl(stream);
  if (ret < 0) {
    libc_errno = static_cast<int>(-ret);
    return -1;
  }
  // Return the child's wait status (encoded as waitpid returns it).
  return static_cast<int>(ret);
}

} // namespace LIBC_NAMESPACE_DECL
