//===-- Windows locale data subclass -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Windows-specific locale data, extending the platform-agnostic __locale_data
// base. Follows the File pattern: platform code knows the concrete type and
// downcasts from __locale_data * to access platform-specific fields.

#ifndef LLVM_LIBC_SRC_LOCALE_WINDOWS_LOCALE_DATA_H
#define LLVM_LIBC_SRC_LOCALE_WINDOWS_LOCALE_DATA_H

#include "src/locale/locale.h"

struct WindowsLocaleData : __locale_data {
  // Pointer into the read-only NLS blob (nullptr = C locale).
  const unsigned char *nls_record = nullptr;
};

// Downcast helper — mirrors the static_cast pattern used by File callbacks.
inline const unsigned char *
get_nls_record(const __locale_data *data) {
  return static_cast<const WindowsLocaleData *>(data)->nls_record;
}

#endif // LLVM_LIBC_SRC_LOCALE_WINDOWS_LOCALE_DATA_H
