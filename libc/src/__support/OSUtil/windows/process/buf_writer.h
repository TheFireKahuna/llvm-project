//===-- Buffer packing utility for struct passwd/group -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A small bump allocator that packs multiple NUL-terminated strings (and
// aligned pointer arrays) into a flat char buffer. Returns interior pointers
// for populating struct passwd / struct group fields.
//
// Used by passwd_ops.cpp and group_ops.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_BUF_WRITER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_BUF_WRITER_H

#include "src/__support/OSUtil/windows/nt/nt_string_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/string/string_utils.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct BufWriter {
  char *buf;
  size_t cap;
  size_t pos;

  LIBC_INLINE BufWriter(char *b, size_t c) : buf(b), cap(c), pos(0) {}

  // Write a C string. Returns pointer to the copy, or nullptr on overflow.
  LIBC_INLINE char *put_str(const char *s) {
    size_t len = internal::string_length(s);
    if (pos + len + 1 > cap)
      return nullptr;
    char *start = buf + pos;
    __builtin_memcpy(start, s, len + 1); // includes NUL
    pos += len + 1;
    return start;
  }

  // Write a wide string as UTF-8. Optionally normalize backslashes to
  // forward slashes. Returns pointer to the copy, or nullptr on overflow.
  LIBC_INLINE char *put_wide(const WCHAR *src, size_t src_len,
                             bool normalize_path = false) {
    if (pos >= cap)
      return nullptr;
    char *start = buf + pos;
    size_t remaining = cap - pos;
    if (remaining < 2)
      return nullptr;

    int utf8_bytes =
        windows::wide_to_utf8_n(src, src_len, start, remaining - 1);
    if (utf8_bytes < 0)
      return nullptr;

    if (normalize_path) {
      for (int i = 0; i < utf8_bytes; ++i) {
        if (start[i] == '\\')
          start[i] = '/';
      }
    }

    start[utf8_bytes] = '\0';
    pos += static_cast<size_t>(utf8_bytes) + 1;
    return start;
  }

  // Reserve space for a pointer array (aligned). Returns pointer or nullptr.
  LIBC_INLINE char **put_ptr_array(size_t count) {
    size_t align = alignof(char *);
    size_t aligned_pos = (pos + align - 1) & ~(align - 1);
    size_t needed = count * sizeof(char *);
    if (aligned_pos + needed > cap)
      return nullptr;
    auto *result = reinterpret_cast<char **>(buf + aligned_pos);
    pos = aligned_pos + needed;
    return result;
  }
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_BUF_WRITER_H
