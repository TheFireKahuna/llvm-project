//===-- Starting point for scanf --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_SCANF_CORE_SCANF_MAIN_H
#define LLVM_LIBC_SRC_STDIO_SCANF_CORE_SCANF_MAIN_H

#include "src/__support/arg_list.h"
#include "src/__support/macros/config.h"
#include "src/stdio/scanf_core/converter.h"
#include "src/stdio/scanf_core/core_structs.h"
#include "src/stdio/scanf_core/parser.h"
#include "src/stdio/scanf_core/reader.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace scanf_core {

template <typename T, typename CharType = char>
int scanf_main(Reader<T, CharType> *reader, const CharType *__restrict str,
               internal::ArgList &args) {
  Parser<internal::ArgList, CharType> parser(str, args);
  int ret_val = READ_OK;
  int conversions = 0;
  for (FormatSection cur_section = parser.get_next_section();
       cur_section.raw_len != 0 && ret_val == READ_OK;
       cur_section = parser.get_next_section()) {
    if (cur_section.has_conv) {
      ret_val = convert(reader, cur_section);
      // Only count successful assignments. %n doesn't count (C11 §7.21.6.2p12)
      // and assignment-suppressed conversions (%*) don't count either
      // (C11 §7.21.6.2p16: returns "the number of input items assigned").
      if (cur_section.conv_name != 'n' &&
          (cur_section.flags & FormatFlags::NO_WRITE) == 0)
        conversions += ret_val == READ_OK ? 1 : 0;
    } else {
      ret_val = raw_match(
          reader, static_cast<const CharType *>(cur_section.raw_begin),
          cur_section.raw_len);
    }
  }

  // C11 §7.21.6.2p16: return EOF when an input failure occurs before the
  // first conversion completes.  If the conversion consumed characters
  // (chars_read > 0) the failure is a matching failure, not input failure.
  // When chars_read == 0, the converter read one char and pushed it back
  // (balanced getc/ungetc) — we must probe to distinguish "first char was
  // EOF" (input failure → -1) from "first char didn't match" (matching
  // failure → 0).
  if (conversions == 0 && ret_val != READ_OK) {
    if (reader->chars_read() > 0)
      return 0;
    CharType probe = reader->getc();
    reader->ungetc(probe);
    if (probe == CharType('\0'))
      return -1;
  }

  return conversions;
}

} // namespace scanf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_SCANF_CORE_SCANF_MAIN_H
