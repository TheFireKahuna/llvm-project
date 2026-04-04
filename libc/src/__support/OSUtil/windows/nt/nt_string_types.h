//===-- NT string conversion, NLS, and Unicode helpers -------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_STRING_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_STRING_TYPES_H

#include "src/__support/CPP/limits.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/nt/nt_wchar_converter.h"
#include "src/__support/OSUtil/windows/nt/nt_wstring_view.h"
#include "src/__support/wchar/utf16_utils.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Prefer nt_wstring_view(s).size() for new code. This wrapper is retained for
// callers that need SIZE_T return type compatibility.
inline SIZE_T wide_string_length(PCWCH s) {
  return static_cast<SIZE_T>(nt_wstring_view(s).size());
}

//===----------------------------------------------------------------------===//
// wchar_t → WCHAR (UTF-32 → UTF-16) conversion
//
// WCHAR is always 16-bit (Win32 ABI). With llvm-libc, wchar_t is 32-bit
// (UTF-32, signed int). This helper bridges the two at NT API boundaries.
//
// Returns the number of WCHAR units written (excluding NUL), or -1 on
// error. When dst is nullptr, returns the required count (dry run).
// Errors: negative wchar_t, surrogate codepoint, out-of-range (>U+10FFFF),
// or destination buffer too small.
//===----------------------------------------------------------------------===//
inline int wchar_to_utf16_n(const wchar_t *src, SIZE_T src_len, WCHAR *dst,
                            SIZE_T dst_cap) {
  SIZE_T out = 0;
  for (SIZE_T i = 0; i < src_len; ++i) {
    // wchar_t is signed (int) with -fsigned-wchar. Reject negative values.
    if (src[i] < 0)
      return -1;

    // Delegate scalar encoding to the portable UTF-16 primitive.
    wchar_t units[2];
    auto result =
        internal::encode_wchar_scalar(static_cast<char32_t>(src[i]), units);
    if (!result.has_value())
      return -1;
    size_t n = result.value();

    if (dst) {
      if (out + n > dst_cap)
        return -1;
      for (size_t j = 0; j < n; ++j)
        dst[out + j] = static_cast<WCHAR>(units[j]);
    }
    out += n;
  }
  if (dst && out < dst_cap)
    dst[out] = u'\0';
  return out > static_cast<SIZE_T>(cpp::numeric_limits<int>::max())
             ? -1
             : static_cast<int>(out);
}

inline int utf8_to_wide_n_len(const char *src, SIZE_T src_len) {
  int n = utf8_to_utf16(src, src_len, nullptr, 0);
  return n < 0 ? -1 : n;
}

inline int utf8_to_wide_len(LIBC_NAMESPACE::cpp::string_view src) {
  int wide_chars = utf8_to_wide_n_len(src.data(), src.size());
  return wide_chars < 0 ? 0 : wide_chars + 1;
}

inline int utf8_to_wide_n(const char *src, SIZE_T src_len, WCHAR *dst,
                          SIZE_T dst_len) {
  int n = utf8_to_utf16(src, src_len, dst, dst_len);
  return n < 0 ? -1 : n;
}

inline int utf8_to_wide(LIBC_NAMESPACE::cpp::string_view src, WCHAR *dst,
                        int dst_len) {
  if (dst_len <= 0)
    return 0;

  int wide_chars =
      utf8_to_wide_n(src.data(), src.size(), dst,
                     static_cast<SIZE_T>(dst_len - 1));
  if (wide_chars < 0)
    return 0;

  dst[wide_chars] = u'\0';
  return wide_chars + 1;
}

inline int wide_to_utf8_n_len(PCWCH src, SIZE_T src_len) {
  int n = utf16_to_utf8(src, src_len, nullptr, 0);
  return n < 0 ? -1 : n;
}

inline int wide_to_utf8_len(PCWCH src) {
  int utf8_bytes = wide_to_utf8_n_len(src, wide_string_length(src));
  return utf8_bytes < 0 ? 0 : utf8_bytes + 1;
}

inline int wide_to_utf8_n(PCWCH src, SIZE_T src_len, char *dst,
                          SIZE_T dst_len) {
  int n = utf16_to_utf8(src, src_len, dst, dst_len);
  return n < 0 ? -1 : n;
}

inline int wide_to_utf8(PCWCH src, char *dst, int dst_len) {
  if (dst_len <= 0)
    return 0;

  int utf8_bytes =
      wide_to_utf8_n(src, wide_string_length(src), dst,
                     static_cast<SIZE_T>(dst_len - 1));
  if (utf8_bytes < 0)
    return 0;

  dst[utf8_bytes] = '\0';
  return utf8_bytes + 1;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_STRING_TYPES_H
