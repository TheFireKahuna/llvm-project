//===-- Shared wide I/O helpers ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Factored wchar_t <-> File byte-stream glue shared by fputwc, fgetwc,
// fgetws, and the printf / scanf wide writers/readers. Keeps the
// CharacterConverter + wide_mbstate plumbing in one place so individual
// entrypoints stay thin.
//
// Conventions (matching File's byte-side write_unlocked / read_unlocked):
//   * Caller must hold the file lock.
//   * Caller must have adopted wide orientation before the first call.
//   * Returns FileIOResult{chars_transferred, errno}. A partial transfer
//     with a non-zero errno means progress stopped at that point. A clean
//     EOF on read returns {chars_read, 0} — the caller distinguishes by
//     checking f->iseof_unlocked().
//   * On encoding / I/O error the stream error indicator is set via
//     File::set_err_unlocked so ferror() reports consistently even if the
//     caller forgets to propagate the errno.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_WCHAR_WIDE_IO_H
#define LLVM_LIBC_SRC___SUPPORT_WCHAR_WIDE_IO_H

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "hdr/types/char32_t.h"
#include "hdr/types/char8_t.h"
#include "hdr/types/wchar_t.h"
#include "src/__support/File/file.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/character_converter.h"
#include "src/__support/wchar/locale_encoding.h"
#include "src/__support/wchar/mbstate.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Encode |n| wide characters through the stream's wide_mbstate and push
// the resulting bytes through File::write_unlocked. Stops at the first
// encoding or write error and reports how many wchars were fully emitted.
LIBC_INLINE FileIOResult write_wide_unlocked(File *f, const wchar_t *src,
                                             size_t n) {
  mbstate *state = f->get_wide_mbstate();
  CharacterConverter cr(state, locale_encoding_is_utf8());

  for (size_t i = 0; i < n; ++i) {
    int push_err = cr.push(static_cast<char32_t>(src[i]));
    if (push_err != 0) {
      f->set_err_unlocked();
      return {i, push_err};
    }
    while (!cr.isEmpty()) {
      uint8_t byte = static_cast<uint8_t>(cr.pop_utf8().value());
      auto r = f->write_unlocked(&byte, 1);
      if (r.has_error())
        return {i, r.error};
      if (r.value < 1)
        return {i, EIO};
    }
  }
  return {n, 0};
}

// Decode up to |n| wide characters from the stream. Consults the wide
// pushback slot first, then drains bytes from File::read_unlocked through
// the stream's wide_mbstate. A clean EOF returns the number of complete
// wchars read with errno=0; a partial multibyte sequence at EOF is an
// encoding error (EILSEQ).
LIBC_INLINE FileIOResult read_wide_unlocked(File *f, wchar_t *dst, size_t n) {
  mbstate *state = f->get_wide_mbstate();

  for (size_t i = 0; i < n; ++i) {
    if (f->has_wide_unget()) {
      dst[i] = f->pop_wide_char();
      continue;
    }

    CharacterConverter cr(state, locale_encoding_is_utf8());

    while (!cr.isFull()) {
      uint8_t byte;
      auto r = f->read_unlocked(&byte, 1);
      if (r.has_error()) {
        f->set_err_unlocked();
        return {i, r.error};
      }
      if (r.value < 1) {
        if (!cr.isEmpty()) {
          cr.clear();
          f->set_err_unlocked();
          return {i, EILSEQ};
        }
        return {i, 0};
      }
      int push_err = cr.push(static_cast<char8_t>(byte));
      if (push_err != 0) {
        cr.clear();
        f->set_err_unlocked();
        return {i, push_err};
      }
    }

    auto cp = cr.pop_utf32();
    if (!cp.has_value()) {
      cr.clear();
      f->set_err_unlocked();
      return {i, EILSEQ};
    }
    dst[i] = static_cast<wchar_t>(cp.value());
  }
  return {n, 0};
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_WCHAR_WIDE_IO_H
