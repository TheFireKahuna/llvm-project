//===- alloc/pagemap_classifier.h - Band-level VA classifier ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// First-line band-dispatch classifier over the pagemap.
///
/// Libc-internal VA (allocator chunks, sealed Tier-A regions, va_tracker-
/// internal chunks, internal-VA facade kinds) and cordon kinds
/// (Image / Kernel / Foreign / ForeignStale) carry pagemap entries; this
/// header turns the raw \c pagemap_load_decoded routing tuple into the
/// band-level dispatch surface those callers need.
///
/// POSIX-visible VA is intentionally NOT indexed in the pagemap. A probe
/// on POSIX VA decodes to \c Empty; the VEH master and
/// \c mem_fault_handler treat \c Empty as "consult \c va_tracker::resolve"
/// and propagate to SEH on a tracker miss.
///
/// The classifier is the first probe of:
///
///   * the master VEH handler (first-line dispatch before falling
///     through to \c va_tracker::resolve on \c Empty),
///   * \c is_libc_pointer membership tests,
///   * \c validate_map_fixed_target cordon-overlap rejection.
///
/// Hard contract: every entry point is wait-free, non-faulting, and
/// allocation-free. Each call composes \c pagemap_load_decoded (one
/// ACQUIRE load + cookie XOR + bounds check) with a single
/// integer-range comparison. No syscalls, no Crystalline pins, no
/// skiplist walks — safe from VEH masters, debugger probes, and
/// \c __cxa_finalize teardown.
///
/// Tag-band agreement (locked here so band-aware code does not need to
/// chase enumerator additions made by sibling tracks):
///
///   * `0x00`        — \c Empty (untouched / retired / out-of-bounds);
///                     also every POSIX-visible address.
///   * `0x01..0x0F`  — Layer-2 / Layer-3 chunk allocator.
///   * `0x10..0x1F`  — Sealed Tier-A / hardening kinds.
///   * `0x20..0x2F`  — Layer-1 va_tracker internals.
///   * `0x30..0x3F`  — Cordons (\c Image, \c Kernel, \c Foreign,
///                     \c ForeignStale).
///   * `0x40..0x4F`  — Libc-internal facade tags.
///   * `0x60..0x6F`  — Reserved-empty. `is_libc_owned` is a positive
///                     allow-list, so a stamp in this band (e.g. from
///                     a wild write hitting the pagemap) classifies
///                     as not libc-owned.
///   * `0xFF`        — Misc fallback (treated as libc-owned).
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PAGEMAP_CLASSIFIER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PAGEMAP_CLASSIFIER_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {
namespace pagemap {

//===----------------------------------------------------------------------===//
// Band boundaries
//===----------------------------------------------------------------------===//

/// Contiguous libc-owned body: Layer-2/3 chunk allocators, sealed
/// Tier-A / hardening kinds, and Layer-1 va_tracker internals
/// (`0x01..0x2F`). Excludes \c Empty (\c 0x00) below and the cordon
/// band (\c 0x30..0x3F) above.
inline constexpr uint8_t kLibcBodyBandLo = 0x01;
inline constexpr uint8_t kLibcBodyBandHi = 0x2F;

inline constexpr uint8_t kCordonBandLo = 0x30;
inline constexpr uint8_t kCordonBandHi = 0x3F;

/// Libc-internal facade band (`0x40..0x4F`). Each entry names a
/// libc-internal subsystem that owns VA outside the chunk allocator's
/// descriptor pools.
inline constexpr uint8_t kLibcFacadeBandLo = 0x40;
inline constexpr uint8_t kLibcFacadeBandHi = 0x4F;

//===----------------------------------------------------------------------===//
// ChunkClassification
//===----------------------------------------------------------------------===//

/// Routing tuple returned by \c classify.
///
/// \c tag is the raw \c VaChunkConsumer decoded from the pagemap word;
/// callers may compare against named enumerators or use the band
/// predicates below. \c slot_idx is the consumer's per-band index into
/// its descriptor pool: 0 for cordons (which have no descriptor pool;
/// the tag alone routes dispatch), and per-slab or per-region for the
/// Layer-2 / Layer-1 / facade kinds. For \c Empty entries the field
/// is meaningless and callers MUST gate on \c tag first.
struct ChunkClassification {
  VaChunkConsumer tag;
  uint32_t slot_idx;
};

//===----------------------------------------------------------------------===//
// classify
//===----------------------------------------------------------------------===//

/// First-line band-dispatch decode for \p addr.
///
/// Wait-free, non-faulting for any address in the user-VA window. One
/// ACQUIRE load + cookie XOR + bounds check in \c pagemap_load_decoded,
/// plus one struct repack. Out-of-bounds addresses decode to
/// `{Empty, 0}`.
[[nodiscard]] LIBC_INLINE ChunkClassification
classify(const void *addr) noexcept {
  PagemapDecoded d = pagemap_load_decoded(addr);
  return ChunkClassification{d.tag, d.slot_idx};
}

//===----------------------------------------------------------------------===//
// Band predicates
//===----------------------------------------------------------------------===//

// Each predicate composes classify() with a single numeric range test.
// Ranges are expressed against `static_cast<uint8_t>(d.tag)` so they
// survive non-contiguous enumerator additions and cannot be defeated by
// reordering of `enum class` members.

/// \c true iff \p addr decodes to an empty pagemap entry: retired,
/// never-stamped, out-of-bounds, or POSIX-visible (whose VA is never
/// stamped). Useful as a defensive early-out before calling other
/// predicates; on the VEH path \c Empty causes the master handler to
/// fall through to \c EXCEPTION_CONTINUE_SEARCH.
[[nodiscard]] LIBC_INLINE bool is_empty(const void *addr) noexcept {
  return classify(addr).tag == VaChunkConsumer::Empty;
}

/// \c true iff \p addr's chunk is currently tagged in the cordon band
/// (Image / Kernel / Foreign / ForeignStale, plus any future foreign
/// sub-kinds). Cordon overlap detection is pagemap-only — cordons never
/// enter the va_tracker skiplist.
[[nodiscard]] LIBC_INLINE bool is_cordon(const void *addr) noexcept {
  uint8_t t = static_cast<uint8_t>(classify(addr).tag);
  return t >= kCordonBandLo && t <= kCordonBandHi;
}

/// \c true iff \p addr's chunk is owned by libc bookkeeping or
/// libc-owned scratch (Layer-2/3 chunks, sealed Tier-A regions,
/// va_tracker internals, internal-VA facade, plus the \c Misc
/// catch-all); \c false for unstamped (possibly POSIX), cordon, and
/// out-of-bounds addresses. This is the predicate \c is_libc_pointer
/// is built on.
///
/// Implemented as a positive allow-list of the libc-stamped bands
/// (\c kLibcBodyBandLo..kLibcBodyBandHi, \c kLibcFacadeBandLo..kLibcFacadeBandHi,
/// and \c VaChunkConsumer::Misc). A stamp landing in any other band
/// — including the reserved-empty band \c 0x60..0x6F, currently
/// unallocated bands like \c 0x50..0x5F, and the long unused range
/// \c 0x70..0xFE — classifies as not libc-owned. The allow-list
/// closes a defence-in-depth gap: a wild write that flips an entry
/// into an unallocated band cannot fool \c is_libc_pointer into
/// declaring attacker-controlled VA as ours. Adding a new libc band
/// requires editing this predicate (intentional: the edit is the
/// reviewer's nudge to check whether the new band actually wants
/// \c is_libc_owned semantics).
[[nodiscard]] LIBC_INLINE bool is_libc_owned(const void *addr) noexcept {
  uint8_t t = static_cast<uint8_t>(classify(addr).tag);
  if (t >= kLibcBodyBandLo && t <= kLibcBodyBandHi)
    return true;
  if (t >= kLibcFacadeBandLo && t <= kLibcFacadeBandHi)
    return true;
  if (t == static_cast<uint8_t>(VaChunkConsumer::Misc))
    return true;
  return false;
}

} // namespace pagemap
} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
