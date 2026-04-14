//===-- Implementation of iswupper_l --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wctype/iswupper_l.h"
#include "src/__support/common.h"
#include "src/__support/wctype/wctype_classification_utils.h"

#include "hdr/types/locale_t.h"
#include "hdr/types/wint_t.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, iswupper_l, (wint_t c, locale_t loc)) {
  (void)loc;
  return (lookup_properties(static_cast<wchar_t>(c)) & UPPER) ? 1 : 0;
}

} // namespace LIBC_NAMESPACE_DECL
