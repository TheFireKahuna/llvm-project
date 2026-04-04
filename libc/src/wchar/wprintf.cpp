//===-- Implementation of wprintf -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/wprintf.h"

#include "src/__support/File/file.h"
#include "src/__support/arg_list.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/stdio/printf_core/error_mapper.h"
#include "src/stdio/printf_core/vfwprintf_internal.h"

#include "hdr/types/FILE.h"
#include <stdarg.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, wprintf,
                   (const wchar_t *__restrict format, ...)) {
  va_list vlist;
  va_start(vlist, format);
  internal::ArgList args(vlist);
  va_end(vlist);

  auto ret_val = printf_core::vfwprintf_internal(
      reinterpret_cast<::FILE *>(stdout), format, args);
  if (!ret_val.has_value()) {
    libc_errno = printf_core::internal_error_to_errno(ret_val.error());
    return -1;
  }
  return static_cast<int>(ret_val.value());
}

} // namespace LIBC_NAMESPACE_DECL
