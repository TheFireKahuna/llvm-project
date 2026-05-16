//===- mmap_entry.cpp - POSIX mmap dispatcher -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mmap/mmap.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "include/llvm-libc-macros/windows/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/alloc/pagemap_classifier.h"
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

// Returns positive errno on rejection, 0 on accept. Carries only the
// shape-independent checks; per-shape extras (MAP_FIXED_NOREPLACE addr,
// file fd validation) run in their own helpers below.
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

// Anon-private with no destructive placement flag. MAP_ANON|MAP_SHARED is
// excluded so the future anon-shared path owns the SHARED desc-flag
// distinction; routing it here would silently drop the SHARED bit.
LIBC_INLINE bool is_anon_private_shape(int flags) {
  if (!(flags & MAP_ANONYMOUS))
    return false;
  if (!(flags & MAP_PRIVATE))
    return false;
  if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE | MAP_HUGETLB))
    return false;
  return true;
}

// MAP_FIXED / MAP_FIXED_NOREPLACE anon-private — the only FIXED shape with
// a new-path handler today. HUGETLB / anon-shared / file FIXED return
// ENOSYS until their phases land.
LIBC_INLINE bool is_fixed_anon_private_shape(int flags) {
  if (!(flags & MAP_ANONYMOUS))
    return false;
  if (!(flags & MAP_PRIVATE))
    return false;
  if (flags & MAP_HUGETLB)
    return false;
  return true;
}

// Run on a successful acquire only; the early-return branches never reach
// here. Lock / prefetch failures are non-fatal — mmap success is contractual
// once the VA is registered.
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
  // The spec demands distinct errnos for `size == 0` (EINVAL) and post-round
  // overflow (ENOMEM), so the pre-round zero flag and the rounded value both
  // feed `validate_entry`.
  const size_t rounded_size = mp::rounded_len_or_zero(size);
  if (int e = validate_entry(size == 0, prot, flags, rounded_size); e != 0)
    return -e;

  if (int e = mp::validate_fixed_noreplace_addr(flags, addr); e != 0)
    return -e;

  if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
    // Anti-data-loss gate runs before any destructive substrate op, even on
    // the not-yet-implemented FIXED shapes. Image / kernel / libc-internal
    // overlap surfaces as EINVAL; foreign overlap as ENOMEM so portable apps
    // can retry without a hint.
    if (int e = ::LIBC_NAMESPACE::windows::alloc::pagemap::
            validate_map_fixed_target(addr,
                                      static_cast<size_t>(rounded_size));
        e != 0)
      return -e;
    if (!is_fixed_anon_private_shape(flags))
      return -ENOSYS;

    const vt::AcquireMeta meta = mp::anon_private_meta(prot, flags);
    const vt::VaRange range = mp::make_range(addr, rounded_size);
    // NOREPLACE wins when both bits are set — matches Linux: the explicit
    // no-clobber request must not silently degrade to the destructive
    // MAP_FIXED path just because the caller ORed both.
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

  // Unimplemented shapes (HUGETLB, anon-shared, fd-backed) fail closed —
  // there is no legacy fallthrough to preserve, the rebuild has no consumers
  // yet.
  if (!is_anon_private_shape(flags))
    return -ENOSYS;

  intptr_t result = mmap_anon_private(addr, rounded_size, prot, flags);
  if (result < 0)
    return result;

  post_acquire_hooks(reinterpret_cast<void *>(result),
                     static_cast<size_t>(rounded_size), prot, flags);

  // Linux ignores fd / offset on anon-private; matching that.
  (void)fd;
  (void)offset;
  return result;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
