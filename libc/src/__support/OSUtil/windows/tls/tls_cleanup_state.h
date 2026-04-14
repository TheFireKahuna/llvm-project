//===-- Canonical TLS cleanup state for Windows ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lightweight process-wide TLS cleanup registry types used directly in the
// PCB. This keeps the storage model typed without dragging in the cleanup
// execution helpers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TLS_TLS_CLEANUP_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TLS_TLS_CLEANUP_STATE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

using TlsCleanupFn = void(NTAPI *)(void *);

inline constexpr unsigned TLS_CLEANUP_MAX_SLOTS = 16;

struct TlsCleanupEntry {
  DWORD tls_index;
  TlsCleanupFn callback;
};

struct TlsCleanupState {
  TlsCleanupEntry entries[TLS_CLEANUP_MAX_SLOTS];
  cpp::Atomic<unsigned> count;
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TLS_TLS_CLEANUP_STATE_H
