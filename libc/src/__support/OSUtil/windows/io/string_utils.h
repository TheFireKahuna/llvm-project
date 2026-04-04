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

#include "src/__support/CPP/string_view.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace string_util {

// Null-safe string equality check via string_view.
LIBC_INLINE bool streq(const char *lhs, const char *rhs) {
  if (!lhs || !rhs)
    return false;
  return cpp::string_view(lhs) == cpp::string_view(rhs);
}

// Null-safe prefix check via string_view.
LIBC_INLINE bool starts_with(const char *text, const char *prefix) {
  if (!text || !prefix)
    return false;
  return cpp::string_view(text).starts_with(cpp::string_view(prefix));
}

// Case-insensitive equality for ASCII string_views.
// Only folds A-Z to a-z; non-ASCII bytes compare as-is.
LIBC_INLINE bool ascii_streq_icase(cpp::string_view lhs,
                                   cpp::string_view rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (size_t i = 0; i < lhs.size(); ++i) {
    unsigned char a = static_cast<unsigned char>(lhs[i]);
    unsigned char b = static_cast<unsigned char>(rhs[i]);
    if (a >= 'A' && a <= 'Z')
      a += 32;
    if (b >= 'A' && b <= 'Z')
      b += 32;
    if (a != b)
      return false;
  }
  return true;
}

// Null-safe overload for raw C strings.
LIBC_INLINE bool ascii_streq_icase(const char *lhs, const char *rhs) {
  if (!lhs || !rhs)
    return false;
  return ascii_streq_icase(cpp::string_view(lhs), cpp::string_view(rhs));
}

} // namespace string_util
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STRING_UTILS_H
