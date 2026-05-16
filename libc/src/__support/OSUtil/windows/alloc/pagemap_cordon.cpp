//===- alloc/pagemap_cordon.cpp - Cordon stamp implementation -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Most cordon mutations route through pagemap.h's owner-publish API. The
// novelty here is the set_cordon_stale / clear_cordon_stale CAS-and-retry
// loop, which must coexist with concurrent stamp / retire (single-publisher
// per chunk) and wait-free readers.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/alloc/pagemap_cordon.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include "hdr/stdint_proxy.h"
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {
namespace pagemap {

namespace {

LIBC_INLINE bool is_chunk_aligned_size(size_t v) {
  return (v & (kPagemapChunkBytes - 1)) == 0;
}

LIBC_INLINE bool is_chunk_aligned_addr(const void *p) {
  return (reinterpret_cast<uintptr_t>(p) & (kPagemapChunkBytes - 1)) == 0;
}

LIBC_INLINE bool range_well_formed(const void *base, size_t bytes) {
  return base != nullptr && bytes != 0 && is_chunk_aligned_addr(base) &&
         is_chunk_aligned_size(bytes);
}

LIBC_INLINE VaChunkConsumer kind_to_tag(CordonKind kind) {
  switch (kind) {
  case CordonKind::Image:
    return VaChunkConsumer::Image;
  case CordonKind::Kernel:
    return VaChunkConsumer::Kernel;
  case CordonKind::Foreign:
    return VaChunkConsumer::Foreign;
  }
  // Unreachable enum value implies caller-side corruption: trap rather
  // than silently stamping a wrong band, which would survive the cookie
  // XOR and present as a forged cordon to is_cordon().
  __builtin_trap();
}

// Walk [base, base + bytes) chunk-by-chunk and CAS-replace any entry whose
// decoded tag is `from_tag` with the encoded form of (slot_idx=0, to_tag).
//
// Concurrency: readers see either OLD or NEW (8 B atomic word, torn-free
// on x86-64 and AArch64). On CAS loss we re-decode and retry only if the
// entry still carries `from_tag`; any other observed tag means the entry
// moved on (retired, re-stamped, opposite-toggled) and we drop the chunk
// (best-effort). Two opposite toggles racing compose to either no-op or
// one ordered flip; we never deadlock.
//
// Memory ordering: ACQ_REL on success — the ACQUIRE half pairs with the
// RELEASE in pagemap_store / pagemap_store_encoded that originally
// stamped `from_tag` (cross-TU), and the RELEASE half is consumed by
// later ACQUIRE readers (pagemap_load_decoded). ACQUIRE on failure
// refreshes `observed` for the retry decision below.
void toggle_cordon_band(void *base, size_t bytes, VaChunkConsumer from_tag,
                         VaChunkConsumer to_tag) {
  if (LIBC_UNLIKELY(!range_well_formed(base, bytes)))
    return;

  size_t n_chunks = bytes >> kPagemapShift;
  for (size_t i = 0; i < n_chunks; ++i) {
    void *entry_addr =
        static_cast<char *>(base) + (i * kPagemapChunkBytes);

    // pagemap_load_decoded is bounds-checked and returns (0, Empty) for
    // out-of-range or unstamped entries — either case fails the
    // from_tag gate and we skip without touching the slot.
    PagemapDecoded decoded = pagemap_load_decoded(entry_addr);
    if (decoded.tag != from_tag)
      continue;

    // Reconstruct encoded from/to via pagemap_encode rather than a second
    // raw load — gives the CAS a known-good baseline even if the ACQUIRE
    // caught a transitional state.
    uint64_t observed = pagemap_encode(decoded.slot_idx, from_tag);
    uint64_t desired = pagemap_encode(decoded.slot_idx, to_tag);

    PagemapEntry *slot = internal::pagemap_slot_unchecked(entry_addr);

    if (slot->encoded.compare_exchange_weak(
            observed, desired, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE))
      continue;

    // First CAS lost; the ACQUIRE failure refreshed `observed`. With
    // compare_exchange_weak the loss may be spurious (no concurrent
    // writer) — retry once when the freshly decoded tag is still
    // from_tag. Any other tag means a real concurrent writer flipped
    // the entry (retire, stamp, opposite toggle); drop the chunk.
    // Second loss intentionally unhandled.
    decoded = pagemap_decode(observed);
    if (decoded.tag != from_tag)
      continue;

    desired = pagemap_encode(decoded.slot_idx, to_tag);
    (void)slot->encoded.compare_exchange_weak(
        observed, desired, cpp::MemoryOrder::ACQ_REL,
        cpp::MemoryOrder::ACQUIRE);
  }
}

} // namespace

[[nodiscard]] int stamp_cordon(void *base, size_t bytes, CordonKind kind) {
  if (LIBC_UNLIKELY(!range_well_formed(base, bytes)))
    return -EINVAL;

  // Upgrade backing pagemap OS pages from PAGE_READONLY shared-zero to
  // PAGE_READWRITE. Idempotent. Runs before the publish loop, so failure
  // leaves no entries observable as stamped.
  int rc = pagemap_register_range(base, bytes);
  if (LIBC_UNLIKELY(rc != 0))
    return -ENOMEM;

  // slot_idx fixed at 0 — cordons have no descriptor pool.
  pagemap_publish_range(base, bytes, /*slot_idx=*/0u, kind_to_tag(kind));
  return 0;
}

void retire_cordon(void *base, size_t bytes) {
  if (LIBC_UNLIKELY(!range_well_formed(base, bytes)))
    return;
  pagemap_retire_range(base, bytes);
}

void set_cordon_stale(void *base, size_t bytes) {
  toggle_cordon_band(base, bytes, VaChunkConsumer::Foreign,
                     VaChunkConsumer::ForeignStale);
}

void clear_cordon_stale(void *base, size_t bytes) {
  toggle_cordon_band(base, bytes, VaChunkConsumer::ForeignStale,
                     VaChunkConsumer::Foreign);
}

} // namespace pagemap
} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
