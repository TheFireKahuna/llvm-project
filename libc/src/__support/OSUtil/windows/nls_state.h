//===-- NLS process state for Windows ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NLS_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NLS_STATE_H

#include "src/__support/OSUtil/windows/nls_locale.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// NLS blob + parsed tables. Init is gated by a file-local LazyInit<> in
// nls_locale.cpp (not `.libclzr`-registered: the blob comes from
// NtInitializeNlsFiles, which is immutable process-wide and survives exec).
struct NlsState {
  void *blob;
  nls::LocaleHeader *header;
  uint8_t *records;
  uint8_t *calendar_records;
  WCHAR *strings;
  nls::LcidEntry *lcid_table;
  nls::NameEntry *name_table;
  uint32_t default_lcid;
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NLS_STATE_H
