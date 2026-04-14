//===-- Implementation of vswscanf ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/vswscanf.h"

#include "hdr/stdio_macros.h"
#include "src/__support/CPP/limits.h"
#include "src/__support/arg_list.h"
#include "src/__support/macros/config.h"
#include "src/stdio/scanf_core/scanf_main.h"
#include "src/stdio/scanf_core/string_reader.h"

#include <stdarg.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, vswscanf,
                   (const wchar_t *__restrict s,
                    const wchar_t *__restrict format, va_list vlist)) {
  internal::ArgList args(vlist);
  scanf_core::StringReader<wchar_t> reader(s,
                                           cpp::numeric_limits<size_t>::max());
  int ret_val = scanf_core::scanf_main(&reader, format, args);
  return (ret_val == -1) ? EOF : ret_val;
}

} // namespace LIBC_NAMESPACE_DECL
