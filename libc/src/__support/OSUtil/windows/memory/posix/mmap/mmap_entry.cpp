//===- mmap_entry.cpp - POSIX mmap dispatcher -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Top-level `internal::mmap` entry. The structure is
//   1) entry validation (size, sharing exact-one, W+X opt-in, page-
//      round overflow, the explicitly-rejected Linux flag values), and
//   2) shape dispatch via a small switch on the relevant flag bits.
//
// Only the anonymous-private shape lands on the new path; every other
// shape (MAP_FIXED, MAP_FIXED_NOREPLACE, MAP_HUGETLB, anon-shared,
// file-backed) delegates to the renamed legacy engine until its own
// phase replaces the delegation. The MAP_FIXED branches still call
// `validate_map_fixed_target` first so a destructive request that
// straddles a cordoned chunk receives `-EINVAL` regardless of which
// implementation handles the body — the anti-data-loss invariant
// stays in one place from the moment this dispatcher goes live.
//
// Post-acquire hooks (MAP_POPULATE / MAP_LOCKED / MCL_FUTURE) run only
// on a successful acquire; the early-return branches never reach the
// hooks.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mmap/mmap.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "include/llvm-libc-macros/windows/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/alloc/pagemap_classifier.h"
#include "src/__support/OSUtil/windows/memory/legacy/mmap_engine.h"
#include "src/__support/OSUtil/windows/memory/posix/mlock_policy.h"
#include "src/__support/OSUtil/windows/memory/posix/mmap/mmap_fixed.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_meta.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_validation.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/nt_pal/query.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

namespace mp = ::LIBC_NAMESPACE::windows::memory_posix;
namespace vt = ::LIBC_NAMESPACE::windows::va_tracker;

// Returns 0 if `(addr, size, prot, flags)` passes the entry-validation
// table that applies to every mmap shape (size, sharing exact-one, W+X
// opt-in, page-round overflow, the explicitly-rejected Linux flag
// values). Positive errno on rejection. Per-shape extra validation
// (MAP_FIXED_NOREPLACE addr, file fd shape) lives inside the per-shape
// handler so the dispatcher stays single-concern.
LIBC_INLINE int validate_entry(bool size_zero, int prot, int flags,
                               size_t rounded_size) {
  if (LIBC_UNLIKELY(size_zero))
    return EINVAL;
  if (int e = mp::validate_mmap_flags(flags); e != 0)
    return e;
  if (int e = mp::validate_posix_prot(prot, flags); e != 0)
    return e;
  if (LIBC_UNLIKELY(rounded_size == 0))
    return ENOMEM;
  return 0;
}

// Returns true iff `flags` selects the anonymous-private shape with no
// destructive placement flag — the only fully-implemented mmap shape
// in this layer. MAP_ANON|MAP_SHARED falls back to anonymous-private
// on Windows historically (no fork-shared anonymous shm), but routing
// it through this gate would skip the SHARED desc flag — the future
// anon-shared path owns that distinction. The strict gate keeps the
// SHARED case delegated.
LIBC_INLINE bool is_anon_private_shape(int flags) {
  if (!(flags & MAP_ANONYMOUS))
    return false;
  if (!(flags & MAP_PRIVATE))
    return false;
  if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE | MAP_HUGETLB))
    return false;
  return true;
}

// MAP_FIXED variant of the above: HUGETLB still routes to the legacy
// engine pending P4, file-backed and anon-shared FIXED do likewise.
// Anon-private FIXED (with or without NOREPLACE) is the only shape P3
// handles on the new path.
LIBC_INLINE bool is_fixed_anon_private_shape(int flags) {
  if (!(flags & MAP_ANONYMOUS))
    return false;
  if (!(flags & MAP_PRIVATE))
    return false;
  if (flags & MAP_HUGETLB)
    return false;
  return true;
}

// Fire the post-acquire hooks (MAP_POPULATE / MAP_LOCKED / MCL_FUTURE)
// on a successful anonymous mapping. Caller passes the returned base
// and the rounded size — the same value range the user will observe.
// Lock / prefetch failures never propagate; mmap success is contractual.
LIBC_INLINE void post_acquire_hooks(void *base, size_t bytes, int prot,
                                    int flags) {
  if ((flags & MAP_POPULATE) && prot != PROT_NONE)
    ::LIBC_NAMESPACE::nt_pal::prefetch_committed(base,
                                                  static_cast<SIZE_T>(bytes));

  if ((flags & MAP_LOCKED) && prot != PROT_NONE)
    ::LIBC_NAMESPACE::windows::lock_range(base,
                                          static_cast<SIZE_T>(bytes));

  if (prot != PROT_NONE)
    ::LIBC_NAMESPACE::windows::lock_if_future(base,
                                              static_cast<SIZE_T>(bytes));
}

} // namespace

namespace internal {

intptr_t mmap(void *addr, size_t size, int prot, int flags, int fd,
              off_t offset) {
  // Two-tier overflow check: pre-validation reports `size == 0`
  // separately from the post-round `ENOMEM`, since the spec demands
  // distinct errnos for the two.
  const size_t rounded_size = mp::rounded_len_or_zero(size);
  if (int e = validate_entry(size == 0, prot, flags, rounded_size); e != 0)
    return -e;

  // MAP_FIXED_NOREPLACE: validate the address shape regardless of which
  // phase implements the dispatch. Null / unaligned addr is `EINVAL`
  // even before any cordon probe.
  if (int e = mp::validate_fixed_noreplace_addr(flags, addr); e != 0)
    return -e;

  // MAP_FIXED / MAP_FIXED_NOREPLACE: P3 owns the anon-private dispatch.
  // The anti-data-loss invariant runs here once so a destructive
  // request straddling a cordoned chunk receives `EINVAL` (image /
  // kernel / libc-internal) or `ENOMEM` (foreign). File-backed /
  // anon-shared / hugetlb FIXED return `ENOSYS` until P4 — the
  // rebuild has no consumers yet, so preserving the legacy path for
  // unfinished shapes only adds drift.
  if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
    if (int e = ::LIBC_NAMESPACE::windows::alloc::pagemap::
            validate_map_fixed_target(addr,
                                      static_cast<size_t>(rounded_size));
        e != 0)
      return -e;
    if (!is_fixed_anon_private_shape(flags))
      return -ENOSYS;

    const vt::AcquireMeta meta = mp::anon_private_meta(prot, flags);
    const vt::VaRange range = mp::make_range(addr, rounded_size);
    const intptr_t result =
        (flags & MAP_FIXED_NOREPLACE)
            ? ::LIBC_NAMESPACE::internal::mmap_fixed_noreplace_claim(
                  range, vt::RegionKind::AnonPrivate, meta)
            : ::LIBC_NAMESPACE::internal::mmap_fixed_replace(
                  range, vt::RegionKind::AnonPrivate, meta);
    if (result < 0)
      return result;

    post_acquire_hooks(reinterpret_cast<void *>(result),
                       static_cast<size_t>(rounded_size), prot, flags);
    (void)fd;
    (void)offset;
    return result;
  }

  // Every remaining shape that is not P2's anon-private routes to the
  // legacy engine until its own phase. MAP_HUGETLB / MAP_ANON|SHARED /
  // fd-backed (file private / file shared) all live there for now.
  if (!is_anon_private_shape(flags))
    return ::LIBC_NAMESPACE::internal::legacy_mmap_engine(addr, size, prot,
                                                          flags, fd, offset);

  intptr_t result = mmap_anon_private(addr, rounded_size, prot, flags);
  if (result < 0)
    return result;

  post_acquire_hooks(reinterpret_cast<void *>(result),
                     static_cast<size_t>(rounded_size), prot, flags);

  // The fd / offset arguments are unused on the anon-private path
  // even when present; Linux ignores them and we follow suit.
  (void)fd;
  (void)offset;
  return result;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
