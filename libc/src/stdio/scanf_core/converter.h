//===-- Format specifier converter for scanf -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_SCANF_CORE_CONVERTER_H
#define LLVM_LIBC_SRC_STDIO_SCANF_CORE_CONVERTER_H

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
// the reader.
template <typename T, typename CharType = char>
int raw_match(Reader<T, CharType> *reader, const CharType *raw_string,
              size_t len) {
  CharType cur_char = reader->getc();
  int ret_val = READ_OK;
  for (size_t i = 0; i < len; ++i) {
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

// Convenience overload for null-terminated raw strings.
template <typename T, typename CharType = char>
int raw_match(Reader<T, CharType> *reader, const CharType *raw_string) {
  size_t len = 0;
  while (raw_string[len] != CharType())
    ++len;
  return raw_match(reader, raw_string, len);
}

// Convenience overload for matching a single character.
template <typename T, typename CharType = char>
int raw_match(Reader<T, CharType> *reader, CharType ch) {
  return raw_match(reader, &ch, 1);
}

// convert will call a conversion function to convert the FormatSection into
// its string representation, and then that will write the result to the
// reader.
template <typename T, typename CharType = char>
int convert(Reader<T, CharType> *reader, const FormatSection &to_conv) {
  int ret_val = 0;
  switch (to_conv.conv_name) {
  case '%':
    return raw_match(reader, CharType('%'));
  case 's':
    ret_val = raw_match(reader, CharType(' '));
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
    ret_val = raw_match(reader, CharType(' '));
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
    ret_val = raw_match(reader, CharType(' '));
    if (ret_val != READ_OK)
      return ret_val;
    return convert_float(reader, to_conv);
#endif // LIBC_COPT_SCANF_DISABLE_FLOAT
  case 'n':
    return convert_current_pos(reader, to_conv);
  case 'p':
    ret_val = raw_match(reader, CharType(' '));
    if (ret_val != READ_OK)
      return ret_val;
    return convert_pointer(reader, to_conv);
  default:
    return raw_match(reader,
                     static_cast<const CharType *>(to_conv.raw_begin),
                     to_conv.raw_len);
  }
  return -1;
}

} // namespace scanf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_SCANF_CORE_CONVERTER_H
