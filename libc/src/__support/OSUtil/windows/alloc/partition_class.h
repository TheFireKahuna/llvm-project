//===- partition_class.h - PartitionClass identity + geometry --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stable identity for the partition layer's type-isolation taxonomy plus the
// fixed geometry (4 GiB stride, reserve-table size, guard bands) that every
// partition-aware subsystem agrees on.
//
// PartitionClass numeric values are part of the on-process ABI: stamped into
// the pagemap entry's consumer_tag high byte, surfaced in CPU traces, and
// reproduced in fastfail crash diagnostics. Renumbering invalidates every
// stamped pagemap entry across fork/exec and breaks post-mortem dump
// analysis -- existing entries must never be renumbered; new classes append
// at the end of their band.
//
// Three bands with distinct lifecycle contracts:
//
//   * Core libc-internal (slots 1..15) -- eager-reserved at process
//     bring-up on the node-agnostic key; pinned for process lifetime so the
//     empty-transition retire path never reaps them. Back fixed-size
//     internal pools (fd table, OFD, AIO control blocks, skiplist nodes).
//   * User-facing allocator (slots 16..19) -- demand-reserved per
//     (class, numa_node); retire-eligible. When both bytes_committed and
//     active_chunks drop to zero on a partition, the decrementing thread
//     inlines retire and the 4 GiB VA returns to the OS.
//   * VA-index (slots 20..31) -- node-agnostic key; pinned (mix of eager
//     bring-up and demand-reserved on first use, both publishing on
//     kNodeAgnostic). Used by the POSIX-visible VA index (skiplist height
//     buckets, ART node sizes, region descriptors, per-leaf arena state).
//
// Fundamentally a hardening primitive: type-isolated VA disjointness (a
// use-after-free on one type cannot land in another partition's slab),
// compact-pointer support (32-bit offset within a 4 GiB partition),
// NUMA-aware per-class replicas for the user-facing band, and
// PAGE_NOACCESS guard bands bracketing each partition so OOB past a
// boundary traps deterministically. Reverse-lookup ("is this VA
// allocator-owned?") falls out as a secondary consequence of the layout.
//
// Size-class taxonomy descends from mimalloc (Leijen, Zorn, de Moura,
// APLAS 2019) and PartitionAlloc (Chromium); per-class typed-isolation
// mirrors PartitionAlloc's type partitions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PARTITION_CLASS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PARTITION_CLASS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {
namespace partition {

// Stable 16-bit identity for one type-isolated partition class. Encoded
// into the pagemap entry's consumer_tag high byte so each class must fit
// in 8 bits; numeric values are on-process ABI (see file banner for the
// renumbering contract).
enum class PartitionClass : uint16_t {
  Empty = 0,

  // ---- Core libc-internal (slots 1..15) -- eager, node-agnostic, pinned.

  ThreadScratchPool = 1,
  FdTable = 2,
  IoringStorage = 3,
  SkiplistNode = 4,           // Skiplist leaf nodes (non-VA-index callers).
  ArtNode = 5,                // ART outer nodes (non-VA-index callers).
  RegionDesc = 6,
  OpenFileDescription = 7,
  AioCb = 8,
  TimerNode = 9,
  ChildEntry = 10,
  EpollNode = 11,
  PkeyRange = 12,
  CoreReserved13 = 13,
  CoreReserved14 = 14,
  CoreReserved15 = 15,

  // ---- User-facing allocator (slots 16..19) -- demand, per-node, retire.

  AllocSmall = 16,
  AllocMedium = 17,
  AllocLarge = 18,
  AllocHuge = 19,

  // ---- VA-index (slots 20..31) -- node-agnostic, pinned (mix of eager
  //      and demand reservation; all publishing on kNodeAgnostic).
  //
  // One class per shape of VA-index descriptor so the partition contract
  // (the chunk owner is the sole writer of any pagemap entry inside its
  // partition's VA range) holds even though all VA-index descriptors live
  // in the same module. Each class gets its own 4 GiB window with
  // chunk-state-machine reuse.
  //
  // Slot 29 is intentionally unused -- placeholder reserved against a
  // future per-CPU arena split; never stamped into any pagemap entry.

  VaTrackerSkiplist1_2  = 20, // Skiplist height buckets (1-2, 3-4, 5-8, 9-16).
  VaTrackerSkiplist3_4  = 21,
  VaTrackerSkiplist5_8  = 22,
  VaTrackerSkiplist9_16 = 23,
  VaTrackerArtNode4     = 24, // ART node sizes (Node4 / 16 / 48 / 256).
  VaTrackerArtNode16    = 25,
  VaTrackerArtNode48    = 26,
  VaTrackerArtNode256   = 27,
  VaTrackerRegionDesc   = 28, // POSIX-visible mapping payload.
  VaTrackerArena        = 30, // Per-leaf arena state (320 B slot).
  VaTrackerDescBacking  = 31, // Refcounted, shared per source mapping.

  // 32..254 reserved for future hardening / compactor / GWP-ASan. 256..0xFFFE
  // intentionally unused -- uint16_t is for cheap encode/decode shifts, not
  // for >256 classes. Append within a band; never in the gap.

  Sentinel = 0xFFFF,
};

// Sentinel NUMA node id for partitions not bound to a specific node. The
// pinned bands (core libc-internal and VA-index) reserve without an
// MemExtendedParameterNumaNode hint -- physical pages fault in on the
// requesting CPU's node under NT default placement.
inline constexpr uint16_t kNodeAgnostic = 0xFFFF;

//===----------------------------------------------------------------------===//
// Pagemap consumer_tag encoding.
//
// Each pagemap entry carries a 16-bit consumer_tag of two disjoint 8-bit
// fields:
//
//   bits 0..7  -- VaChunkConsumer (chunk-owning mechanism; pagemap.h
//                 enumerates BuddyDirect / ThreadHeap / HugeDirect /
//                 PartitionGuard / GwpAsan / Misc).
//   bits 8..15 -- PartitionClass low byte (the 4 GiB partition the chunk
//                 lives in; stamped by the chunk owner at register time).
//
// Splitting them lets the SIGSEGV classifier answer both "what mechanism"
// and "what partition" from a single pagemap load.
//
// Both fields fit in 8 bits today (PartitionClass <= 31, VaChunkConsumer
// <= 5 plus Misc=0xFF). Extending either taxonomy must keep the live
// ranges <= 0xFE; 0xFF is the "none assigned" sentinel for both.
//===----------------------------------------------------------------------===//

// Sentinel low-byte meaning "no partition class assigned".
inline constexpr uint16_t kPartitionClassNone = 0xFF;

[[nodiscard]] LIBC_INLINE constexpr uint16_t
encode_consumer_tag(uint16_t chunk_consumer_low8, PartitionClass cls) {
  return static_cast<uint16_t>(
      (chunk_consumer_low8 & 0xFFu) |
      ((static_cast<uint16_t>(cls) & 0xFFu) << 8));
}

[[nodiscard]] LIBC_INLINE constexpr uint16_t
decode_chunk_consumer_low8(uint16_t consumer_tag) {
  return static_cast<uint16_t>(consumer_tag & 0xFFu);
}

[[nodiscard]] LIBC_INLINE constexpr PartitionClass
decode_partition_class(uint16_t consumer_tag) {
  return static_cast<PartitionClass>((consumer_tag >> 8) & 0xFFu);
}

//===----------------------------------------------------------------------===//
// Geometry -- 4 GiB-aligned partitions, 32-bit shift.
//===----------------------------------------------------------------------===//

// 4 GiB stride lets a 32-bit offset within a partition act as a compact
// pointer (snmalloc, Liétar et al., ISMM 2019).
inline constexpr uint8_t kPartitionShift = 32;

inline constexpr size_t kPartitionBytes = static_cast<size_t>(1)
                                          << kPartitionShift;

// Open-addressed reserve-table capacity. 128 * 8 B = 1024 B = 16 cache
// lines, L1-resident; power of 2 so probe wrap collapses to & (size-1).
// Live populations:
//   * 12 core libc-internal on kNodeAgnostic (slots 1..15 - 3 reserved).
//   * 11 VA-index on kNodeAgnostic (slots 20..31 - slot 29).
//   * 4 user-facing allocator classes replicated per active NUMA node.
// Worst observed (16-CCD Threadripper Pro 7995WX, NPS4) = 23 + 4*16 = 87
// entries, 68% load factor -- open-addressing-tolerable with headroom.
// The previous size (64) overflowed at that point.
inline constexpr uint32_t kReserveTableSize = 128;

inline constexpr uint32_t kReserveTableMask = kReserveTableSize - 1;

// Coarse pagemap reservation capacity, in entries. 47-bit user VA / 4 GiB
// stride = 2^15 entries; 8 B/entry = 256 KiB reserved up front, lazily
// committed per touched 4 KiB OS page (one page covers 512 entries =
// 2 TiB of VA).
inline constexpr size_t kCoarseMaxEntries = static_cast<size_t>(1) << 15;

// Reservation size rounded up to NT's 64 KiB allocation granularity so the
// placeholder size matches what MEM_REPLACE_PLACEHOLDER expects on commit.
inline constexpr size_t kCoarseBytes =
    ((kCoarseMaxEntries * sizeof(void *) + 64) + 0xFFFF) &
    ~static_cast<size_t>(0xFFFF);

// Descriptor pool capacity. 256 * 128 B = 32 KiB; ~16 core + ~16 user *
// up to 4 NUMA nodes plus headroom.
inline constexpr uint32_t kPartitionDescPoolCapacity = 256;

// Head and tail guard size. 64 KiB matches NT's allocation granularity so
// the usable range inside the partition stays 64 KiB-aligned within the
// 4 GiB reservation. Mapped PAGE_NOACCESS so OOB past a partition
// boundary traps deterministically instead of landing in a sibling slab.
inline constexpr size_t kPartitionGuardBytes = 64 * 1024;

//===----------------------------------------------------------------------===//
// Reserve-table key packing.
//
// A partition is uniquely identified by (PartitionClass, numa_node). The
// pair packs into one 32-bit key so the open-addressing primary slot
// derives from a single 64-bit Splitmix64 finalizer call.
//===----------------------------------------------------------------------===//

[[nodiscard]] LIBC_INLINE constexpr uint32_t
pack_partition_key(PartitionClass cls, uint16_t numa_node) {
  return (static_cast<uint32_t>(cls) << 16) | numa_node;
}

[[nodiscard]] LIBC_INLINE constexpr PartitionClass
unpack_class(uint32_t key) {
  return static_cast<PartitionClass>(key >> 16);
}

[[nodiscard]] LIBC_INLINE constexpr uint16_t unpack_node(uint32_t key) {
  return static_cast<uint16_t>(key & 0xFFFFu);
}

// Threads racing to register the same (class, node) pair must hash to the
// same starting slot so open-addressing's linear probe converges on one
// CAS winner; a non-deterministic hash would expose a duplicate-publish
// window. Splitmix64 finalizer (Stafford 2013) chosen for its avalanche
// over a 32-bit input.
[[nodiscard]] LIBC_INLINE constexpr uint32_t
primary_slot(uint32_t key) {
  uint64_t k = key;
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return static_cast<uint32_t>(k) & kReserveTableMask;
}

} // namespace partition
} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
