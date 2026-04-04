//===-- NT WCHAR string_view implementation -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A string_view over WCHAR (unsigned short, 16-bit UTF-16LE) for NT kernel
// API string building. This is NT ABI infrastructure, NOT POSIX wchar_t
// support — WCHAR is always 16-bit regardless of the compiler's wchar_t.
//
// 1:1 API mirror of cpp::string_view, operating on WCHAR instead of char.
//
// Internal representation is UNICODE_STRING — the NT kernel's native counted
// wide string. This makes nt_nt_wstring_view layout-equivalent to UNICODE_STRING,
// enabling zero-cost bridging to any Nt* API that takes PUNICODE_STRING or
// PCUNICODE_STRING. Call unicode_string() to get a pointer to the underlying
// NT struct without any conversion or copy.
//
// Naming note: the `nt_` prefix is load-bearing. A plain `nt_wstring_view` in
// this layer would collide with `cpp::nt_wstring_view` (platform `wchar_t`, 32
// bits on NTPOSIX) and hide the ABI boundary. See
// `libc/src/__support/CPP/string_view.h` for the parallel `cpp::w16string_view`
// — that alias is for pure 16-bit code-unit views that do NOT need to bridge
// to the NT ABI; this type is strictly for UNICODE_STRING-adjacent work.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_NT_WSTRING_VIEW_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_NT_WSTRING_VIEW_H

#include "src/__support/CPP/limits.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include <stddef.h>
#include "include/sys/ntabi.h" // WCHAR, UNICODE_STRING, PCUNICODE_STRING

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Non-owning view over a contiguous sequence of WCHAR. Mirrors cpp::string_view
// but for NT's 16-bit wide character type. No bounds checks — callers are
// expected to validate before invoking methods.
//
// Internally a UNICODE_STRING, so zero-cost conversion to the NT ABI is
// available via unicode_string(). The USHORT byte-count fields limit views
// to 32767 WCHARs (65534 bytes) — this matches the NT kernel's own limit
// for UNICODE_STRING and is sufficient for all NT paths and object names.
class nt_wstring_view {
private:
  // The sole data member. Layout: {USHORT Length, USHORT MaximumLength,
  // WCHAR *Buffer} — 16 bytes on x64 (4 bytes of struct padding between
  // MaximumLength and Buffer).
  UNICODE_STRING us_;

  LIBC_INLINE static constexpr size_t min(size_t A, size_t B) {
    return A <= B ? A : B;
  }

  LIBC_INLINE static constexpr int compareMemory(const WCHAR *Lhs,
                                                  const WCHAR *Rhs,
                                                  size_t Length) {
    for (size_t i = 0; i < Length; ++i)
      if (int Diff = static_cast<int>(Lhs[i]) - static_cast<int>(Rhs[i]))
        return Diff;
    return 0;
  }

  LIBC_INLINE static constexpr size_t length(const WCHAR *Str) {
    for (const WCHAR *End = Str;; ++End)
      if (*End == u'\0')
        return static_cast<size_t>(End - Str);
  }

  LIBC_INLINE constexpr bool equals(nt_wstring_view Other) const {
    return (size() == Other.size() &&
            compareMemory(data(), Other.data(), Other.size()) == 0);
  }

  // Convert a WCHAR count to a byte count for UNICODE_STRING fields.
  LIBC_INLINE static constexpr USHORT to_bytes(size_t wchar_count) {
    return static_cast<USHORT>(wchar_count * sizeof(WCHAR));
  }

public:
  using value_type = WCHAR;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using pointer = WCHAR *;
  using const_pointer = const WCHAR *;
  using reference = WCHAR &;
  using const_reference = const WCHAR &;
  using const_iterator = const WCHAR *;
  using iterator = const_iterator;

  LIBC_INLINE_VAR static constexpr size_t npos =
      cpp::numeric_limits<size_t>::max();

  LIBC_INLINE constexpr nt_wstring_view() : us_{0, 0, nullptr} {}

  // Assumes Str is a NUL-terminated wide string.
  LIBC_INLINE constexpr nt_wstring_view(const WCHAR *Str)
      : us_{to_bytes(length(Str)),
            to_bytes(length(Str) + 1),
            const_cast<WCHAR *>(Str)} {}

  LIBC_INLINE constexpr nt_wstring_view(const WCHAR *Str, size_t N)
      : us_{to_bytes(N),
            to_bytes(N + 1),
            const_cast<WCHAR *>(Str)} {}

  // Construct directly from an existing UNICODE_STRING.
  LIBC_INLINE constexpr nt_wstring_view(const UNICODE_STRING &us)
      : us_{us.Length, us.MaximumLength, us.Buffer} {}

  //===--------------------------------------------------------------------===//
  // C++ string_view interface
  //===--------------------------------------------------------------------===//

  LIBC_INLINE constexpr const WCHAR *data() const { return us_.Buffer; }
  LIBC_INLINE constexpr size_t size() const {
    return us_.Length / sizeof(WCHAR);
  }
  LIBC_INLINE constexpr bool empty() const { return us_.Length == 0; }

  LIBC_INLINE constexpr const WCHAR *begin() const { return us_.Buffer; }
  LIBC_INLINE constexpr const WCHAR *end() const {
    return us_.Buffer + size();
  }

  LIBC_INLINE constexpr const WCHAR &operator[](size_t Index) const {
    return us_.Buffer[Index];
  }

  LIBC_INLINE constexpr int compare(nt_wstring_view Other) const {
    if (int Res = compareMemory(data(), Other.data(),
                                min(size(), Other.size())))
      return Res < 0 ? -1 : 1;
    if (size() == Other.size())
      return 0;
    return size() < Other.size() ? -1 : 1;
  }

  LIBC_INLINE constexpr bool operator==(nt_wstring_view Other) const {
    return equals(Other);
  }
  LIBC_INLINE constexpr bool operator!=(nt_wstring_view Other) const {
    return !(*this == Other);
  }
  LIBC_INLINE constexpr bool operator<(nt_wstring_view Other) const {
    return compare(Other) == -1;
  }
  LIBC_INLINE constexpr bool operator<=(nt_wstring_view Other) const {
    return compare(Other) != 1;
  }
  LIBC_INLINE constexpr bool operator>(nt_wstring_view Other) const {
    return compare(Other) == 1;
  }
  LIBC_INLINE constexpr bool operator>=(nt_wstring_view Other) const {
    return compare(Other) != -1;
  }

  LIBC_INLINE constexpr void remove_prefix(size_t N) {
    us_.Buffer += N;
    us_.Length -= to_bytes(N);
    us_.MaximumLength -= to_bytes(N);
  }

  LIBC_INLINE constexpr void remove_suffix(size_t N) {
    us_.Length -= to_bytes(N);
    us_.MaximumLength -= to_bytes(N);
  }

  LIBC_INLINE constexpr bool starts_with(nt_wstring_view Prefix) const {
    return size() >= Prefix.size() &&
           compareMemory(data(), Prefix.data(), Prefix.size()) == 0;
  }

  LIBC_INLINE constexpr bool starts_with(WCHAR Prefix) const {
    return !empty() && front() == Prefix;
  }

  LIBC_INLINE constexpr bool ends_with(WCHAR Suffix) const {
    return !empty() && back() == Suffix;
  }

  LIBC_INLINE constexpr bool ends_with(nt_wstring_view Suffix) const {
    return size() >= Suffix.size() &&
           compareMemory(end() - Suffix.size(), Suffix.data(),
                         Suffix.size()) == 0;
  }

  LIBC_INLINE constexpr nt_wstring_view substr(size_t Start,
                                            size_t N = npos) const {
    Start = min(Start, size());
    return nt_wstring_view(data() + Start, min(N, size() - Start));
  }

  LIBC_INLINE constexpr WCHAR front() const { return us_.Buffer[0]; }
  LIBC_INLINE constexpr WCHAR back() const {
    return us_.Buffer[size() - 1];
  }

  LIBC_INLINE constexpr size_t find_first_of(WCHAR c,
                                             size_t From = 0) const {
    for (size_t Pos = From; Pos < size(); ++Pos)
      if ((*this)[Pos] == c)
        return Pos;
    return npos;
  }

  LIBC_INLINE constexpr size_t find_last_of(WCHAR c,
                                            size_t End = npos) const {
    End = End >= size() ? size() : End + 1;
    for (; End > 0; --End)
      if ((*this)[End - 1] == c)
        return End - 1;
    return npos;
  }

  LIBC_INLINE constexpr size_t find_first_not_of(WCHAR c,
                                                 size_t From = 0) const {
    for (size_t Pos = From; Pos < size(); ++Pos)
      if ((*this)[Pos] != c)
        return Pos;
    return npos;
  }

  LIBC_INLINE constexpr bool contains(WCHAR c) const {
    return find_first_of(c) != npos;
  }

  // Byte count of the view's content (== UNICODE_STRING.Length).
  LIBC_INLINE constexpr USHORT byte_size() const { return us_.Length; }

  //===--------------------------------------------------------------------===//
  // NT ABI bridge — zero-cost access to the underlying UNICODE_STRING
  //===--------------------------------------------------------------------===//

  // Returns a pointer to the internal UNICODE_STRING for direct use with
  // Nt* APIs. The returned pointer is valid for the lifetime of this view.
  LIBC_INLINE PCUNICODE_STRING unicode_string() const { return &us_; }
  LIBC_INLINE PUNICODE_STRING unicode_string() { return &us_; }
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_NT_WSTRING_VIEW_H
