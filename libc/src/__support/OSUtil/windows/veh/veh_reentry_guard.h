//===-- VEH reentry guard via TEB TLS slot ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Prevents infinite recursion when a VEH handler itself faults. A single
// inline TEB TLS slot (one MOV instruction to read/write) gates entry into
// all libc VEH handlers. If a fault occurs inside a handler, the re-entrant
// dispatch sees the flag and bails to EXCEPTION_CONTINUE_SEARCH, letting the
// OS crash handler produce a useful dump instead of a stack overflow.
//
// The TEB TLS slot cannot fault --- the TEB page is always resident and
// mapped by the kernel. This is the only safe place to store per-thread
// state that a VEH handler can read without risk of recursion.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_REENTRY_GUARD_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_REENTRY_GUARD_H

#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

/// Allocate the TEB TLS slot. Must be called once at process startup,
/// before any VEH handler can fire. Returns false on allocation failure.
bool init_veh_reentry_guard();

/// RAII guard for VEH handler entry. Construction checks the TEB slot;
/// if already set, marks the guard as reentrant. Destruction clears
/// the flag if this guard set it.
class VehReentryGuard {
  unsigned tls_index_;
  bool reentrant_;

public:
  LIBC_INLINE explicit VehReentryGuard(unsigned tls_index)
      : tls_index_(tls_index) {
    void *val = teb_tls_get(tls_index);
    if (val) {
      reentrant_ = true;
    } else {
      reentrant_ = false;
      teb_tls_set(tls_index, reinterpret_cast<void *>(uintptr_t(1)));
    }
  }

  LIBC_INLINE ~VehReentryGuard() {
    if (!reentrant_)
      teb_tls_set(tls_index_, nullptr);
  }

  VehReentryGuard(const VehReentryGuard &) = delete;
  VehReentryGuard &operator=(const VehReentryGuard &) = delete;

  LIBC_INLINE bool is_reentry() const { return reentrant_; }
};

/// The TLS index used by all libc VEH handlers. Valid after
/// init_veh_reentry_guard() returns true. TLS_OUT_OF_INDEXES otherwise.
unsigned get_veh_reentry_tls_index();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_REENTRY_GUARD_H
