//===- mmap_fixed.h - POSIX MAP_FIXED / MAP_FIXED_NOREPLACE -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// MAP_FIXED and MAP_FIXED_NOREPLACE dispatchers. Both run the shared
// `validate_map_fixed_target` cordon gate first (anti-data-loss invariant
// against image / kernel / libc-internal / foreign overlap), then diverge:
//   * mmap_fixed_replace          -> va_tracker::replace
//   * mmap_fixed_noreplace_claim  -> va_tracker::acquire
// The substrate owns straddler split, demote / coalesce, and the kernel
// transitions; the POSIX layer only translates errors. Both return the
// registered base as an `intptr_t` on success and a negative Linux errno on
// failure; the top-level `internal::mmap` propagates that convention to its
// own callers (`windows_syscalls::mmap` flips the sign for ErrorOr;
// `SYS_mmap` returns it raw for syscall(2) semantics).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MMAP_MMAP_FIXED_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MMAP_MMAP_FIXED_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t mmap_fixed_replace(
    ::LIBC_NAMESPACE::windows::va_tracker::VaRange range,
    ::LIBC_NAMESPACE::windows::va_tracker::RegionKind kind,
    const ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta &meta);

intptr_t mmap_fixed_noreplace_claim(
    ::LIBC_NAMESPACE::windows::va_tracker::VaRange range,
    ::LIBC_NAMESPACE::windows::va_tracker::RegionKind kind,
    const ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta &meta);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MMAP_MMAP_FIXED_H
