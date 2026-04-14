//===-- Implementation of wcsftime ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/wcsftime.h"

#include "hdr/types/size_t.h"
#include "hdr/types/struct_tm.h"
#include "hdr/types/wchar_t.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/time/strftime_core/converter.h"
#include "src/time/strftime_core/core_structs.h"

namespace LIBC_NAMESPACE_DECL {

// Adapts a wchar_t output buffer to the writer interface expected by
// strftime_core converters. All converter output is narrow ASCII (digits,
// month names, separators); we widen each char as it's written. Raw wide
// format segments are written directly via write(wchar_t).
namespace {
class WcsftimeWriter {
  wchar_t *buff;
  size_t buff_len;
  size_t buff_cur = 0;
  size_t chars_written = 0;

  LIBC_INLINE void put(wchar_t wc) {
    ++chars_written;
    if (buff_cur < buff_len)
      buff[buff_cur++] = wc;
  }

public:
  LIBC_INLINE WcsftimeWriter(wchar_t *buff, size_t buff_len)
      : buff(buff), buff_len(buff_len) {}

  // Narrow string -> widen each byte to wchar_t.
  LIBC_INLINE int write(cpp::string_view sv) {
    for (size_t i = 0; i < sv.size(); ++i)
      put(static_cast<wchar_t>(static_cast<unsigned char>(sv[i])));
    return 0;
  }

  // Single narrow char -> widen.
  LIBC_INLINE int write(char c) {
    put(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return 0;
  }

  // Repeated narrow char -> widen.
  LIBC_INLINE int write(char c, size_t count) {
    wchar_t wc = static_cast<wchar_t>(static_cast<unsigned char>(c));
    for (size_t i = 0; i < count; ++i)
      put(wc);
    return 0;
  }

  // Wide char -- for raw format segments.
  LIBC_INLINE int write(wchar_t wc) {
    put(wc);
    return 0;
  }

  LIBC_INLINE size_t get_chars_written() const { return chars_written; }
};
} // anonymous namespace

LLVM_LIBC_FUNCTION(size_t, wcsftime,
                   (wchar_t *__restrict s, size_t maxsize,
                    const wchar_t *__restrict format, const tm *timeptr)) {
  size_t usable = maxsize > 0 ? maxsize - 1 : 0;
  WcsftimeWriter writer(s, usable);

  const wchar_t *pos = format;
  int result = 0;

  while (*pos != L'\0' && result >= 0) {
    if (*pos != L'%') {
      // Raw segment: write wide chars directly.
      while (*pos != L'%' && *pos != L'\0' && result >= 0) {
        result = writer.write(*pos);
        ++pos;
      }
      continue;
    }

    // Format specifier -- all specifier syntax is ASCII.
    ++pos; // skip '%'

    strftime_core::FormatSection section;
    section.has_conv = true;

    // Parse flags.
    strftime_core::FormatFlags flags = strftime_core::FormatFlags(0);
    bool found_flag = true;
    while (found_flag) {
      char c = static_cast<char>(*pos);
      switch (c) {
      case '+':
        flags = static_cast<strftime_core::FormatFlags>(
            flags | strftime_core::FormatFlags::FORCE_SIGN);
        ++pos;
        break;
      case '0':
        flags = static_cast<strftime_core::FormatFlags>(
            flags | strftime_core::FormatFlags::LEADING_ZEROES);
        ++pos;
        break;
      default:
        found_flag = false;
      }
    }
    section.flags = flags;

    // Parse width.
    section.min_width = 0;
    while (*pos >= L'0' && *pos <= L'9') {
      section.min_width =
          section.min_width * 10 + (static_cast<char>(*pos) - '0');
      ++pos;
    }

    // Parse modifier.
    char mc = static_cast<char>(*pos);
    if (mc == 'E') {
      section.modifier = strftime_core::ConvModifier::E;
      ++pos;
    } else if (mc == 'O') {
      section.modifier = strftime_core::ConvModifier::O;
      ++pos;
    } else {
      section.modifier = strftime_core::ConvModifier::none;
    }

    // Conversion letter.
    section.conv_name = static_cast<char>(*pos);
    if (*pos != L'\0')
      ++pos;

    // raw_string left empty -- converters for known specifiers don't read it,
    // and unknown specifiers produce no output (undefined per C11).

    result = strftime_core::convert(&writer, section, timeptr);
  }

  if (result < 0) {
    if (maxsize > 0)
      s[0] = L'\0';
    return 0;
  }

  size_t written = writer.get_chars_written();
  if (maxsize > 0)
    s[written < usable ? written : usable] = L'\0';
  return written >= maxsize ? 0 : written;
}

} // namespace LIBC_NAMESPACE_DECL
