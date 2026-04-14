//===-- Implementation of iswctype_l --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wctype/iswctype_l.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/wctype/wctype_descriptors.h"
#include "src/__support/wctype_utils.h"

#include "hdr/types/locale_t.h"
#include "hdr/types/wctype_t.h"
#include "hdr/types/wint_t.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, iswctype_l, (wint_t c, wctype_t desc, locale_t loc)) {
  (void)loc;
  wchar_t wc = static_cast<wchar_t>(c);
  switch (desc) {
  case WCTYPE_ALNUM:
    return internal::isalnum(wc);
  case WCTYPE_ALPHA:
    return internal::isalpha(wc);
  case WCTYPE_BLANK:
    return internal::isblank(wc);
  case WCTYPE_CNTRL:
    return internal::iscntrl(wc);
  case WCTYPE_DIGIT:
    return internal::isdigit(wc);
  case WCTYPE_GRAPH:
    return internal::isgraph(wc);
  case WCTYPE_LOWER:
    return internal::islower(wc);
  case WCTYPE_PRINT:
    return internal::isprint(wc);
  case WCTYPE_PUNCT:
    return internal::ispunct(wc);
  case WCTYPE_SPACE:
    return internal::isspace(wc);
  case WCTYPE_UPPER:
    return internal::isupper(wc);
  case WCTYPE_XDIGIT:
    return internal::isxdigit(wc);
  default:
    return 0;
  }
}

} // namespace LIBC_NAMESPACE_DECL
