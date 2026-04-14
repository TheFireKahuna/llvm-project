//===-- Implementation of wcscoll_l ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/wcscoll_l.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/wchar/wcscoll.h"

namespace LIBC_NAMESPACE_DECL {

// Locale parameter is accepted but not yet used; delegates to wcscoll
// which provides C-locale (ordinal) comparison on all platforms.
LLVM_LIBC_FUNCTION(int, wcscoll_l,
                   (const wchar_t *left, const wchar_t *right, locale_t)) {
  return LIBC_NAMESPACE::wcscoll(left, right);
}

} // namespace LIBC_NAMESPACE_DECL
