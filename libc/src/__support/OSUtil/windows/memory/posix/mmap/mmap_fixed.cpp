//===- mmap_fixed.cpp - POSIX MAP_FIXED dispatch on the tracker -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// MAP_FIXED + MAP_FIXED_NOREPLACE land here as one typed-op envelope each
// after the shared `validate_map_fixed_target` cordon gate.
//
//   MAP_FIXED            walk_range (straddler discovery, wait-free) →
//                        optional split at each straddling edge →
//                        va_tracker::replace
//   MAP_FIXED_NOREPLACE  va_tracker::acquire (atomic claim; substrate
//                        handles sub-64K-aligned hints via prefix shave
//                        and rejects collisions with EEXIST)
//
// `validate_map_fixed_target` is the sole and authoritative anti-data-loss
// gate. POSIX-visible mappings live in `va_tracker`; cordoned VA (PE
// images, kernel-loaned ranges, libc-internal allocator chunks, foreign
// `VirtualAllocEx` tenants) lives in the pagemap and is invisible to the
// tracker. A `walk_range` pre-check could not see cordons even in theory,
// so the pagemap probe is mandatory. Image / Kernel / libc-internal hits
// surface as EINVAL; Foreign / ForeignStale hits surface as ENOMEM so
// portable apps can fall back to the system-chosen base.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mmap/mmap_fixed.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/alloc/pagemap_classifier.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

namespace vt = ::LIBC_NAMESPACE::windows::va_tracker;

// Straddler-discovery context for the `walk_range` visitor. The visitor
// receives each desc covering the range in its full extent — covered.lo
// equals desc->lo, covered.hi equals desc->hi — so `covered.lo <
// range_lo` proves a head straddle and `covered.hi > range_hi` proves a
// tail straddle. By VA contiguity there is at most one of each.
struct StraddlerCtx {
  uintptr_t range_lo;
  uintptr_t range_hi;
  bool head_straddles;
  bool tail_straddles;
};

LIBC_INLINE void find_straddlers_visitor(vt::VaRange covered,
                                          vt::RegionDesc * /*desc*/,
                                          void *ctx_p) {
  auto *c = static_cast<StraddlerCtx *>(ctx_p);
  if (covered.lo() < c->range_lo)
    c->head_straddles = true;
  if (covered.hi() > c->range_hi)
    c->tail_straddles = true;
}

// Issue one split when the walk saw a straddler at `boundary`. A
// concurrent peer mutation (`release` / `replace` of an adjacent range)
// that fires between the walk and the split may move the boundary off
// any covering desc, turning the would-be straddler into MEM_FREE or a
// desc edge — split surfaces this as ENOENT or EINVAL. Both are benign:
// the next-step `replace` envelope re-locks the latest state and
// tolerates either, so we propagate only real failures (ENOMEM,
// substrate exhaustion). No retry loop — the substrate's per-arena
// LOCKED hold takes the next envelope past the race.
LIBC_INLINE int issue_split_at(void *boundary) {
  int rc = vt::split(boundary);
  if (rc == 0 || rc == ENOENT || rc == EINVAL)
    return 0;
  return rc;
}

LIBC_INLINE int cordon_gate(vt::VaRange range) {
  return ::LIBC_NAMESPACE::windows::alloc::pagemap::validate_map_fixed_target(
      range.start, range.bytes);
}

} // namespace

namespace internal {

intptr_t mmap_fixed_replace(vt::VaRange range, vt::RegionKind kind,
                             const vt::AcquireMeta &meta) {
  if (int e = cordon_gate(range); e != 0)
    return -e;

  StraddlerCtx ctx{range.lo(), range.hi(), false, false};
  vt::walk_range(range, &find_straddlers_visitor, &ctx);

  if (ctx.head_straddles) {
    if (int rc = issue_split_at(range.start); rc != 0)
      return -rc;
  }
  if (ctx.tail_straddles) {
    void *tail =
        reinterpret_cast<void *>(static_cast<char *>(range.start) + range.bytes);
    if (int rc = issue_split_at(tail); rc != 0)
      return -rc;
  }

  int rc = vt::replace(range, kind, meta);
  // B2-β multi-prot sibling re-map is deferred past P3. The POSIX
  // surface surfaces ENOMEM rather than the substrate's ENOTSUP signal —
  // ENOTSUP is not a POSIX MAP_FIXED errno and would confuse callers.
  if (rc == ENOTSUP)
    rc = ENOMEM;
  if (rc != 0)
    return -rc;
  return reinterpret_cast<intptr_t>(range.start);
}

intptr_t mmap_fixed_noreplace_claim(vt::VaRange range, vt::RegionKind kind,
                                     const vt::AcquireMeta &meta) {
  if (int e = cordon_gate(range); e != 0)
    return -e;

  // `acquire` is the atomic claim primitive: NT places the placeholder
  // via MEM_RESERVE_PLACEHOLDER inside the envelope, so a concurrent
  // peer attempting the same range observes STATUS_CONFLICTING_ADDRESSES
  // and the tracker returns EEXIST. Sub-64K-aligned page-aligned hints
  // run through the substrate's prefix-shave path internally — the
  // POSIX layer passes the user's exact bytes through unchanged.
  auto ref = vt::acquire(range, kind, meta);
  if (!ref.has_value())
    return -ref.error();
  return reinterpret_cast<intptr_t>(range.start);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
