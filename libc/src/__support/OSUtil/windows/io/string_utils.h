//===-- Null-safe string comparison helpers ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lightweight string helpers for internal use by the Windows OSUtil layer.
// These do not depend on libc's own string functions (which may not yet be
// available during early init) and handle null pointers gracefully.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STRING_UTILS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STRING_UTILS_H

#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace string_util {

// Null-safe byte-by-byte string equality check.
LIBC_INLINE bool streq(const char *lhs, const char *rhs) {
  if (!lhs || !rhs)
    return false;
  while (*lhs && *rhs) {
    if (*lhs != *rhs)
      return false;
    ++lhs;
    ++rhs;
  }
  return *lhs == 0 && *rhs == 0;
}

// Null-safe prefix check.  Returns true when text begins with prefix.
LIBC_INLINE bool starts_with(const char *text, const char *prefix) {
  if (!text || !prefix)
    return false;
  while (*prefix) {
    if (*text++ != *prefix++)
      return false;
  }
  return true;
}

} // namespace string_util
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STRING_UTILS_H
