//===-- Implementation of wctrans -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wctype/wctrans.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/wctype/wctype_descriptors.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wctrans_t, wctrans, (const char *charclass)) {
  if (__builtin_strcmp(charclass, "toupper") == 0)
    return WCTRANS_TOUPPER;
  if (__builtin_strcmp(charclass, "tolower") == 0)
    return WCTRANS_TOLOWER;
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
