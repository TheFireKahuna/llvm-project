//===-- Implementation header for the locale --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_LOCALE_LOCALE_H
#define LLVM_LIBC_SRC_LOCALE_LOCALE_H

#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include "hdr/types/locale_t.h"

#include <stddef.h>

// Locale profile (NT-POSIX): C, POSIX, and UTF-8 locales are accepted.
// Accepted locale-name forms for setlocale()/newlocale():
//   "C", "POSIX"            -> C locale.
//   ""                      -> system default (per-user Windows locale).
//   "<lang>[_<territory>]"  -> NLS lookup, codeset defaults to UTF-8.
//   "<name>.UTF-8" / ".utf8" (case-insensitive, dash optional).
//   "<name>[@<modifier>]"   -> NLS-variant selector (e.g. "@euro").
// Any explicit non-UTF-8 codeset (".SJIS", ".GBK", ".ISO-8859-*", ...) is
// rejected: setlocale returns NULL; newlocale returns NULL + errno=EINVAL.
// nl_langinfo(CODESET) reports "UTF-8" for every accepted locale, including
// C / POSIX — the libc has a single multibyte codec (UTF-8) and CODESET
// advertises that uniformly rather than lying about a single-byte path that
// no conversion routine actually implements. Full NLS metadata (names,
// numbers, currency, dates, collation) is available for every installed
// locale regardless.

// Max locale name: "xx-Xxxx-XX" (BCP 47 script subtag) + NUL.
inline constexpr size_t MAX_LOCALE_NAME_SIZE = 16;

// Base locale data — platform-agnostic. Platform-specific subclasses (e.g.,
// WindowsLocaleData) extend this with additional fields, following the same
// pattern as File/LinuxFile. Platform code downcasts to the concrete type.
struct __locale_data {
  char name[MAX_LOCALE_NAME_SIZE];
};

namespace LIBC_NAMESPACE_DECL {

// The default "C" locale.
extern __locale_t c_locale;

// Per-thread locale pointer. nullptr = global locale.
//
// Defined inline so the C++17 inline-variable rule (linkonce_odr / COMDAT
// pick-any) applies to both the variable and clang's emitted Itanium TLS
// init wrapper (_ZTH...). Without this, every TU including this header
// would emit a non-mergeable strong wrapper and lld-link would reject the
// duplicates. Standard C++ semantics — no MSVC extension required.
inline LIBC_THREAD_LOCAL locale_t thread_locale = nullptr;

// Global locale — set by setlocale(), read as fallback when thread_locale
// is nullptr. Atomic pointer swap for thread safety.
locale_t get_global_locale();
void set_global_locale(locale_t loc);

// Return the effective locale for the calling thread.
inline locale_t get_current_locale() {
  locale_t tl = thread_locale;
  return tl ? tl : get_global_locale();
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_LOCALE_LOCALE_H
