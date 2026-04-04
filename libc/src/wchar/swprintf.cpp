//===-- Implementation of swprintf ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/swprintf.h"

#include "src/__support/arg_list.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/stdio/printf_core/core_structs.h"
#include "src/stdio/printf_core/error_mapper.h"
#include "src/stdio/printf_core/wide_writer.h"
#include "src/stdio/printf_core/wprintf_main.h"

#include <stdarg.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, swprintf,
                   (wchar_t *__restrict s, size_t n,
                    const wchar_t *__restrict format, ...)) {
  va_list vlist;
  va_start(vlist, format);
  internal::ArgList args(vlist);
  va_end(vlist);

  printf_core::WideWriter writer(s, n);

  auto ret_val = printf_core::wprintf_main(&writer, format, args);
  if (!ret_val.has_value()) {
    libc_errno = printf_core::internal_error_to_errno(ret_val.error());
    return -1;
  }

  if (n > 0)
    writer.null_terminate();

  // C11 §7.29.2.4: swprintf returns -1 if n or more wide characters were
  // requested (unlike snprintf which returns would-have-written count).
  size_t chars = ret_val.value();
  if (chars >= n) {
    libc_errno = EOVERFLOW;
    return -1;
  }
  return static_cast<int>(chars);
}

} // namespace LIBC_NAMESPACE_DECL
