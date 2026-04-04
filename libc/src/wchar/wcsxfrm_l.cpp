//===-- Implementation of wcsxfrm_l ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/wcsxfrm_l.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/wchar/wcsxfrm.h"

#include "hdr/types/locale_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/wchar_t.h"

namespace LIBC_NAMESPACE_DECL {

// Locale parameter is accepted but not yet used; delegates to wcsxfrm
// which provides C-locale (identity) transformation on all platforms.
LLVM_LIBC_FUNCTION(size_t, wcsxfrm_l,
                   (wchar_t *__restrict dest, const wchar_t *__restrict src,
                    size_t n, locale_t)) {
  return LIBC_NAMESPACE::wcsxfrm(dest, src, n);
}

} // namespace LIBC_NAMESPACE_DECL
