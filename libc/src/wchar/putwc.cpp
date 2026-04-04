//===-- Implementation of putwc -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/putwc.h"

#include "hdr/types/FILE.h"
#include "hdr/types/wchar_t.h"
#include "hdr/types/wint_t.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/wchar/fputwc.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wint_t, putwc, (wchar_t wc, ::FILE *stream)) {
  return LIBC_NAMESPACE::fputwc(wc, stream);
}

} // namespace LIBC_NAMESPACE_DECL
