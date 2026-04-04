//===-- Format specifier converter for scanf -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_SCANF_CORE_CONVERTER_H
#define LLVM_LIBC_SRC_STDIO_SCANF_CORE_CONVERTER_H

#include "src/__support/CPP/string_view.h"
#include "src/__support/CPP/type_traits/type_identity.h"
#include "src/__support/ctype_utils.h"
#include "src/__support/macros/config.h"
#include "src/__support/wctype_utils.h"
#include "src/stdio/scanf_core/core_structs.h"
#include "src/stdio/scanf_core/reader.h"

#ifndef LIBC_COPT_SCANF_DISABLE_FLOAT
#include "src/stdio/scanf_core/float_converter.h"
#endif // LIBC_COPT_SCANF_DISABLE_FLOAT
#include "src/stdio/scanf_core/current_pos_converter.h"
#include "src/stdio/scanf_core/int_converter.h"
#include "src/stdio/scanf_core/ptr_converter.h"
#include "src/stdio/scanf_core/string_converter.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace scanf_core {

// raw_match takes a raw string and matches it to the characters obtained from
// the reader. Whitespace in the pattern matches zero or more whitespace
// characters in the input. CharType is deduced from the reader; the
// raw_string argument is a non-deduced context so string-literal callers
// can rely on basic_string_view's implicit constructor.
template <typename T, typename CharType = char>
LIBC_INLINE int raw_match(
    Reader<T, CharType> *reader,
    typename cpp::type_identity<cpp::basic_string_view<CharType>>::type
        raw_string) {
  CharType cur_char = reader->getc();
  int ret_val = READ_OK;
  for (size_t i = 0; i < raw_string.size(); ++i) {
    // Any space character matches any number of space characters.
    if (internal::isspace(raw_string[i])) {
      while (internal::isspace(cur_char)) {
        cur_char = reader->getc();
      }
    } else {
      if (raw_string[i] == cur_char) {
        cur_char = reader->getc();
      } else {
        ret_val = MATCHING_FAILURE;
        break;
      }
    }
  }
  reader->ungetc(cur_char);
  return ret_val;
}

// convert reads the conversion described by to_conv from the reader. It
// dispatches by conv_name to the appropriate converter.
template <typename T, typename CharType = char>
LIBC_INLINE int
convert(Reader<T, CharType> *reader,
        const basic_format_section<CharType> &to_conv) {
  int ret_val = 0;
  const CharType percent_pat[] = {CharType('%'), CharType(0)};
  const CharType space_pat[] = {CharType(' '), CharType(0)};
  switch (to_conv.conv_name) {
  case '%':
    return raw_match(reader, percent_pat);
  case 's':
    ret_val = raw_match(reader, space_pat);
    if (ret_val != READ_OK)
      return ret_val;
    return convert_string(reader, to_conv);
  case 'c':
  case '[':
    return convert_string(reader, to_conv);
  case 'd':
  case 'i':
  case 'u':
  case 'o':
  case 'x':
  case 'X':
    ret_val = raw_match(reader, space_pat);
    if (ret_val != READ_OK)
      return ret_val;
    return convert_int(reader, to_conv);
#ifndef LIBC_COPT_SCANF_DISABLE_FLOAT
  case 'f':
  case 'F':
  case 'e':
  case 'E':
  case 'a':
  case 'A':
  case 'g':
  case 'G':
    ret_val = raw_match(reader, space_pat);
    if (ret_val != READ_OK)
      return ret_val;
    return convert_float(reader, to_conv);
#endif // LIBC_COPT_SCANF_DISABLE_FLOAT
  case 'n':
    return convert_current_pos(reader, to_conv);
  case 'p':
    ret_val = raw_match(reader, space_pat);
    if (ret_val != READ_OK)
      return ret_val;
    return convert_pointer(reader, to_conv);
  default:
    return raw_match(reader, to_conv.raw_string);
  }
  return -1;
}

} // namespace scanf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_SCANF_CORE_CONVERTER_H
