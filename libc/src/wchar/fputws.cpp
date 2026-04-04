//===-- Implementation of fputws ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/fputws.h"

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

LLVM_LIBC_FUNCTION(int, fputws,
                   (const wchar_t *__restrict ws, ::FILE *__restrict stream)) {
  auto *f = reinterpret_cast<LIBC_NAMESPACE::File *>(stream);
  f->lock();

  // Adopt wide orientation on first use; reject on byte-oriented streams
  // (C11 §7.21.2p3/p4).
  if (!f->adopt_orientation_unlocked(1)) {
    f->set_err_unlocked();
    f->unlock();
    return -1;
  }

  internal::mbstate *state = f->get_wide_mbstate();

  for (const wchar_t *p = ws; *p != L'\0'; ++p) {
    char32_t scalar = static_cast<char32_t>(*p);

    internal::CharacterConverter cr(state, internal::locale_encoding_is_utf8());
    int err = cr.push(scalar);
    if (err != 0) {
      f->set_err_unlocked();
      f->unlock();
      libc_errno = err;
      return -1;
    }

    while (!cr.isEmpty()) {
      auto utf8 = cr.pop_utf8();
      uint8_t byte = static_cast<uint8_t>(utf8.value());
      auto result = f->write_unlocked(&byte, 1);
      if (result.has_error() || result.value < 1) {
        f->unlock();
        libc_errno = result.has_error() ? result.error : EIO;
        return -1;
      }
    }
  }

  f->unlock();
  return 0; // C11 §7.29.3.4: returns a nonnegative value on success.
}

} // namespace LIBC_NAMESPACE_DECL
