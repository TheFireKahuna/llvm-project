//===-- Implementation of putwchar_unlocked -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/putwchar_unlocked.h"

#include "hdr/types/FILE.h"
#include "hdr/types/wchar_t.h"
#include "hdr/types/wint_t.h"
#include "src/__support/File/file.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/wchar/fputwc_unlocked.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wint_t, putwchar_unlocked, (wchar_t wc)) {
  return LIBC_NAMESPACE::fputwc_unlocked(wc,
                                         reinterpret_cast<::FILE *>(stdout));
}

} // namespace LIBC_NAMESPACE_DECL
