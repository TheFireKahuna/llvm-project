//===-- Implementation of towctrans ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wctype/towctrans.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/wctype/wctype_classification_utils.h"
#include "src/__support/wctype/wctype_descriptors.h"

#include "hdr/types/wctrans_t.h"
#include "hdr/types/wint_t.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wint_t, towctrans, (wint_t c, wctrans_t desc)) {
  if (desc == WCTRANS_TOUPPER)
    return static_cast<wint_t>(unicode_to_upper(static_cast<wchar_t>(c)));
  if (desc == WCTRANS_TOLOWER)
    return static_cast<wint_t>(unicode_to_lower(static_cast<wchar_t>(c)));
  return c;
}

} // namespace LIBC_NAMESPACE_DECL
