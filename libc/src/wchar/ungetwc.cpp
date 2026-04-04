//===-- Implementation of ungetwc -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/ungetwc.h"

#include "hdr/types/FILE.h"
#include "hdr/types/wint_t.h"
#include "hdr/wchar_macros.h"
#include "src/__support/File/file.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wint_t, ungetwc, (wint_t c, ::FILE *stream)) {
  if (c == WEOF)
    return WEOF;

  auto *f = reinterpret_cast<LIBC_NAMESPACE::File *>(stream);
  f->lock();

  // Adopt wide orientation; reject on byte-oriented streams
  // (C11 §7.21.2p3/p4).
  if (!f->adopt_orientation_unlocked(1)) {
    f->unlock();
    return WEOF;
  }

  bool ok = f->push_wide_char(static_cast<wchar_t>(c));

  f->unlock();
  return ok ? c : WEOF;
}

} // namespace LIBC_NAMESPACE_DECL
