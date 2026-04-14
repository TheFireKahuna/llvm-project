//===-- Implementation of strcoll -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/string/strcoll.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/null_check.h"

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/locale/locale.h"
#include "src/stdlib/free.h"
#include "src/stdlib/malloc.h"
#endif

namespace LIBC_NAMESPACE_DECL {

#ifdef LIBC_TARGET_OS_IS_WINDOWS

// Locale-aware string comparison using RtlCompareUnicodeStrings.
// Converts both UTF-8 strings to UTF-16, then delegates to ntdll.
// For the C locale, falls back to byte comparison (per POSIX: "In the
// POSIX locale, strcoll() shall be equivalent to strcmp()").
static int strcoll_impl(const char *left, const char *right,
                        locale_t loc) {
  // C locale → byte comparison (POSIX requirement).
  if (!loc || loc == &c_locale) {
    for (; *left && *left == *right; ++left, ++right)
      ;
    return static_cast<int>(static_cast<unsigned char>(*left)) -
           static_cast<int>(static_cast<unsigned char>(*right));
  }

  // Convert both strings to UTF-16 for Unicode-aware comparison.
  // Stack buffers for typical short strings; the Rtl conversion functions
  // handle the sizing.
  SIZE_T left_len = __builtin_strlen(left);
  SIZE_T right_len = __builtin_strlen(right);

  // Query required wide char counts.
  int left_wchars = windows::utf8_to_wide_n_len(left, left_len);
  int right_wchars = windows::utf8_to_wide_n_len(right, right_len);

  if (left_wchars < 0 || right_wchars < 0) {
    // Invalid UTF-8 — fall back to byte comparison.
    for (; *left && *left == *right; ++left, ++right)
      ;
    return static_cast<int>(static_cast<unsigned char>(*left)) -
           static_cast<int>(static_cast<unsigned char>(*right));
  }

  // Use stack buffers for small strings, heap for large.
  constexpr int kStackChars = 256;
  WCHAR left_stack[kStackChars];
  WCHAR right_stack[kStackChars];

  WCHAR *left_wide = left_stack;
  WCHAR *right_wide = right_stack;

  // For large strings, allocate via malloc (SlabPool-backed).
  bool left_heap = left_wchars > kStackChars;
  bool right_heap = right_wchars > kStackChars;

  if (left_heap) {
    left_wide = static_cast<WCHAR *>(
        LIBC_NAMESPACE::malloc(static_cast<size_t>(left_wchars) * sizeof(WCHAR)));
    if (!left_wide)
      return -1; // OOM — arbitrary but deterministic.
  }
  if (right_heap) {
    right_wide = static_cast<WCHAR *>(LIBC_NAMESPACE::malloc(
        static_cast<size_t>(right_wchars) * sizeof(WCHAR)));
    if (!right_wide) {
      if (left_heap)
        LIBC_NAMESPACE::free(left_wide);
      return 1;
    }
  }

  windows::utf8_to_wide_n(left, left_len, left_wide, left_wchars);
  windows::utf8_to_wide_n(right, right_len, right_wide, right_wchars);

  LONG result = ::RtlCompareUnicodeStrings(
      left_wide, static_cast<SIZE_T>(left_wchars), right_wide,
      static_cast<SIZE_T>(right_wchars),
      /*CaseInSensitive=*/false);

  if (left_heap)
    LIBC_NAMESPACE::free(left_wide);
  if (right_heap)
    LIBC_NAMESPACE::free(right_wide);

  return static_cast<int>(result);
}

LLVM_LIBC_FUNCTION(int, strcoll, (const char *left, const char *right)) {
  LIBC_CRASH_ON_NULLPTR(left);
  LIBC_CRASH_ON_NULLPTR(right);
  return strcoll_impl(left, right, get_current_locale());
}

#else // !LIBC_TARGET_OS_IS_WINDOWS

LLVM_LIBC_FUNCTION(int, strcoll, (const char *left, const char *right)) {
  LIBC_CRASH_ON_NULLPTR(left);
  LIBC_CRASH_ON_NULLPTR(right);
  // C locale: byte comparison (POSIX).
  for (; *left && *left == *right; ++left, ++right)
    ;
  return static_cast<unsigned char>(*left) - static_cast<unsigned char>(*right);
}

#endif // LIBC_TARGET_OS_IS_WINDOWS

} // namespace LIBC_NAMESPACE_DECL
