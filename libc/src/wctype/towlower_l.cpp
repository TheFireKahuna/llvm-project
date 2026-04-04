//===-- Implementation of towlower_l --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wctype/towlower_l.h"
#include "src/__support/common.h"
#include "src/__support/wctype/wctype_classification_utils.h"

#include "hdr/types/locale_t.h"
#include "hdr/types/wint_t.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wint_t, towlower_l, (wint_t c, locale_t loc)) {
  (void)loc;
  return static_cast<wint_t>(unicode_to_lower(static_cast<wchar_t>(c)));
}

} // namespace LIBC_NAMESPACE_DECL
