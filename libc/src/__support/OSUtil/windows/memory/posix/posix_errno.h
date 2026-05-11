//===- posix_errno.h - POSIX-layer errno overrides --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Context-specific NTSTATUS → errno overrides for the POSIX memory ops.
///
/// The generic NTSTATUS table lives in `nt/nt_error.h`
/// (`windows_util::ntstatus_to_errno`); per-op files call it directly
/// for the common case. This header carries only the call sites where
/// POSIX mandates an errno different from the generic mapping, plus a
/// small `va_tracker::ErrorOr<T>` → negative-errno helper for op
/// epilogues. Each override states the rule, the NTSTATUS it overrides,
/// and the per-op contract that requires the divergence.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_ERRNO_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_ERRNO_H

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace memory_posix {

/// MAP_FIXED_NOREPLACE atomic placeholder claim.
///
/// `STATUS_CONFLICTING_ADDRESSES` here means the VA was already taken
/// by a concurrent mapping. POSIX answer is `EEXIST` — the generic
/// table maps to `EINVAL` because the same NTSTATUS surfaces from
/// argument-shape failures elsewhere.
[[nodiscard]] LIBC_INLINE int
errno_for_fixed_noreplace_failure(NTSTATUS status) {
  if (status == STATUS_CONFLICTING_ADDRESSES)
    return EEXIST;
  return ::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(status);
}

/// MAP_HUGETLB privilege / availability failure.
///
/// Both `STATUS_PRIVILEGE_NOT_HELD` and `STATUS_ACCESS_DENIED` mean
/// "no `SeLockMemoryPrivilege`" in the large-page commit path; POSIX
/// answer is `EPERM` for both. The generic mapping for
/// `STATUS_ACCESS_DENIED` is `EACCES`, which a portable app reading
/// errno would misread as a sharing violation.
[[nodiscard]] LIBC_INLINE int errno_for_hugetlb_failure(NTSTATUS status) {
  if (status == STATUS_ACCESS_DENIED)
    return EPERM;
  return ::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(status);
}

/// `mlock` / `mlock2` per-chunk result after `expand_working_set`
/// retries have been exhausted.
///
/// `STATUS_WORKING_SET_QUOTA` → `EAGAIN` — POSIX distinguishes
/// "transient quota pressure" (try again) from "no headroom" (the
/// generic `ENOMEM`); mlock callers rely on `EAGAIN` for retry.
///
/// `STATUS_ACCESS_DENIED` → `EPERM` — the lock failed against the
/// RLIMIT_MEMLOCK policy boundary; the generic `EACCES` mapping would
/// misread as filesystem permission.
[[nodiscard]] LIBC_INLINE int errno_for_mlock_failure(NTSTATUS status) {
  if (status == STATUS_WORKING_SET_QUOTA)
    return EAGAIN;
  if (status == STATUS_ACCESS_DENIED)
    return EPERM;
  return ::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(status);
}

/// Drain a `va_tracker` `ErrorOr<T>` into a kernel-style negative-errno
/// at a POSIX entry point's tail. The success branch's value is
/// discarded — callers that need the value must unwrap the `ErrorOr`
/// directly. Used to make "tracker returned a region ref" → "syscall
/// returns 0" translation a one-liner.
template <class T>
[[nodiscard]] LIBC_INLINE int negate_errno(const ErrorOr<T> &r) {
  if (r.has_value())
    return 0;
  int e = r.error();
  return e > 0 ? -e : e;
}

} // namespace memory_posix
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_ERRNO_H
