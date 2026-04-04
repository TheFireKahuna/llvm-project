//===-- Implementation of fputwc ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/fputwc.h"

#include "hdr/types/FILE.h"
#include "hdr/types/wchar_t.h"
#include "hdr/types/wint_t.h"
#include "hdr/wchar_macros.h"
#include "src/__support/File/file.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/wide_io.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wint_t, fputwc, (wchar_t wc, ::FILE *stream)) {
  auto *f = reinterpret_cast<LIBC_NAMESPACE::File *>(stream);
  f->lock();

  // Adopt wide orientation on first use; reject on byte-oriented streams
  // (C11 §7.21.2p3/p4).
  if (!f->adopt_orientation_unlocked(1)) {
    f->set_err_unlocked();
    f->unlock();
    return WEOF;
  }

  auto result = internal::write_wide_unlocked(f, &wc, 1);
  f->unlock();

  if (result.has_error()) {
    libc_errno = result.error;
    return WEOF;
  }
  if (result.value < 1)
    return WEOF;
  return static_cast<wint_t>(wc);
}

} // namespace LIBC_NAMESPACE_DECL
