//===-- RAII scope-exit guard ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_CPP_SCOPE_GUARD_H
#define LLVM_LIBC_SRC___SUPPORT_CPP_SCOPE_GUARD_H

#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace cpp {

/// Calls a cleanup callable on scope exit unless dismiss() is called.
/// Freestanding-safe: no heap, no exceptions.
template <typename F> class ScopeGuard {
  F func;
  bool active;

public:
  LIBC_INLINE constexpr explicit ScopeGuard(F f)
      : func(static_cast<F &&>(f)), active(true) {}
  LIBC_INLINE ~ScopeGuard() {
    if (active)
      func();
  }
  LIBC_INLINE void dismiss() { active = false; }

  ScopeGuard(const ScopeGuard &) = delete;
  ScopeGuard &operator=(const ScopeGuard &) = delete;
};

template <typename F> LIBC_INLINE constexpr ScopeGuard<F> make_scope_guard(F f) {
  return ScopeGuard<F>(static_cast<F &&>(f));
}

} // namespace cpp
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_CPP_SCOPE_GUARD_H
