//===-- Wide Stream Reader for fwscanf --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Reads wide characters from a FILE* stream via its wide_read hook. Byte-
// backed streams fall through to generic_wide_read (mbstate-decoded bytes
// from the byte buffer); wide-native backends return wchar_t directly.
// Lookahead rollback uses File's first-class wide pushback slot — the
// decoded wchar_t round-trips at the wide layer, so wide_mbstate is
// untouched and the encoding model (UTF-8 or otherwise) never enters the
// scanf pipeline. Caller must hold the file lock (vfwscanf_internal
// handles locking) and must have adopted wide orientation.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_SCANF_CORE_WIDE_STREAM_READER_H
#define LLVM_LIBC_SRC_STDIO_SCANF_CORE_WIDE_STREAM_READER_H

#include "hdr/types/wchar_t.h"
#include "src/__support/File/file.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/wide_io.h"
#include "src/stdio/scanf_core/reader.h"

namespace LIBC_NAMESPACE_DECL {
namespace scanf_core {

class WideStreamReader : public Reader<WideStreamReader, wchar_t> {
  File *file;

  // Read one wchar_t from the stream. Returns L'\0' on EOF or error.
  LIBC_INLINE wchar_t read_one() {
    wchar_t wc;
    auto result = internal::read_wide_unlocked(file, &wc, 1);
    if (result.has_error() || result.value < 1)
      return L'\0';
    return wc;
  }

public:
  LIBC_INLINE explicit WideStreamReader(File *file) : file(file) {}

  LIBC_INLINE wchar_t getc() { return read_one(); }

  LIBC_INLINE void ungetc(wchar_t wc) {
    // L'\0' is the reader's EOF sentinel (see read_one); never push it back.
    // Otherwise hand the wchar_t directly to File's wide pushback slot —
    // the next read_wide_unlocked pops it before decoding any new bytes.
    if (wc == L'\0')
      return;
    file->push_wide_char(wc);
  }
};

} // namespace scanf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_SCANF_CORE_WIDE_STREAM_READER_H
