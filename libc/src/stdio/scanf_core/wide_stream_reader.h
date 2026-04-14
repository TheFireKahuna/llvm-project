//===-- Wide Stream Reader for fwscanf --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Reads wide characters from a FILE* stream via the shared
// read_wchar_unlocked path. The stream must already be locked by the caller
// (vfwscanf_internal handles locking).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_SCANF_CORE_WIDE_STREAM_READER_H
#define LLVM_LIBC_SRC_STDIO_SCANF_CORE_WIDE_STREAM_READER_H

#include "hdr/types/wchar_t.h"
#include "src/__support/File/file.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/read_wchar.h"
#include "src/__support/wchar/wcrtomb.h"
#include "src/stdio/scanf_core/reader.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace scanf_core {

// Stream-based wide reader for fwscanf/wscanf. Delegates wide decoding to the
// shared read helper, but performs scanf rollback through byte-level ungetc so
// the File remains the single source of truth for stream position. Caller must
// hold the file lock.
class WideStreamReader : public Reader<WideStreamReader, wchar_t> {
  File *file;

  // Read one wchar_t from the stream. Returns L'\0' on EOF or error.
  // read_wchar_unlocked handles stream state, decoding, and wide pushback.
  LIBC_INLINE wchar_t read_one() {
    wint_t wc = internal::read_wchar_unlocked(file);
    return wc == WEOF ? L'\0' : static_cast<wchar_t>(wc);
  }

public:
  LIBC_INLINE explicit WideStreamReader(File *file) : file(file) {}

  LIBC_INLINE wchar_t getc() { return read_one(); }

  LIBC_INLINE void ungetc(wchar_t wc) {
    // Scanf lookahead must roll back the underlying byte stream so ftell/fseek
    // observe the unread character. Encode the wchar_t back to bytes and push
    // them in reverse order, matching the order they were consumed.
    if (wc == L'\0')
      return;

    char bytes[4];
    internal::mbstate state{};
    auto encoded_size = internal::wcrtomb(bytes, wc, &state);
    if (!encoded_size.has_value())
      return;

    for (size_t i = encoded_size.value(); i > 0; --i) {
      if (file->ungetc_unlocked(
              static_cast<unsigned char>(bytes[i - 1])) == EOF)
        break;
    }
  }
};

} // namespace scanf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_SCANF_CORE_WIDE_STREAM_READER_H
