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
// On Windows, use __declspec(thread) instead of C++ thread_local to avoid
// per-TU initialization routines that cause duplicate symbol errors in DLLs.
#ifdef _WIN32
extern __declspec(thread) locale_t thread_locale;
#else
extern LIBC_THREAD_LOCAL locale_t thread_locale;
#endif

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
