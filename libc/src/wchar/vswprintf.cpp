//===-- Implementation of vswprintf ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/vswprintf.h"

#include "src/__support/arg_list.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/stdio/printf_core/core_structs.h"
#include "src/stdio/printf_core/error_mapper.h"
#include "src/stdio/printf_core/wide_writer.h"
#include "src/stdio/printf_core/wprintf_main.h"

#include <stdarg.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, vswprintf,
                   (wchar_t *__restrict s, size_t n,
                    const wchar_t *__restrict format, va_list vlist)) {
  internal::ArgList args(vlist);

  // C11 §7.29.2.4p3: if n is zero the buffer may be a null pointer and
  // nothing is written. We still format to count characters.
  printf_core::WideWriter writer(s, n);

  auto ret_val = printf_core::wprintf_main(&writer, format, args);
  if (!ret_val.has_value()) {
    libc_errno = printf_core::internal_error_to_errno(ret_val.error());
    return -1;
  }

  // Null-terminate. When n > 0, writes at buff_cur (or at n-1 if overflow).
  if (n > 0)
    writer.null_terminate();

  // C11 §7.29.2.4p4: "returns the number of wide characters that would have
  // been written had n been sufficiently large, not counting the terminating
  // null wide character, or a negative value if an encoding error occurred,
  // or if the number of characters that would have been written [...] is n
  // or more."
  // Note: unlike snprintf, swprintf returns -1 on truncation.
  size_t chars = ret_val.value();
  if (chars >= n) {
    libc_errno = EOVERFLOW;
    return -1;
  }
  return static_cast<int>(chars);
}

} // namespace LIBC_NAMESPACE_DECL
