//===-- Locale encoding query for multibyte functions -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_WCHAR_LOCALE_ENCODING_H
#define LLVM_LIBC_SRC___SUPPORT_WCHAR_LOCALE_ENCODING_H

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Returns true when the current LC_CTYPE encoding is UTF-8.
//
// NT-POSIX runs a UTF-8-only locale profile: setlocale() / newlocale() reject
// any explicit non-UTF-8 codeset suffix (see nls_find_locale), and the default
// C locale is itself UTF-8 here (matching musl, upstream llvm-libc, and the
// upstream multibyte test suite, which exercises UTF-8 sequences without
// calling setlocale). Any future single-byte locale would be a process-wide
// override and must be wired through here, not by treating the unconfigured
// startup state as single-byte.
LIBC_INLINE bool locale_encoding_is_utf8() { return true; }

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_WCHAR_LOCALE_ENCODING_H
