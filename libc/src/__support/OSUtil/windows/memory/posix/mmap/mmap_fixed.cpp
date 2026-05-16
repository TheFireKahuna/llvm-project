//===- mmap_fixed.cpp - POSIX MAP_FIXED dispatch on the tracker -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
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

// Anti-data-loss cordon. POSIX-visible VA lives in `va_tracker`; cordoned VA
// (PE images, kernel-loaned ranges, libc-internal allocator chunks, foreign
// VirtualAllocEx tenants) lives only in the pagemap. Image / Kernel /
// libc-internal hits return EINVAL; Foreign / ForeignStale return ENOMEM so
// portable apps can fall back to a system-chosen base.
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

  // Substrate `replace` absorbs edge-straddler split atomically under its
  // per-arena LOCKED hold — no caller-side pre-split, no MEM_FREE window.
  // The B2-β multi-prot sibling re-map path is deferred past P3; the
  // substrate signals it via ENOTSUP which we surface as ENOMEM (ENOTSUP is
  // not a POSIX MAP_FIXED errno).
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

  // Atomic claim: NT places the placeholder via MEM_RESERVE_PLACEHOLDER
  // inside the envelope, so a concurrent peer requesting the same range
  // observes STATUS_CONFLICTING_ADDRESSES and the tracker returns EEXIST.
  auto ref = vt::acquire(range, kind, meta);
  if (!ref.has_value())
    return -ref.error();
  return reinterpret_cast<intptr_t>(range.start);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
