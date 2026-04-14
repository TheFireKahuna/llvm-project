//===-- Locale encoding query for multibyte functions -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_WCHAR_LOCALE_ENCODING_H
#define LLVM_LIBC_SRC___SUPPORT_WCHAR_LOCALE_ENCODING_H

#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "src/locale/locale.h"
#endif

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Returns true when the current LC_CTYPE encoding is UTF-8.
// Returns false for the C/POSIX locale (single-byte identity mapping).
//
// On platforms without locale support, always returns true (UTF-8 assumed,
// matching upstream llvm-libc / musl behavior).
inline bool locale_encoding_is_utf8() {
#ifdef LIBC_TARGET_OS_IS_WINDOWS
  // On Windows with locale support, the C locale uses single-byte encoding
  // per POSIX. All NLS locales use UTF-8.
  locale_t loc = get_current_locale();
  return loc != nullptr && loc != &c_locale;
#else
  // Upstream default: always UTF-8 (matches musl behavior).
  return true;
#endif
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_WCHAR_LOCALE_ENCODING_H
