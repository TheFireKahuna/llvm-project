//===-- Core Structures for scanf ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_SCANF_CORE_CORE_STRUCTS_H
#define LLVM_LIBC_SRC_STDIO_SCANF_CORE_CORE_STRUCTS_H

#include "src/__support/CPP/bitset.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <inttypes.h>
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace scanf_core {

// These length modifiers match the length modifiers in the format string, which
// is why they are formatted differently from the rest of the file. Backed by
// uint8_t so FormatDesc packs without padding — 9 values, trivially fits.
enum class LengthModifier : uint8_t { hh, h, l, ll, j, z, t, L, NONE };

enum FormatFlags : uint8_t {
  NONE = 0x00,
  NO_WRITE = 0x01, // *
  ALLOCATE = 0x02, // m
};

// Range-list bracket-set storage for wide %[. Mirrors the role of
// cpp::bitset<256> in the narrow FormatSection. A bitset is not feasible for
// 32-bit wchar_t, so membership is tested by linear scan of inclusive ranges.
// Bracket sets are small in practice (MAX_RANGES == 32). Declared here so the
// ScanSetFor trait below can name it; the narrow path never instantiates it.
struct WideScanSet {
  struct Range {
    wchar_t lo, hi;
  };
  static constexpr size_t MAX_RANGES = 32;
  Range ranges[MAX_RANGES];
  size_t count = 0;
  bool inverted = false;

  LIBC_INLINE void set(wchar_t c) {
    if (count < MAX_RANGES)
      ranges[count++] = {c, c};
  }

  LIBC_INLINE void set_range(wchar_t lo, wchar_t hi) {
    wchar_t a = lo < hi ? lo : hi;
    wchar_t b = lo < hi ? hi : lo;
    if (count < MAX_RANGES)
      ranges[count++] = {a, b};
  }

  LIBC_INLINE bool test(wchar_t c) const {
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
      if (c >= ranges[i].lo && c <= ranges[i].hi) {
        found = true;
        break;
      }
    }
    return inverted ? !found : found;
  }

  LIBC_INLINE bool operator==(const WideScanSet &other) const {
    if (count != other.count || inverted != other.inverted)
      return false;
    for (size_t i = 0; i < count; ++i)
      if (ranges[i].lo != other.ranges[i].lo ||
          ranges[i].hi != other.ranges[i].hi)
        return false;
    return true;
  }
};

// Char-agnostic descriptor fields shared by narrow and wide sections. Field
// order clusters the four 1-byte fields at the front so FormatDesc packs to
// exactly 16 B with zero padding — basic_format_section<char> lands on a
// single 64 B cache line (16 B desc + 16 B raw_string + 32 B scan_set).
struct FormatDesc {
  bool has_conv;
  FormatFlags flags = FormatFlags::NONE;
  LengthModifier length_modifier = LengthModifier::NONE;
  char conv_name;

  int max_width = -1;

  // output_ptr is nullptr if and only if the NO_WRITE flag is set.
  void *output_ptr = nullptr;
};

// Trait picking the bracket-set storage for a given CharT. Narrow uses a
// dense bitset (256 bits covers all unsigned char values); wide uses a
// range list (bitset size for 32-bit wchar_t would be infeasible).
template <typename CharT> struct ScanSetFor;
template <> struct ScanSetFor<char> {
  using type = cpp::bitset<256>;
};
template <> struct ScanSetFor<wchar_t> {
  using type = WideScanSet;
};

// Format section parameterized on CharT. Narrow and wide differ in the
// raw-literal storage (basic_string_view<CharT>) and the bracket-set storage
// (ScanSetFor<CharT>::type); descriptor fields live in the shared FormatDesc
// base. Mirrors the std::basic_string / basic_string_view pattern.
template <typename CharT> struct basic_format_section : FormatDesc {
  cpp::basic_string_view<CharT> raw_string;
  typename ScanSetFor<CharT>::type scan_set;

  LIBC_INLINE bool operator==(const basic_format_section &other) const {
    if (has_conv != other.has_conv)
      return false;

    if (raw_string != other.raw_string)
      return false;

    if (has_conv) {
      if (!((static_cast<uint8_t>(flags) ==
             static_cast<uint8_t>(other.flags)) &&
            (max_width == other.max_width) &&
            (length_modifier == other.length_modifier) &&
            (conv_name == other.conv_name)))
        return false;

      // If the pointers are used, then they should be equal. If the NO_WRITE
      // flag is set or the conversion is %, then the pointers are not used.
      // If the pointers are used and they are not equal, return false.

      if (!(((flags & FormatFlags::NO_WRITE) != 0) || (conv_name == '%') ||
            (output_ptr == other.output_ptr)))
        return false;

      if (conv_name == '[')
        return scan_set == other.scan_set;
    }
    return true;
  }
};

using FormatSection = basic_format_section<char>;
using WideFormatSection = basic_format_section<wchar_t>;

enum ErrorCodes : int {
  // This is the value to be returned by conversions when no error has occurred.
  READ_OK = 0,
  // These are the scanf return values for when an error has occurred. They are
  // all negative, and should be distinct.
  FILE_READ_ERROR = -1,
  FILE_STATUS_ERROR = -2,
  MATCHING_FAILURE = -3,
  ALLOCATION_FAILURE = -4,
};
} // namespace scanf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_SCANF_CORE_CORE_STRUCTS_H
