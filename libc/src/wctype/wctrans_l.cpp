//===-- Implementation of wctrans_l ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wctype/wctrans_l.h"

#include "src/wctype/wctrans.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wctrans_t, wctrans_l,
                   (const char *charclass, locale_t loc)) {
  (void)loc;
  return LIBC_NAMESPACE::wctrans(charclass);
}

} // namespace LIBC_NAMESPACE_DECL
