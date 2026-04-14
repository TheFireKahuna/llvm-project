//===-- Lightweight RAII for NT kernel handles -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ScopedNtHandle: move-only RAII wrapper for a raw NT HANDLE.
// Destructor calls NtClose. Useful for file handles, thread handles, and
// other kernel objects that don't need the provenance tracking of
// SectionHandle (which has owned/borrowed modes).
//
// This is intentionally minimal — no factory methods, no borrow mode.
// Use SectionHandle for section objects that need fd-cache borrowing.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SCOPED_NT_HANDLE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SCOPED_NT_HANDLE_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

class ScopedNtHandle {
  HANDLE h_ = nullptr;

public:
  LIBC_INLINE ScopedNtHandle() = default;
  LIBC_INLINE explicit ScopedNtHandle(HANDLE h) : h_(h) {}

  LIBC_INLINE ~ScopedNtHandle() {
    if (h_)
      ::NtClose(h_);
  }

  // Move-only.
  LIBC_INLINE ScopedNtHandle(ScopedNtHandle &&o) noexcept : h_(o.h_) {
    o.h_ = nullptr;
  }
  LIBC_INLINE ScopedNtHandle &operator=(ScopedNtHandle &&o) noexcept {
    if (this != &o) {
      if (h_)
        ::NtClose(h_);
      h_ = o.h_;
      o.h_ = nullptr;
    }
    return *this;
  }
  ScopedNtHandle(const ScopedNtHandle &) = delete;
  ScopedNtHandle &operator=(const ScopedNtHandle &) = delete;

  // Observers.
  LIBC_INLINE explicit operator bool() const { return h_ != nullptr; }
  LIBC_INLINE HANDLE get() const { return h_; }

  /// Release ownership and return the raw handle (caller now owns it).
  LIBC_INLINE HANDLE release() {
    HANDLE tmp = h_;
    h_ = nullptr;
    return tmp;
  }

  /// Reset to a new handle, closing the old one if any.
  LIBC_INLINE void reset(HANDLE h = nullptr) {
    if (h_)
      ::NtClose(h_);
    h_ = h;
  }
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SCOPED_NT_HANDLE_H
