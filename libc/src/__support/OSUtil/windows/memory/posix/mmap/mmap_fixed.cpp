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
//   MAP_FIXED            va_tracker::replace — the substrate handles edge-
//                        straddler split atomically inside the per-arena
//                        envelope (no caller-side pre-split, no race
//                        window). Demote, coalesce-when-needed, sibling
//                        re-map, and `commit_replace` / `map_section_replace`
//                        are all substrate-internal.
//
//   MAP_FIXED_NOREPLACE  va_tracker::acquire — atomic claim via
//                        MEM_RESERVE_PLACEHOLDER; collision returns EEXIST.
//                        Sub-64K-aligned page-aligned hints take the
//                        substrate's prefix-shave path internally.
//
// `validate_map_fixed_target` is the sole and authoritative anti-data-loss
// gate. POSIX-visible mappings live in `va_tracker`; cordoned VA (PE
// images, kernel-loaned ranges, libc-internal allocator chunks, foreign
// `VirtualAllocEx` tenants) lives in the pagemap and is invisible to the
// tracker. Image / Kernel / libc-internal hits surface as EINVAL;
// Foreign / ForeignStale hits surface as ENOMEM so portable apps can
// fall back to the system-chosen base.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mmap/mmap_fixed.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/alloc/pagemap_classifier.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

namespace vt = ::LIBC_NAMESPACE::windows::va_tracker;

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

  // The substrate's `replace` envelope absorbs edge-straddler split
  // atomically under the per-arena LOCKED hold. POSIX MAP_FIXED
  // contract — "discard any overlapping mappings; succeed regardless
  // of straddling pre-existing mappings" — is enforced here in one
  // typed-op call. B2-β multi-prot sibling re-map is deferred past
  // P3; the substrate's ENOTSUP for that case is surfaced as ENOMEM
  // because ENOTSUP is not a POSIX MAP_FIXED errno.
  int rc = vt::replace(range, kind, meta);
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
