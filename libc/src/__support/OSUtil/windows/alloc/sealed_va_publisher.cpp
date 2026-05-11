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

//===----------------------------------------------------------------------===//
// Transient inventory (BSS-resident).
//
// Capacity sized for: 1 Pagemap + 3 Buddy (partition, tree, desc pool) +
// 2 Partition coarse-pagemap/desc-pool + ~30 individual partitions (today
// 22 core, with growth headroom). 64 entries covers the foreseeable
// surface; the publisher traps on overflow rather than growing silently.
//===----------------------------------------------------------------------===//

namespace {

constexpr size_t kMaxSealedRanges = 64;

struct SealedRangeEntry {
  void *base;      ///< Inclusive base of the half-open range.
  void *end;       ///< Exclusive upper bound (base + size).
  SealedKind kind; ///< Category for pagemap stamp encoding.
  uint32_t _pad;   ///< Pads the entry to 24 bytes for stable layout.
};
static_assert(sizeof(SealedRangeEntry) == 24,
              "SealedRangeEntry layout drift — 24 B expected");

// BSS, zero-initialised on first commit. 64 * 24 B = 1.5 KiB.
SealedRangeEntry g_sealed_ranges[kMaxSealedRanges];
size_t g_sealed_count = 0;

// Set by `seal_time_assert_and_wipe()` so a double wipe (or a stray
// publish that slipped past the init-state gate) can be trapped.
bool g_sealed_table_wiped = false;

//===----------------------------------------------------------------------===//
// Per-kind eager-pagemap registration policy.
//
// Eager kinds get every covered pagemap entry stamped at publish time.
// Non-eager kinds are either too large for eager pagemap commit to be
// worthwhile or already have chunk-level subscribers that stamp the
// pagemap incrementally as live chunks are handed out.
//===----------------------------------------------------------------------===//

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
  // Unknown kind — fail loud at the caller's frame.
  __builtin_trap();
}

//===----------------------------------------------------------------------===//
// Range geometry helpers.
//===----------------------------------------------------------------------===//

[[nodiscard]] LIBC_INLINE bool ranges_overlap(void *a_lo, void *a_hi,
                                              void *b_lo, void *b_hi) {
  // Half-open [lo, hi). Overlap iff a_lo < b_hi AND b_lo < a_hi.
  return reinterpret_cast<uintptr_t>(a_lo) <
             reinterpret_cast<uintptr_t>(b_hi) &&
         reinterpret_cast<uintptr_t>(b_lo) <
             reinterpret_cast<uintptr_t>(a_hi);
}

//===----------------------------------------------------------------------===//
// Pagemap stamp loop (eager kinds only).
//===----------------------------------------------------------------------===//

void stamp_pagemap_for_range(SealedKind kind, void *base, size_t size) {
  // Round size up to pagemap chunk granularity. Bases are already
  // chunk-aligned by NT's 64 KiB allocation granularity.
  size_t aligned_size =
      (size + kPagemapChunkBytes - 1) & ~(kPagemapChunkBytes - 1);
  if (LIBC_UNLIKELY(aligned_size == 0))
    __builtin_trap();

  // Upgrade affected pagemap OS pages RO->RW. Idempotent — internal
  // per-OS-page state elides redundant `NtProtectVirtualMemory` calls.
  int rc = pagemap_register_range(base, aligned_size);
  if (LIBC_UNLIKELY(rc != 0))
    __builtin_trap();

  // Bulk-publish `(slot_idx = kind, tag = LibcSealed)`. This store is
  // the publication boundary for the stamped range; wait-free readers
  // acquire-load the encoded word in `pagemap_load_decoded` and observe
  // the stamp atomically.
  pagemap_publish_range(base, aligned_size, static_cast<uint32_t>(kind),
                        VaChunkConsumer::LibcSealed);
}

//===----------------------------------------------------------------------===//
// Sorted insert + invariant validation.
//===----------------------------------------------------------------------===//

void insert_sorted_or_trap(const SealedRangeEntry &entry) {
  // Phase 1: disjointness against existing entries.
  void *e_lo = entry.base;
  void *e_hi = entry.end;
  for (size_t i = 0; i < g_sealed_count; ++i) {
    const SealedRangeEntry &existing = g_sealed_ranges[i];
    if (LIBC_UNLIKELY(
            ranges_overlap(e_lo, e_hi, existing.base, existing.end)))
      __builtin_trap();
  }

  // Phase 2: capacity. Fail loud rather than silently truncate.
  if (LIBC_UNLIKELY(g_sealed_count >= kMaxSealedRanges))
    __builtin_trap();

  // Phase 3: sorted insert by `base`. Maintaining sort order during
  // publication keeps `seal_time_assert_and_wipe`'s final pass to a
  // single sweep over adjacent pairs.
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

//===----------------------------------------------------------------------===//
// Public API.
//===----------------------------------------------------------------------===//

void publish_sealed_va_range(SealedKind kind, void *base, size_t size) {
  // Init-state gate. This primitive is bring-up only; any call after
  // the read-only PCB seal is a structural bug.
  if (LIBC_UNLIKELY(::LIBC_NAMESPACE::pcb_init_state() >=
                    ::LIBC_NAMESPACE::PcbInitState::TierA))
    __builtin_trap();
  if (LIBC_UNLIKELY(g_sealed_table_wiped))
    __builtin_trap();
  if (LIBC_UNLIKELY(base == nullptr || size == 0))
    __builtin_trap();

  // Base alignment to pagemap chunk granularity. NT's 64 KiB allocation
  // granularity guarantees this for every `nt_pal::reserve_*` return;
  // the check catches future callers that source VA elsewhere.
  if (LIBC_UNLIKELY((reinterpret_cast<uintptr_t>(base) &
                     (kPagemapChunkBytes - 1)) != 0))
    __builtin_trap();

  SealedRangeEntry entry{};
  entry.base = base;
  entry.end = static_cast<unsigned char *>(base) + size;
  entry.kind = kind;
  insert_sorted_or_trap(entry);

  // Stamp the pagemap only after the inventory accepted the range, so
  // a publish that traps on overlap leaves no partially stamped pagemap
  // (the stamp itself is idempotent, but consistency between the two
  // structures is easier to reason about with this ordering).
  if (kind_registers_in_pagemap(kind))
    stamp_pagemap_for_range(kind, base, size);
}

void seal_time_assert_and_wipe() {
  if (LIBC_UNLIKELY(g_sealed_table_wiped))
    __builtin_trap();

  // Tautology check over the sorted inventory. The publish path already
  // maintains these invariants; the scan exists to catch future drift in
  // `insert_sorted_or_trap`. It cannot detect a caller that bypassed
  // the publisher entirely (no entry to scan), only inventory-internal
  // corruption. Cheap, runs once.
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

  // Wipe the inventory. After this point a post-seal partial-read
  // primitive landing on the publisher's BSS page sees zeros instead of
  // the live sealed-VA layout. Individual sealed bases remain reachable
  // through their sealed PCB Zone 0 fields; this only removes the
  // centralized aggregate.
  ::memset(&g_sealed_ranges, 0, sizeof(g_sealed_ranges));
  g_sealed_count = 0;
  g_sealed_table_wiped = true;
}

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
