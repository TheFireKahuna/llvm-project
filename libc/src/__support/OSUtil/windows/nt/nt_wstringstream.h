//===-- NT WCHAR StringStream implementation ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A span-backed stream for building WCHAR (16-bit UTF-16LE) strings used by
// NT kernel APIs. This is NT ABI infrastructure, NOT POSIX wchar_t support.
//
// 1:1 API mirror of cpp::StringStream, operating on WCHAR instead of char.
// Supports operator<< for nt_wstring_view, const WCHAR*, WCHAR, and integers.
// Integer formatting uses IntegerToString internally and widens the ASCII
// digit output to WCHAR (lossless — all digits/hex chars are in [0,127]).
//
// Zero-allocation, async-signal-safe.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_NT_WSTRINGSTREAM_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_NT_WSTRINGSTREAM_H

#include "nt_wstring_view.h"

#include "src/__support/CPP/span.h"
#include "src/__support/CPP/type_traits.h"
#include "src/__support/integer_to_string.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Writes WCHAR characters into a user-provided buffer without any dynamic
// memory allocation. Mirrors cpp::StringStream for the WCHAR domain.
class WStringStream {
  cpp::span<WCHAR> data;
  size_t write_ptr = 0;
  bool err = false;

  void write(const WCHAR *chars, size_t size) {
    size_t i = 0;
    const size_t data_size = data.size();
    for (; write_ptr < data_size && i < size; ++i, ++write_ptr)
      data[write_ptr] = chars[i];
    if (i < size)
      err = true;
  }

  // Widen ASCII chars from IntegerToString output to WCHAR.
  // All integer digits (0-9, a-f, A-F, +, -, 0x prefix) are ASCII,
  // so static_cast<WCHAR>(c) is exact and lossless.
  void write_narrow(const char *chars, size_t size) {
    size_t i = 0;
    const size_t data_size = data.size();
    for (; write_ptr < data_size && i < size; ++i, ++write_ptr)
      data[write_ptr] = static_cast<WCHAR>(chars[i]);
    if (i < size)
      err = true;
  }

public:
  static constexpr WCHAR ENDS = u'\0';

  constexpr WStringStream(const cpp::span<WCHAR> &buf) : data(buf) {}

  // Return a nt_wstring_view to the current characters in the stream.
  nt_wstring_view str() const { return nt_wstring_view(data.data(), write_ptr); }

  // Write wide characters from a nt_wstring_view.
  WStringStream &operator<<(nt_wstring_view str) {
    write(str.data(), str.size());
    return *this;
  }

  // Write a NUL-terminated wide string (the NUL is not written).
  WStringStream &operator<<(const WCHAR *str) {
    return operator<<(nt_wstring_view(str));
  }

  // Write a single wide character.
  WStringStream &operator<<(WCHAR c) {
    write(&c, 1);
    return *this;
  }

  // Write an integer value as its decimal string representation.
  // Uses IntegerToString to format into a char buffer, then widens to WCHAR.
  template <typename T, cpp::enable_if_t<cpp::is_integral_v<T>, int> = 0>
  WStringStream &operator<<(T val) {
    const IntegerToString<T> buffer(val);
    cpp::string_view sv = buffer.view();
    write_narrow(sv.data(), sv.size());
    return *this;
  }

  // Write an integer with a specific radix format (e.g. radix::Hex::WithPrefix).
  // Usage: ss.write_int<radix::Hex::WithPrefix>(value);
  template <typename Fmt, typename T>
  WStringStream &write_int(T val) {
    const IntegerToString<T, Fmt> buffer(val);
    cpp::string_view sv = buffer.view();
    write_narrow(sv.data(), sv.size());
    return *this;
  }

  // Return true if any write operation(s) failed due to insufficient size.
  bool overflow() const { return err; }

  size_t bufsize() const { return data.size(); }

  // NUL-terminate the buffer at the current write position.
  // Returns false if there is no room for the terminator.
  bool null_terminate() {
    if (write_ptr < data.size()) {
      data[write_ptr] = u'\0';
      return true;
    }
    err = true;
    return false;
  }
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_NT_WSTRINGSTREAM_H
