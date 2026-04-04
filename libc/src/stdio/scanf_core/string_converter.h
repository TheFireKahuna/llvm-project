//===-- String type specifier converters for scanf --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_SCANF_CORE_STRING_CONVERTER_H
#define LLVM_LIBC_SRC_STDIO_SCANF_CORE_STRING_CONVERTER_H

#include "src/__support/CPP/limits.h"
#include "src/__support/CPP/type_traits.h"
#include "src/__support/ctype_utils.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/character_converter.h"
#include "src/__support/wchar/locale_encoding.h"
#include "src/__support/wchar/mbstate.h"
#include "src/__support/wctype_utils.h"
#include "src/stdio/scanf_core/char_ops.h"
#include "src/stdio/scanf_core/core_structs.h"
#include "src/stdio/scanf_core/reader.h"

#include "hdr/types/wchar_t.h"
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace scanf_core {

// Same-width string conversion: CharType in, CharType out.
// Used for narrow %s/%c/%[ and wide %ls/%lc/%l[.
template <typename T, typename CharType, typename SectionT>
LIBC_INLINE int convert_string_same(Reader<T, CharType> *reader,
                                    const SectionT &to_conv,
                                    size_t max_width) {
  CharType *output = reinterpret_cast<CharType *>(to_conv.output_ptr);

  CharType cur_char = reader->getc();
  size_t i = 0;
  for (; i < max_width && cur_char != CharType(); ++i) {
    if ((to_conv.conv_name == 's' && internal::isspace(cur_char)) ||
        (to_conv.conv_name == '[' &&
         !CharOps<CharType>::in_scanset(to_conv, cur_char))) {
      break;
    }
    if ((to_conv.flags & NO_WRITE) == 0)
      output[i] = cur_char;
    cur_char = reader->getc();
  }

  reader->ungetc(cur_char);

  if (to_conv.conv_name != 'c' && (to_conv.flags & NO_WRITE) == 0)
    output[i] = CharType();

  // C11 §7.21.6.2p12: %c matches "exactly the number specified by the field
  // width". If fewer characters are available, it is a matching failure.
  if (to_conv.conv_name == 'c' && i < max_width)
    return MATCHING_FAILURE;

  if (i == 0)
    return MATCHING_FAILURE;
  return READ_OK;
}

// Wide-to-narrow: read wchar_t from stream, convert to multibyte, write char*.
// Used by wscanf %s/%c (no l modifier).
template <typename T>
LIBC_INLINE int convert_string_wide_to_narrow(Reader<T, wchar_t> *reader,
                                              const WideFormatSection &to_conv,
                                              size_t max_width) {
  char *output = reinterpret_cast<char *>(to_conv.output_ptr);
  internal::mbstate state{};

  wchar_t cur_char = reader->getc();
  size_t out_idx = 0;
  size_t chars_read = 0;
  for (; chars_read < max_width && cur_char != L'\0'; ++chars_read) {
    if ((to_conv.conv_name == 's' && internal::isspace(cur_char)) ||
        (to_conv.conv_name == '[' &&
         !CharOps<wchar_t>::in_scanset(to_conv, cur_char))) {
      break;
    }
    if ((to_conv.flags & NO_WRITE) == 0) {
      // Convert wchar_t → UTF-8 bytes.
      internal::CharacterConverter cr(&state,
                                      internal::locale_encoding_is_utf8());
      if (cr.push(static_cast<char32_t>(cur_char)) != 0)
        return MATCHING_FAILURE;
      while (!cr.isEmpty()) {
        auto byte = cr.pop_utf8();
        if (!byte.has_value())
          return MATCHING_FAILURE;
        output[out_idx++] = static_cast<char>(byte.value());
      }
    }
    cur_char = reader->getc();
  }

  reader->ungetc(cur_char);

  if (to_conv.conv_name != 'c' && (to_conv.flags & NO_WRITE) == 0)
    output[out_idx] = '\0';

  if (to_conv.conv_name == 'c' && chars_read < max_width)
    return MATCHING_FAILURE;

  if (chars_read == 0)
    return MATCHING_FAILURE;
  return READ_OK;
}

// Narrow-to-wide: read multibyte char from stream, convert to wchar_t,
// write wchar_t*. Used by scanf %ls/%lc.
//
// The scanset (for %l[) lives in the narrow FormatSection's bitset<256>; a
// decoded wchar_t outside that range is by construction not in the set.
template <typename T>
LIBC_INLINE int convert_string_narrow_to_wide(Reader<T, char> *reader,
                                              const FormatSection &to_conv,
                                              size_t max_width) {
  wchar_t *output = reinterpret_cast<wchar_t *>(to_conv.output_ptr);
  internal::mbstate state{};

  char cur_char = reader->getc();
  size_t wchars_written = 0;
  // max_width counts wide characters produced.
  for (; wchars_written < max_width && cur_char != '\0';) {
    // Accumulate multibyte bytes until a full wchar_t is decoded.
    internal::CharacterConverter cr(&state,
                                    internal::locale_encoding_is_utf8());
    while (cur_char != '\0') {
      int err =
          cr.push(static_cast<char8_t>(static_cast<unsigned char>(cur_char)));
      if (err != 0)
        return MATCHING_FAILURE;
      if (cr.isFull())
        break;
      cur_char = reader->getc();
    }
    if (!cr.isFull()) {
      // Incomplete sequence at EOF.
      reader->ungetc(cur_char);
      break;
    }
    auto wc_result = cr.pop_utf32();
    if (!wc_result.has_value())
      return MATCHING_FAILURE;
    wchar_t wc = static_cast<wchar_t>(wc_result.value());

    const bool in_narrow_scanset =
        static_cast<char32_t>(wc) < 256 &&
        to_conv.scan_set.test(static_cast<unsigned char>(wc));

    if ((to_conv.conv_name == 's' && internal::isspace(wc)) ||
        (to_conv.conv_name == '[' && !in_narrow_scanset)) {
      reader->ungetc(cur_char);
      break;
    }
    if ((to_conv.flags & NO_WRITE) == 0)
      output[wchars_written] = wc;
    ++wchars_written;
    cur_char = reader->getc();
  }

  reader->ungetc(cur_char);

  if (to_conv.conv_name != 'c' && (to_conv.flags & NO_WRITE) == 0)
    output[wchars_written] = L'\0';

  if (to_conv.conv_name == 'c' && wchars_written < max_width)
    return MATCHING_FAILURE;

  if (wchars_written == 0)
    return MATCHING_FAILURE;
  return READ_OK;
}

template <typename T, typename CharType = char>
LIBC_INLINE int convert_string(Reader<T, CharType> *reader,
                               const basic_format_section<CharType> &to_conv) {
  size_t max_width = 0;
  if (to_conv.max_width > 0) {
    max_width = to_conv.max_width;
  } else {
    if (to_conv.conv_name == 'c') {
      max_width = 1;
    } else {
      max_width = cpp::numeric_limits<size_t>::max();
    }
  }

  const bool has_l = (to_conv.length_modifier == LengthModifier::l);

  // Cross-width conversions.
  if constexpr (cpp::is_same_v<CharType, wchar_t>) {
    if (!has_l)
      return convert_string_wide_to_narrow(reader, to_conv, max_width);
  } else {
    if (has_l)
      return convert_string_narrow_to_wide(reader, to_conv, max_width);
  }

  // Same-width: CharType in, CharType out.
  return convert_string_same(reader, to_conv, max_width);
}

} // namespace scanf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_SCANF_CORE_STRING_CONVERTER_H
