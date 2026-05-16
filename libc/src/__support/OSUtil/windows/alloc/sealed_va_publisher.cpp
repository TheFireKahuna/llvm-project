//===-- Sealed VA publication chokepoint — implementation ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/alloc/sealed_va_publisher.h"

#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>
#include <string.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

namespace {

// Capacity covers: 1 Pagemap + 3 Buddy (partition, tree, desc pool) +
// 2 Partition coarse-pagemap/desc-pool + ~30 individual partitions
// (today 22 core, with headroom). 64 caps the foreseeable surface;
// overflow traps rather than growing silently — past 64 needs a design
// review, not a quiet realloc.
constexpr size_t kMaxSealedRanges = 64;

struct SealedRangeEntry {
  void *base;      // Inclusive base of the half-open range.
  void *end;       // Exclusive upper bound (base + size).
  SealedKind kind; // Category for pagemap stamp encoding.
  uint32_t _pad;   // Pads entry to 24 B for stable layout.
};
static_assert(sizeof(SealedRangeEntry) == 24,
              "SealedRangeEntry layout drift — 24 B expected");

SealedRangeEntry g_sealed_ranges[kMaxSealedRanges];
size_t g_sealed_count = 0;

// Trips a double wipe (or a stray publish that slipped the init-state
// gate) into a clean trap.
bool g_sealed_table_wiped = false;

// Eager kinds get every covered pagemap entry stamped at publish time.
// Non-eager kinds are either too large for eager commit to be worthwhile
// (the pagemap itself, multi-GiB partition arenas) or already have
// chunk-level subscribers that stamp incrementally as live chunks are
// handed out.
[[nodiscard]] LIBC_INLINE bool kind_registers_in_pagemap(SealedKind k) {
  switch (k) {
  case SealedKind::Pagemap:
  case SealedKind::BuddyPartition:
  case SealedKind::Partition:
    return false;
  case SealedKind::BuddyTree:
  case SealedKind::BuddyDescPool:
  case SealedKind::PartitionCoarsePagemap:
  case SealedKind::PartitionDescPool:
    return true;
  }
  __builtin_trap();
}

[[nodiscard]] LIBC_INLINE bool ranges_overlap(void *a_lo, void *a_hi,
                                              void *b_lo, void *b_hi) {
  // Half-open [lo, hi): overlap iff a_lo < b_hi AND b_lo < a_hi.
  return reinterpret_cast<uintptr_t>(a_lo) <
             reinterpret_cast<uintptr_t>(b_hi) &&
         reinterpret_cast<uintptr_t>(b_lo) <
             reinterpret_cast<uintptr_t>(a_hi);
}

void stamp_pagemap_for_range(SealedKind kind, void *base, size_t size) {
  // Round size up to chunk granularity; bases are already chunk-aligned
  // by NT's 64 KiB allocation granularity (re-checked at the public
  // entry point).
  size_t aligned_size =
      (size + kPagemapChunkBytes - 1) & ~(kPagemapChunkBytes - 1);
  if (LIBC_UNLIKELY(aligned_size == 0))
    __builtin_trap();

  // Upgrade backing pagemap OS pages PAGE_READONLY -> PAGE_READWRITE.
  // Idempotent per pagemap.h.
  int rc = pagemap_register_range(base, aligned_size);
  if (LIBC_UNLIKELY(rc != 0))
    __builtin_trap();

  // Publication boundary: pagemap_publish_range issues one RELEASE store
  // per chunk; readers ACQUIRE-load in pagemap_load_decoded (cross-TU,
  // pagemap.h) and observe the stamp atomically.
  pagemap_publish_range(base, aligned_size, static_cast<uint32_t>(kind),
                        VaChunkConsumer::LibcSealed);
}

void insert_sorted_or_trap(const SealedRangeEntry &entry) {
  // Disjointness: linear scan is fine — bring-up publishes O(30) ranges,
  // no concurrent producers.
  void *e_lo = entry.base;
  void *e_hi = entry.end;
  for (size_t i = 0; i < g_sealed_count; ++i) {
    const SealedRangeEntry &existing = g_sealed_ranges[i];
    if (LIBC_UNLIKELY(
            ranges_overlap(e_lo, e_hi, existing.base, existing.end)))
      __builtin_trap();
  }

  if (LIBC_UNLIKELY(g_sealed_count >= kMaxSealedRanges))
    __builtin_trap();

  // Sorted insert by base keeps seal_time_assert_and_wipe's final pass to
  // a single sweep over adjacent pairs.
  size_t pos = g_sealed_count;
  for (size_t i = 0; i < g_sealed_count; ++i) {
    if (reinterpret_cast<uintptr_t>(entry.base) <
        reinterpret_cast<uintptr_t>(g_sealed_ranges[i].base)) {
      pos = i;
      break;
    }
  }
  for (size_t i = g_sealed_count; i > pos; --i)
    g_sealed_ranges[i] = g_sealed_ranges[i - 1];
  g_sealed_ranges[pos] = entry;
  ++g_sealed_count;
}

} // namespace

void publish_sealed_va_range(SealedKind kind, void *base, size_t size) {
  // Init-state gate. Tier A bring-up states (None..TierA_Identity,
  // values 0..6) permit publication; from TierA (7, "Tier A completed")
  // onward this is a structural bug.
  if (LIBC_UNLIKELY(::LIBC_NAMESPACE::pcb_init_state() >=
                    ::LIBC_NAMESPACE::PcbInitState::TierA))
    __builtin_trap();
  if (LIBC_UNLIKELY(g_sealed_table_wiped))
    __builtin_trap();
  if (LIBC_UNLIKELY(base == nullptr || size == 0))
    __builtin_trap();

  // Chunk alignment: NT's 64 KiB allocation granularity gives this for
  // every nt_pal::reserve_* return; the check catches future callers
  // that source VA elsewhere.
  if (LIBC_UNLIKELY((reinterpret_cast<uintptr_t>(base) &
                     (kPagemapChunkBytes - 1)) != 0))
    __builtin_trap();

  SealedRangeEntry entry{};
  entry.base = base;
  entry.end = static_cast<unsigned char *>(base) + size;
  entry.kind = kind;
  insert_sorted_or_trap(entry);

  // Stamp the pagemap only after the inventory accepted the range, so a
  // publish that traps on overlap leaves no partially stamped pagemap.
  if (kind_registers_in_pagemap(kind))
    stamp_pagemap_for_range(kind, base, size);
}

void seal_time_assert_and_wipe() {
  if (LIBC_UNLIKELY(g_sealed_table_wiped))
    __builtin_trap();

  // Tautology check over the sorted inventory: publish-time invariants
  // already hold this; the scan exists to catch future drift in
  // insert_sorted_or_trap. Cannot detect a caller that bypassed the
  // publisher entirely — only inventory-internal corruption.
  for (size_t i = 1; i < g_sealed_count; ++i) {
    const SealedRangeEntry &prev = g_sealed_ranges[i - 1];
    const SealedRangeEntry &curr = g_sealed_ranges[i];
    if (LIBC_UNLIKELY(reinterpret_cast<uintptr_t>(prev.base) >=
                      reinterpret_cast<uintptr_t>(curr.base)))
      __builtin_trap();
    if (LIBC_UNLIKELY(reinterpret_cast<uintptr_t>(prev.end) >
                      reinterpret_cast<uintptr_t>(curr.base)))
      __builtin_trap();
  }

  // Honeypot wipe: a post-seal partial-read primitive landing on this
  // BSS page sees zeros instead of the live sealed-VA layout. Sealed
  // bases remain reachable through their PCB Zone 0 fields — this only
  // removes the centralized aggregate.
  ::memset(&g_sealed_ranges, 0, sizeof(g_sealed_ranges));
  g_sealed_count = 0;
  g_sealed_table_wiped = true;
}

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
