//===- alloc/pagemap_cordon.cpp - Cordon stamp implementation -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the cordon stamp / retire / toggle surface.
//
// Most cordon mutations are one or more pagemap entry stores routed
// through the pagemap header's owner-publish API. The novelty is the
// `set_cordon_stale` / `clear_cordon_stale` CAS-and-retry loop, which
// must coexist with concurrent stamp / retire (single-publisher
// contract) and concurrent readers (wait-free, non-faulting).
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

//===----------------------------------------------------------------------===//
// Range-shape helpers
//===----------------------------------------------------------------------===//

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

/// Map a caller-facing \c CordonKind to its underlying pagemap routing
/// tag. \c ForeignStale is intentionally absent — it is reachable only
/// via the toggle entry points, never via \c stamp_cordon.
LIBC_INLINE VaChunkConsumer kind_to_tag(CordonKind kind) {
  switch (kind) {
  case CordonKind::Image:
    return VaChunkConsumer::Image;
  case CordonKind::Kernel:
    return VaChunkConsumer::Kernel;
  case CordonKind::Foreign:
    return VaChunkConsumer::Foreign;
  }
  // Trap rather than silently stamp a wrong band: an unreachable enum
  // value implies caller-side corruption that we want surfaced.
  __builtin_trap();
}

/// Walk `[base, base + bytes)` chunk-by-chunk and CAS-replace any entry
/// whose decoded tag is \p from_tag with the encoded form of \p to_tag.
/// Best-effort: a chunk whose CAS loses twice is left alone.
///
/// Concurrency contract:
///   * Readers (wait-free ACQUIRE loads) see either the OLD or NEW
///     encoded word — never a torn value (single 8 B CAS on x86-64).
///   * A concurrent \c stamp_cordon or \c retire_cordon may race; on
///     CAS loss we re-decode and retry only if the entry still carries
///     \p from_tag. If it transitioned to anything else (retired,
///     re-stamped to a different kind), we fall through and leave it.
///   * A concurrent opposite-direction toggle against the same chunk is
///     allowed; the second toggle either un-does the first or finds the
///     entry already in its source state and ours bails.
///
/// Memory ordering: ACQ_REL on success. ACQUIRE is required because
/// post-toggle reader logic (region reconciliation signalling a
/// consumer that may re-probe) depends on the toggle being visible;
/// RELEASE matches the publish-side ordering used by
/// \c pagemap_store_encoded.
void toggle_cordon_band(void *base, size_t bytes, VaChunkConsumer from_tag,
                         VaChunkConsumer to_tag) {
  if (LIBC_UNLIKELY(!range_well_formed(base, bytes)))
    return;

  size_t n_chunks = bytes >> kPagemapShift;
  for (size_t i = 0; i < n_chunks; ++i) {
    void *entry_addr =
        static_cast<char *>(base) + (i * kPagemapChunkBytes);

    // First decoded read. pagemap_load_decoded is bounds-checked and
    // returns (0, Empty) for out-of-range or unstamped entries — either
    // case fails the from_tag check below and we skip.
    PagemapDecoded decoded = pagemap_load_decoded(entry_addr);
    if (decoded.tag != from_tag)
      continue;

    // Reconstruct the encoded from/to words by round-tripping through
    // pagemap_encode. No need to read the raw word again, and we want
    // a known-good baseline for the CAS even if the ACQUIRE load above
    // caught a transitional state.
    uint64_t observed = pagemap_encode(decoded.slot_idx, from_tag);
    uint64_t desired = pagemap_encode(decoded.slot_idx, to_tag);

    PagemapEntry *slot = internal::pagemap_slot_unchecked(entry_addr);

    // First attempt. ACQ_REL on success makes the toggle visible to
    // post-toggle readers; ACQUIRE on failure refreshes `observed` for
    // the retry decision below.
    if (slot->encoded.compare_exchange_weak(
            observed, desired, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE))
      continue;

    // First CAS lost. Re-decode the freshly observed value: if it
    // still carries from_tag, retry once with the fresh (slot_idx,
    // encoded) pair. Otherwise the entry has moved on (concurrent
    // stamp / retire / opposite toggle) and we leave it alone — the
    // best-effort contract.
    decoded = pagemap_decode(observed);
    if (decoded.tag != from_tag)
      continue;

    desired = pagemap_encode(decoded.slot_idx, to_tag);
    (void)slot->encoded.compare_exchange_weak(
        observed, desired, cpp::MemoryOrder::ACQ_REL,
        cpp::MemoryOrder::ACQUIRE);
    // Second loss: drop the chunk. Some other writer is in the band;
    // not our place to fight them.
  }
}

} // namespace

//===----------------------------------------------------------------------===//
// Public surface
//===----------------------------------------------------------------------===//

[[nodiscard]] int stamp_cordon(void *base, size_t bytes, CordonKind kind) {
  if (LIBC_UNLIKELY(!range_well_formed(base, bytes)))
    return -EINVAL;

  // Step 1: upgrade every pagemap OS page covering the range from
  // PAGE_READONLY shared-zero to PAGE_READWRITE. pagemap_register_range
  // is idempotent against re-registration (the per-OS-page upgrade-state
  // byte short-circuits the syscall). On failure NO entries are
  // observable as stamped — registration runs first, the publish loop
  // below is unreachable.
  int rc = pagemap_register_range(base, bytes);
  if (LIBC_UNLIKELY(rc != 0))
    return -ENOMEM;

  // Step 2: stamp every chunk with the cordon's encoded word.
  // slot_idx is fixed at 0 — cordons have no per-entry descriptor pool;
  // the tag alone routes diagnostic dispatch.
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
