//===- mmap_fixed.h - POSIX MAP_FIXED / MAP_FIXED_NOREPLACE -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MAP_FIXED and MAP_FIXED_NOREPLACE dispatchers. Both entries share the
/// `validate_map_fixed_target` cordon gate (the anti-data-loss invariant
/// runs once before any destructive substrate op); they diverge in the
/// substrate call that follows.
///
///   * `mmap_fixed_replace` (MAP_FIXED) — wait-free straddler discovery
///     via `va_tracker::walk_range`, pre-split via `va_tracker::split` at
///     each straddling edge, then one `va_tracker::replace` envelope.
///     The substrate owns demote / coalesce / commit_replace and the
///     OLD-backing placeholder-ownership transfer; the POSIX layer does
///     not orchestrate the kernel transitions.
///
///   * `mmap_fixed_noreplace_claim` (MAP_FIXED_NOREPLACE) — one
///     `va_tracker::acquire` envelope. Collision returns EEXIST via the
///     substrate's atomic `MEM_RESERVE_PLACEHOLDER` claim; sub-64K-
///     aligned page-aligned hints take the substrate's prefix-shave
///     path internally.
///
/// Both return the registered base as an `intptr_t` on success and a
/// Linux-flavoured `-errno` on failure.
///
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
