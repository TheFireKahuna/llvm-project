//===-- Internal read_wchar_unlocked helper ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Reads one wide character from a File stream without locking. Sets the
// stream's error indicator on encoding errors. The caller must hold the
// file lock and must have already set stream orientation.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_WCHAR_READ_WCHAR_H
#define LLVM_LIBC_SRC___SUPPORT_WCHAR_READ_WCHAR_H

#include "hdr/errno_macros.h"
#include "hdr/types/wint_t.h"
#include "hdr/wchar_macros.h"
#include "src/__support/File/file.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/character_converter.h"
#include "src/__support/wchar/locale_encoding.h"
#include "src/__support/wchar/mbstate.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Read one wide character from the stream (unlocked). Sets libc_errno on
// error. Sets the stream's error indicator on encoding errors. Returns
// WEOF on EOF or error.
LIBC_INLINE wint_t read_wchar_unlocked(File *f) {
  // Check wide pushback slot first.
  if (f->has_wide_unget())
    return static_cast<wint_t>(f->pop_wide_char());

  mbstate *state = f->get_wide_mbstate();
  CharacterConverter cr(state, locale_encoding_is_utf8());

  while (!cr.isFull()) {
    uint8_t byte;
    auto result = f->read_unlocked(&byte, 1);
    if (result.has_error()) {
      libc_errno = result.error;
      return WEOF;
    }
    if (result.value < 1) {
      // EOF. If we have a partial sequence, it's an encoding error.
      if (!cr.isEmpty()) {
        cr.clear();
        f->set_err_unlocked();
        libc_errno = EILSEQ;
        return WEOF;
      }
      // Clean EOF — eof flag already set by read_unlocked.
      return WEOF;
    }

    int err = cr.push(static_cast<char8_t>(byte));
    if (err != 0) {
      cr.clear();
      f->set_err_unlocked();
      libc_errno = err;
      return WEOF;
    }
  }

  auto codepoint = cr.pop_utf32();
  // pop_utf32 may reject overlong, surrogate, or out-of-range sequences.
  if (!codepoint.has_value()) {
    cr.clear();
    f->set_err_unlocked();
    libc_errno = EILSEQ;
    return WEOF;
  }

  return static_cast<wint_t>(static_cast<wchar_t>(codepoint.value()));
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_WCHAR_READ_WCHAR_H
