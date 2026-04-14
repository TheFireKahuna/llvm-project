//===-- FLS-compatible API backed by direct TLS ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Migration shim: provides fls_alloc / fls_free / teb_fls_get / teb_fls_set
// backed entirely by inline TEB TLS slots. No FLS, no ntdll, no kernel32.
//
// Callers should migrate to teb_tls.h directly. This header exists so that
// existing callsites compile unchanged during the transition.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEB_FLS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEB_FLS_H

#include "src/__support/OSUtil/windows/tls/teb_tls.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// FLS callback type — kept for API compatibility during migration.
// Callbacks are not registered with ntdll; the caller must arrange
// thread-exit cleanup via .CRT$XL or equivalent.
using FLS_CALLBACK = void(NTAPI *)(PVOID);

// Allocate a TLS slot. The callback parameter is accepted for source
// compatibility but is NOT registered — use .CRT$XL for cleanup.
LIBC_INLINE DWORD fls_alloc(FLS_CALLBACK /*callback*/) {
  return tls_alloc();
}

LIBC_INLINE void fls_free(DWORD index) { tls_free(index); }

LIBC_INLINE void *teb_fls_get(DWORD index) {
  return (index < TLS_INLINE_SLOT_COUNT) ? teb_tls_get(index) : nullptr;
}

LIBC_INLINE void teb_fls_set(DWORD index, void *value) {
  if (index < TLS_INLINE_SLOT_COUNT)
    teb_tls_set(index, value);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEB_FLS_H
