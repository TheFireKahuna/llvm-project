//===- munmap.cpp - POSIX munmap on the va_tracker -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `internal::munmap(addr, size)` lands as three substrate calls:
//   1) `validate_map_fixed_target` for the cordon probe — loaded PE
//      images, kernel mappings (TEB/PEB/stack), and foreign third-
//      party VAs map to `EINVAL`. The same primitive the MAP_FIXED
//      path uses; sharing the gate keeps the "what is unmappable"
//      answer in one place.
//   2) Two `va_tracker::split` calls at the alloc-granularity edges
//      of the requested range. Pure metadata; no kernel work. Catches
//      the straddler case (a region whose extent crosses one of the
//      edges) so the subsequent release sees only fully-inside descs.
//      A boundary that lands in MEM_FREE or on an exact region edge
//      surfaces a benign `ENOENT` / `EINVAL` from split — both are
//      "no straddle, nothing to do."
//   3) `va_tracker::release` over the whole 64 KiB-aligned range. The
//      substrate iterates the locked set, releases each desc, and
//      runs the Stage-2 teardown (unmap section view, free
//      placeholder, close handles) synchronously per the substrate's
//      mutator-owns-kernel-state discipline. Holes inside the range
//      are skipped silently — Linux contract.
//
// Stage-2 ordering is substrate-owned; the POSIX layer trusts the
// typed op. Likewise, the ANON_PLACEHOLDER state-preserving rollback,
// the per-fragment fresh-region-ID assignment, the partial-section
// view re-map, and the `release_if_isolated` decision are all
// absorbed into the substrate.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/munmap.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_meta.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_validation.h"
#include "src/__support/OSUtil/windows/memory/va_inventory.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

namespace mp = ::LIBC_NAMESPACE::windows::memory_posix;
namespace vt = ::LIBC_NAMESPACE::windows::va_tracker;

} // namespace

namespace internal {

intptr_t munmap(void *addr, size_t size) {
  // Zero length is `EINVAL` (legacy convention; matches glibc).
  if (LIBC_UNLIKELY(size == 0))
    return -EINVAL;
  if (LIBC_UNLIKELY(addr == nullptr))
    return -EINVAL;
  if (LIBC_UNLIKELY(!mp::is_page_aligned(addr)))
    return -EINVAL;

  const size_t rounded = mp::rounded_len_or_zero(size);
  if (LIBC_UNLIKELY(rounded == 0))
    return -EINVAL;

  const uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
  if (LIBC_UNLIKELY(mp::addr_plus_len_overflows(addr_val, rounded)))
    return -EINVAL;

  // The substrate's typed-op API works at NT allocation granularity
  // (64 KiB) — both edges round outward. `addr` from a successful
  // mmap is already 64 KiB aligned, so the round-down is normally a
  // no-op; the round-up captures the partial-tail case.
  const uintptr_t lo = mp::align_down_to_granularity(addr_val);
  const uintptr_t hi_raw = mp::align_up_to_granularity(addr_val + rounded);
  if (LIBC_UNLIKELY(hi_raw == 0 || hi_raw <= lo))
    return -EINVAL;

  const size_t kernel_bytes = static_cast<size_t>(hi_raw - lo);

  // Cordon probe. Loaded PE images, kernel mappings (TEB / PEB /
  // stack), and any foreign third-party VAs surface `EINVAL` before
  // any destructive substrate call. This is the new home for the
  // legacy MEM_IMAGE rejection — the cordon decision pre-dates the
  // walk and is wait-free.
  if (int e = ::LIBC_NAMESPACE::windows::validate_map_fixed_target(
          reinterpret_cast<void *>(lo),
          static_cast<SIZE_T>(kernel_bytes));
      e != 0)
    return -e;

  // Pre-split at the alloc-granularity edges. A boundary that lands
  // in MEM_FREE returns `ENOENT`; a boundary that lands on an exact
  // region edge returns `EINVAL`. Both mean "no straddle here, no
  // work to do" — silently tolerated. Only a strict-interior
  // boundary triggers an actual split.
  (void)vt::split(reinterpret_cast<void *>(lo));
  (void)vt::split(reinterpret_cast<void *>(hi_raw));

  vt::VaRange range =
      mp::make_range(reinterpret_cast<void *>(lo), kernel_bytes);

  int rc = vt::release(range);
  if (LIBC_UNLIKELY(rc != 0))
    return -rc;

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
