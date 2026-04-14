//===-- Implementation of wctype_l ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wctype/wctype_l.h"

#include "src/wctype/wctype.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wctype_t, wctype_l, (const char *property, locale_t loc)) {
  (void)loc;
  return LIBC_NAMESPACE::wctype(property);
}

} // namespace LIBC_NAMESPACE_DECL
