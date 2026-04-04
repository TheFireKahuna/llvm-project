//===-- Implementation of fgetws ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/fgetws.h"

#include "hdr/types/FILE.h"
#include "hdr/types/wchar_t.h"
#include "hdr/types/wint_t.h"
#include "hdr/wchar_macros.h"
#include "src/__support/File/file.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/wide_io.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wchar_t *, fgetws,
                   (wchar_t *__restrict ws, int n, ::FILE *__restrict stream)) {
  if (n <= 0)
    return nullptr;

  auto *f = reinterpret_cast<LIBC_NAMESPACE::File *>(stream);
  f->lock();

  // Adopt wide orientation on first use; reject on byte-oriented streams
  // (C11 §7.21.2p3/p4).
  if (!f->adopt_orientation_unlocked(1)) {
    f->set_err_unlocked();
    f->unlock();
    return nullptr;
  }

  // Read one wchar at a time so the newline-stop semantics (C11 §7.29.3.2p2)
  // remain correct without a wide lookahead buffer: batching past L'\n'
  // would consume input we can't push back beyond the single wide ungetc
  // slot.
  int i = 0;
  for (; i < n - 1; ++i) {
    wchar_t wc;
    auto result = internal::read_wide_unlocked(f, &wc, 1);
    if (result.has_error() || result.value < 1) {
      // C11 §7.29.3.2p3: if EOF before any characters are read, return null.
      if (i == 0) {
        f->unlock();
        return nullptr;
      }
      break;
    }
    ws[i] = wc;
    if (wc == L'\n') {
      ++i;
      break;
    }
  }

  ws[i] = L'\0';
  f->unlock();
  return ws;
}

} // namespace LIBC_NAMESPACE_DECL
