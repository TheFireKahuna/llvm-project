//===---------- Windows implementation of the POSIX tmpfile function ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::tmpfile_impl() in stdio_file_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/stdio/tmpfile.h"

#include "hdr/types/FILE.h"
#include "src/__support/OSUtil/windows/io/stdio_file_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(::FILE *, tmpfile, ()) {
  ::FILE *out = nullptr;
  intptr_t ret = internal::tmpfile_impl(&out);
  if (ret < 0) {
    libc_errno = static_cast<int>(-ret);
    return nullptr;
  }
  return out;
}

} // namespace LIBC_NAMESPACE_DECL
