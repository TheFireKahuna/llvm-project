//===-- Lightweight RAII for NT kernel handles -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ScopedNtHandle: move-only RAII wrapper for a raw NT HANDLE.
// Destructor calls NtClose. The universal building block for NT handle
// ownership throughout the libc NT-POSIX layer.
//
// Design principles:
//   - Always-owning. For non-owning references, use raw HANDLE with a
//     comment documenting the ownership contract.
//   - [[nodiscard]] on the class prevents accidental temporaries
//     (ScopedNtHandle(h); would close immediately).
//   - [[nodiscard]] on release() prevents silent handle loss.
//   - put() integrates with NT APIs that write through HANDLE* output
//     parameters, eliminating the two-step raw+adopt dance.
//   - Minimal — no domain-specific factories. Domain types (SectionHandle,
//     etc.) compose this for their owned-handle path.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SCOPED_NT_HANDLE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SCOPED_NT_HANDLE_H

#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

class [[nodiscard]] ScopedNtHandle {
  HANDLE h_ = nullptr;

public:
  // -- Construction --

  LIBC_INLINE ScopedNtHandle() = default;
  LIBC_INLINE explicit ScopedNtHandle(HANDLE h) : h_(h) {}

  LIBC_INLINE ~ScopedNtHandle() {
    if (h_)
      ::NtClose(h_);
  }

  // -- Move-only --

  LIBC_INLINE ScopedNtHandle(ScopedNtHandle &&o) noexcept : h_(o.h_) {
    o.h_ = nullptr;
  }

  LIBC_INLINE ScopedNtHandle &operator=(ScopedNtHandle &&o) noexcept {
    if (this != &o) {
      reset();
      h_ = o.h_;
      o.h_ = nullptr;
    }
    return *this;
  }

  ScopedNtHandle(const ScopedNtHandle &) = delete;
  ScopedNtHandle &operator=(const ScopedNtHandle &) = delete;

  // -- Observers --

  LIBC_INLINE explicit operator bool() const { return h_ != nullptr; }
  LIBC_INLINE HANDLE get() const { return h_; }

  // -- Ownership transfer --

  /// Release ownership and return the raw handle. Caller now owns it.
  /// [[nodiscard]] prevents silent handle loss from an unread release().
  [[nodiscard]] LIBC_INLINE HANDLE release() {
    HANDLE tmp = h_;
    h_ = nullptr;
    return tmp;
  }

  // -- NT output parameter integration --

  /// Return address for receiving a handle from an NT API.
  /// Closes the current handle (if any) before returning the address,
  /// so the NT API writes directly into RAII-managed storage.
  ///
  /// Usage:
  ///   ScopedNtHandle h;
  ///   NTSTATUS st = NtOpenFile(h.put(), access, &oa, &iosb, share, opts);
  ///   // h now owns the handle (or is empty if NtOpenFile failed).
  LIBC_INLINE HANDLE *put() {
    reset();
    return &h_;
  }

  // -- Reset --

  /// Close the current handle (if any) and optionally adopt a new one.
  LIBC_INLINE void reset(HANDLE h = nullptr) {
    if (h_)
      ::NtClose(h_);
    h_ = h;
  }

  // -- Swap --

  LIBC_INLINE void swap(ScopedNtHandle &o) noexcept {
    HANDLE tmp = h_;
    h_ = o.h_;
    o.h_ = tmp;
  }

  LIBC_INLINE friend void swap(ScopedNtHandle &a,
                                ScopedNtHandle &b) noexcept {
    a.swap(b);
  }
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SCOPED_NT_HANDLE_H
