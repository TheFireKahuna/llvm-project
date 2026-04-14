//===-- Implementation of fputwc ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/fputwc.h"

#include "hdr/errno_macros.h"
#include "hdr/types/FILE.h"
#include "hdr/types/wchar_t.h"
#include "hdr/types/wint_t.h"
#include "hdr/wchar_macros.h"
#include "src/__support/File/file.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/character_converter.h"
#include "src/__support/wchar/locale_encoding.h"
#include "src/__support/wchar/mbstate.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wint_t, fputwc, (wchar_t wc, ::FILE *stream)) {
  auto *f = reinterpret_cast<LIBC_NAMESPACE::File *>(stream);
  f->lock();

  // Set wide orientation on first use (C11 §7.21.2p4).
  f->fwide_unlocked(1);

  char32_t scalar = static_cast<char32_t>(wc);

  // Convert the codepoint to UTF-8 via CharacterConverter.
  internal::mbstate *state = f->get_wide_mbstate();
  internal::CharacterConverter cr(state, internal::locale_encoding_is_utf8());

  int err = cr.push(scalar);
  if (err != 0) {
    f->set_err_unlocked();
    f->unlock();
    libc_errno = err;
    return WEOF;
  }

  // Pop UTF-8 bytes and write them to the stream.
  while (!cr.isEmpty()) {
    auto utf8 = cr.pop_utf8();
    // pop_utf8 cannot fail after a successful push.
    uint8_t byte = static_cast<uint8_t>(utf8.value());
    auto result = f->write_unlocked(&byte, 1);
    if (result.has_error() || result.value < 1) {
      f->unlock();
      libc_errno = result.has_error() ? result.error : EIO;
      return WEOF;
    }
  }

  f->unlock();
  return static_cast<wint_t>(wc);
}

} // namespace LIBC_NAMESPACE_DECL
