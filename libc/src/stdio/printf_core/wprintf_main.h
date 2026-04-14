//===-- Starting point for wprintf -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WPRINTF_MAIN_H
#define LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WPRINTF_MAIN_H

#include "hdr/types/wchar_t.h"
#include "src/__support/arg_list.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/stdio/printf_core/parser.h"
#include "src/stdio/printf_core/wide_converter.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace printf_core {

template <typename WideWriterT>
LIBC_INLINE ErrorOr<size_t>
wprintf_main(WideWriterT *writer, const wchar_t *__restrict str,
             internal::ArgList &args) {
  Parser<internal::ArgList, wchar_t> parser(str, args);
  int result = 0;

  for (WideFormatSection cur_section = parser.get_next_section();
       cur_section.raw_len != 0 || cur_section.conv.has_conv;
       cur_section = parser.get_next_section()) {
    if (cur_section.conv.has_conv) {
      result = wide_convert(writer, cur_section.conv);
    } else {
      // Literal text — write wide characters directly.
      result = writer->write_wide(cur_section.raw_begin, cur_section.raw_len);
    }
    if (result < 0)
      return Error(-result);
  }

  return writer->get_chars_written();
}

} // namespace printf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WPRINTF_MAIN_H
