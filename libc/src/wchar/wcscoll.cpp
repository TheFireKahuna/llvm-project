//===-- Implementation of wcscoll -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/wcscoll.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// C/POSIX locale: wcscoll is equivalent to wcscmp (POSIX §7.29.4.4.2).
// Locale-aware collation (e.g. via NLS APIs) is not yet implemented;
// this provides correct C-locale behavior on all platforms.
LLVM_LIBC_FUNCTION(int, wcscoll,
                   (const wchar_t *left, const wchar_t *right)) {
  for (; *left && *left == *right; ++left, ++right)
    ;
  return (*left > *right) - (*left < *right);
}

} // namespace LIBC_NAMESPACE_DECL
