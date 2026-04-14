//===-- Wide Stream Writer for fwprintf --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Writes wide characters to a FILE* stream by converting each wchar_t to
// UTF-8 and calling write_unlocked. The stream must already be locked by
// the caller (vfwprintf_internal handles locking).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WIDE_STREAM_WRITER_H
#define LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WIDE_STREAM_WRITER_H

#include "hdr/types/wchar_t.h"
#include "src/__support/File/file.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/character_converter.h"
#include "src/__support/wchar/locale_encoding.h"
#include "src/__support/wchar/mbstate.h"
#include "src/stdio/printf_core/core_structs.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace printf_core {

// Stream-based wide writer for fwprintf/wprintf. Converts wchar_t to UTF-8
// and writes to the underlying File. Caller must hold the file lock.
class WideStreamWriter {
  File *file;
  size_t chars_written = 0;

  // Write a single wchar_t to the stream as UTF-8.
  LIBC_INLINE int write_one(wchar_t wc) {
    char32_t scalar = static_cast<char32_t>(wc);

    internal::mbstate *state = file->get_wide_mbstate();
    internal::CharacterConverter cr(state, internal::locale_encoding_is_utf8());

    int err = cr.push(scalar);
    if (err != 0)
      return FILE_WRITE_ERROR;

    while (!cr.isEmpty()) {
      auto utf8 = cr.pop_utf8();
      uint8_t byte = static_cast<uint8_t>(utf8.value());
      auto result = file->write_unlocked(&byte, 1);
      if (result.has_error() || result.value < 1)
        return FILE_WRITE_ERROR;
    }
    return WRITE_OK;
  }

public:
  LIBC_INLINE explicit WideStreamWriter(File *file) : file(file) {}

  // Write a single wide character.
  LIBC_INLINE int write(wchar_t wc) {
    ++chars_written;
    return write_one(wc);
  }

  // Fill with a repeated wide character.
  LIBC_INLINE int write(wchar_t wc, size_t count) {
    chars_written += count;
    for (size_t i = 0; i < count; ++i) {
      int r = write_one(wc);
      if (r != WRITE_OK)
        return r;
    }
    return WRITE_OK;
  }

  // Write a wide string.
  LIBC_INLINE int write_wide(const wchar_t *ws, size_t len) {
    chars_written += len;
    for (size_t i = 0; i < len; ++i) {
      int r = write_one(ws[i]);
      if (r != WRITE_OK)
        return r;
    }
    return WRITE_OK;
  }

  LIBC_INLINE size_t get_chars_written() const { return chars_written; }
};

} // namespace printf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WIDE_STREAM_WRITER_H
