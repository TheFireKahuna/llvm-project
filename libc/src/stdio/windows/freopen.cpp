//===---------- Windows implementation of the POSIX freopen function ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::freopen_impl() in stdio_file_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/stdio/freopen.h"

#include "hdr/types/FILE.h"
#include "src/__support/OSUtil/windows/io/stdio_file_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(::FILE *, freopen,
                   (const char *__restrict path, const char *__restrict mode,
                    ::FILE *__restrict stream)) {
  ::FILE *out = nullptr;
  intptr_t ret = internal::freopen_impl(path, mode, stream, &out);
  if (ret < 0) {
    libc_errno = static_cast<int>(-ret);
    return nullptr;
  }
  return out;
}

} // namespace LIBC_NAMESPACE_DECL
