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
#include "src/__support/macros/config.h"

#include "hdr/types/wchar_t.h"
#include <inttypes.h>
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace scanf_core {

// These length modifiers match the length modifiers in the format string, which
// is why they are formatted differently from the rest of the file.
enum class LengthModifier { hh, h, l, ll, j, z, t, L, NONE };

enum FormatFlags : uint8_t {
  NONE = 0x00,
  NO_WRITE = 0x01, // *
  ALLOCATE = 0x02, // m
};

// Scan set for wide %[ conversions. Stores up to MAX_RANGES inclusive
// character ranges. Membership tested by linear scan — bracket sets are
// small in practice.
struct WideScanSet {
  struct Range {
    wchar_t lo, hi;
  };
  static constexpr size_t MAX_RANGES = 32;
  Range ranges[MAX_RANGES];
  size_t count = 0;
  bool inverted = false;

  LIBC_INLINE void add(wchar_t c) {
    if (count < MAX_RANGES)
      ranges[count++] = {c, c};
  }

  LIBC_INLINE void add_range(wchar_t lo, wchar_t hi) {
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
};

struct FormatSection {
  bool has_conv;

  // Type-erased pointer to the raw format string segment. Cast to the
  // appropriate CharType (char or wchar_t) in templated call sites.
  const void *raw_begin = nullptr;
  size_t raw_len = 0;

  // Format Specifier Values
  FormatFlags flags = FormatFlags::NONE;
  LengthModifier length_modifier = LengthModifier::NONE;
  int max_width = -1;

  // output_ptr is nullptr if and only if the NO_WRITE flag is set.
  void *output_ptr = nullptr;

  char conv_name;

  cpp::bitset<256> scan_set;
  WideScanSet wide_scan_set;

  LIBC_INLINE bool operator==(const FormatSection &other) {
    if (has_conv != other.has_conv)
      return false;

    if (raw_len != other.raw_len || raw_begin != other.raw_begin)
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
