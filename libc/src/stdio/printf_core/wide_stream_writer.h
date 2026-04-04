//===-- Wide Stream Writer for fwprintf --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Writes wide characters to a FILE* stream by dispatching through the
// stream's wide_write hook. Byte-backed streams fall through to the
// generic_wide_write bridge (wchar_t → mbstate-encoded bytes →
// write_unlocked_bytes); wide-native backends consume the wchar_t run
// directly without conversion. The stream must already be locked by the
// caller (vfwprintf_internal handles locking) and wide-oriented.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WIDE_STREAM_WRITER_H
#define LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WIDE_STREAM_WRITER_H

#include "hdr/types/wchar_t.h"
#include "src/__support/File/file.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/wide_io.h"
#include "src/stdio/printf_core/core_structs.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace printf_core {

class WideStreamWriter {
  File *file;
  size_t chars_written = 0;

  LIBC_INLINE int write_run(const wchar_t *src, size_t n) {
    auto result = internal::write_wide_unlocked(file, src, n);
    if (result.has_error() || result.value < n)
      return FILE_WRITE_ERROR;
    return WRITE_OK;
  }

public:
  LIBC_INLINE explicit WideStreamWriter(File *file) : file(file) {}

  LIBC_INLINE int write(wchar_t wc) {
    ++chars_written;
    return write_run(&wc, 1);
  }

  LIBC_INLINE int write(wchar_t wc, size_t count) {
    chars_written += count;
    // Emit the repeated character in chunks via a small stack buffer so the
    // backend sees an n > 1 run — wide-native backends then grow once per
    // chunk, byte-backed backends amortize the push/pop loop.
    constexpr size_t CHUNK = 64;
    wchar_t buf[CHUNK];
    for (size_t i = 0; i < CHUNK; ++i)
      buf[i] = wc;
    while (count > 0) {
      size_t n = count < CHUNK ? count : CHUNK;
      int r = write_run(buf, n);
      if (r != WRITE_OK)
        return r;
      count -= n;
    }
    return WRITE_OK;
  }

  LIBC_INLINE int write_wide(const wchar_t *ws, size_t len) {
    chars_written += len;
    return write_run(ws, len);
  }

  LIBC_INLINE size_t get_chars_written() const { return chars_written; }
};

} // namespace printf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WIDE_STREAM_WRITER_H
