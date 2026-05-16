//===- posix_errno.h - POSIX-layer errno overrides --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Context-specific NTSTATUS → errno overrides for the POSIX memory ops, plus
// the sign-convention boundary helper that flips libc-internal negative-errno
// returns into the positive-errno form the public POSIX surface exposes.
//
// `windows_util::ntstatus_to_errno` in `nt/nt_error.h` carries the generic
// table: one NTSTATUS → one POSIX errno, no call-site context. Several mmap-
// family NTSTATUS codes have a context-dependent POSIX answer the generic
// table cannot pick correctly — most prominently `STATUS_ACCESS_DENIED`,
// which the generic table maps to `EACCES` (filesystem-permission sense) but
// which POSIX requires as `EPERM` whenever the kernel is rejecting against a
// process-privilege boundary (`SeLockMemoryPrivilege`, `RLIMIT_MEMLOCK`).
// Each helper below pins one such override at the single call site where the
// op-specific contract makes the divergence unambiguous; everything else
// falls through to the generic mapping.
//
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

// MAP_FIXED_NOREPLACE atomic placeholder claim.
//
// `STATUS_CONFLICTING_ADDRESSES` here means the VA was already taken by a
// concurrent mapping — POSIX answer is `EEXIST`. The generic table maps it
// to `EINVAL` because the same NTSTATUS surfaces from argument-shape
// failures elsewhere; only the MAP_FIXED_NOREPLACE site can distinguish the
// race from a bad argument.
[[nodiscard]] LIBC_INLINE int
errno_for_fixed_noreplace_failure(NTSTATUS status) {
  if (status == STATUS_CONFLICTING_ADDRESSES)
    return EEXIST;
  return ::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(status);
}

// MAP_HUGETLB privilege / availability failure.
//
// On large-page commit, missing `SeLockMemoryPrivilege` can surface as either
// `STATUS_PRIVILEGE_NOT_HELD` (already → `EPERM` in the generic table) or as
// `STATUS_ACCESS_DENIED` (generically → `EACCES`). POSIX expects `EPERM` for
// both — `EACCES` would mislead a portable app into reading the failure as a
// sharing violation. The override pins the ACCESS_DENIED case only; the
// PRIVILEGE_NOT_HELD case already produces the right answer.
[[nodiscard]] LIBC_INLINE int errno_for_hugetlb_failure(NTSTATUS status) {
  if (status == STATUS_ACCESS_DENIED)
    return EPERM;
  return ::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(status);
}

// Per-chunk result for `mlock` / `mlock2` after the working-set expansion
// retry loop has been exhausted.
//
// `STATUS_WORKING_SET_QUOTA` → `EAGAIN`: POSIX `mlock` distinguishes
// "transient quota pressure, try again" from "no headroom anywhere"
// (`ENOMEM`). The generic table picks `ENOMEM` because most callers want
// the conservative answer; mlock callers specifically wait on `EAGAIN`
// before retrying so the override has to be at this site.
//
// `STATUS_ACCESS_DENIED` → `EPERM`: the lock was rejected against the
// `RLIMIT_MEMLOCK` policy boundary, not a filesystem ACL. The generic
// `EACCES` mapping would lead a portable app to test for filesystem
// permission and miss the rlimit cause.
[[nodiscard]] LIBC_INLINE int errno_for_mlock_failure(NTSTATUS status) {
  if (status == STATUS_WORKING_SET_QUOTA)
    return EAGAIN;
  if (status == STATUS_ACCESS_DENIED)
    return EPERM;
  return ::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(status);
}

// Drain a `va_tracker` `ErrorOr<T>` into a kernel-style negative-errno at a
// POSIX entry point's tail. Discards the success-branch value — callers that
// need it must unwrap the `ErrorOr` directly.
//
// `ErrorOr<T>::error()` returns the libc-convention positive errno carried in
// the unexpected branch. The internal layer below the POSIX surface returns
// 0/-errno (Linux-kernel convention: 0 / positive = success, negative errno
// = failure), so the negation here is the sign-convention flip. The
// `e > 0 ? -e : e` guard is defensive — it tolerates an already-negative
// payload from any future caller that pre-flips before constructing the
// `ErrorOr`, so the helper stays a one-liner at every op tail.
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
