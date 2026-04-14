//===-- Wide Writer for wprintf ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WIDE_WRITER_H
#define LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WIDE_WRITER_H

#include "hdr/types/wchar_t.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/stdio/printf_core/core_structs.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace printf_core {

// Buffer that writes wchar_t and silently drops overflow. Used by swprintf.
// buff_len is the total buffer size including the slot reserved for the null
// terminator, so at most buff_len - 1 data characters can be stored.
class WideWriter {
  wchar_t *buff;
  size_t buff_len; // total buffer size (includes null terminator slot)
  size_t buff_cur = 0;
  size_t chars_written = 0;

  // Maximum number of data characters that can be stored.
  LIBC_INLINE size_t max_data() const {
    return buff_len > 0 ? buff_len - 1 : 0;
  }

public:
  LIBC_INLINE WideWriter(wchar_t *buff, size_t buff_len)
      : buff(buff), buff_len(buff_len) {}

  // Write a single wide character.
  LIBC_INLINE int write(wchar_t wc) {
    ++chars_written;
    if (LIBC_LIKELY(buff_cur < max_data())) {
      buff[buff_cur] = wc;
      ++buff_cur;
    }
    return WRITE_OK;
  }

  // Fill with a repeated wide character.
  LIBC_INLINE int write(wchar_t wc, size_t count) {
    chars_written += count;
    size_t limit = max_data();
    for (; count > 0 && buff_cur < limit; --count, ++buff_cur)
      buff[buff_cur] = wc;
    return WRITE_OK;
  }

  // Write a wide string.
  LIBC_INLINE int write_wide(const wchar_t *ws, size_t len) {
    chars_written += len;
    size_t limit = max_data();
    for (size_t i = 0; i < len && buff_cur < limit; ++i, ++buff_cur)
      buff[buff_cur] = ws[i];
    return WRITE_OK;
  }

  // Null-terminate the buffer. buff_cur is always <= max_data(), so
  // buff[buff_cur] is always within bounds when buff_len > 0.
  LIBC_INLINE void null_terminate() {
    if (buff_len > 0)
      buff[buff_cur] = L'\0';
  }

  LIBC_INLINE size_t get_chars_written() const { return chars_written; }
  LIBC_INLINE size_t get_buff_cur() const { return buff_cur; }
};

} // namespace printf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WIDE_WRITER_H
