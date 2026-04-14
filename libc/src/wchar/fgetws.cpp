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
#include "src/__support/wchar/read_wchar.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wchar_t *, fgetws,
                   (wchar_t *__restrict ws, int n, ::FILE *__restrict stream)) {
  if (n <= 0)
    return nullptr;

  auto *f = reinterpret_cast<LIBC_NAMESPACE::File *>(stream);
  f->lock();

  // Set wide orientation on first use (C11 §7.21.2p4).
  f->fwide_unlocked(1);

  int i = 0;
  for (; i < n - 1; ++i) {
    wint_t wc = internal::read_wchar_unlocked(f);
    if (wc == WEOF) {
      // C11 §7.29.3.2p3: if EOF before any characters are read, return null.
      if (i == 0) {
        f->unlock();
        return nullptr;
      }
      break;
    }
    ws[i] = static_cast<wchar_t>(wc);
    if (ws[i] == L'\n') {
      ++i;
      break;
    }
  }

  ws[i] = L'\0';
  f->unlock();
  return ws;
}

} // namespace LIBC_NAMESPACE_DECL
