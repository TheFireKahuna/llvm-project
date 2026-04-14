//===-- Implementation of wscanf ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/wscanf.h"

#include "src/__support/File/file.h"
#include "src/__support/arg_list.h"
#include "src/__support/macros/config.h"
#include "src/stdio/scanf_core/vfwscanf_internal.h"

#include "hdr/types/FILE.h"
#include <stdarg.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, wscanf,
                   (const wchar_t *__restrict format, ...)) {
  va_list vlist;
  va_start(vlist, format);
  internal::ArgList args(vlist);
  va_end(vlist);
  int ret_val = scanf_core::vfwscanf_internal(
      reinterpret_cast<::FILE *>(stdin), format, args);
  return (ret_val == -1) ? EOF : ret_val;
}

} // namespace LIBC_NAMESPACE_DECL
