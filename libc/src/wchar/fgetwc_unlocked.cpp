//===-- Implementation of fgetwc_unlocked ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/fgetwc_unlocked.h"

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

LLVM_LIBC_FUNCTION(wint_t, fgetwc_unlocked, (::FILE * stream)) {
  auto *f = reinterpret_cast<LIBC_NAMESPACE::File *>(stream);
  // Caller holds the file lock (POSIX.1-2024 §2.5.1). Orientation adoption
  // and the decode must run without re-acquiring it.
  if (!f->adopt_orientation_unlocked(1)) {
    f->set_err_unlocked();
    return WEOF;
  }

  wchar_t wc;
  auto result = internal::read_wide_unlocked(f, &wc, 1);
  if (result.has_error()) {
    libc_errno = result.error;
    return WEOF;
  }
  if (result.value < 1)
    return WEOF;
  return static_cast<wint_t>(wc);
}

} // namespace LIBC_NAMESPACE_DECL
