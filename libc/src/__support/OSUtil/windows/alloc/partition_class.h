//===- partition_class.h - PartitionClass identity + geometry --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stable identity for the partition layer's type-isolation taxonomy plus the
/// fixed geometry (4 GiB stride, table size, guard bands) that every
/// partition-aware subsystem agrees on.
///
/// \c PartitionClass numeric values are part of the on-process ABI: they are
/// stamped into the pagemap entry's \c consumer_tag high byte, surface in CPU
/// traces, and appear in fastfail crash diagnostics. Renumbering invalidates
/// every stamped pagemap entry across a fork/exec boundary and breaks
/// post-mortem dump analysis. New classes must be appended at the end of
/// their band; existing entries must never be renumbered.
///
/// The taxonomy splits into three bands with different lifecycle contracts:
///
///   * Core libc-internal partitions (slots 1..15) — eagerly reserved at
///     process bring-up on the node-agnostic key; pinned for process
///     lifetime so the empty-transition retire path never reaps them. These
///     back fixed-size internal pools (fd table, OFD, AIO control blocks,
///     skiplist nodes, etc).
///   * User-facing allocator partitions (slots 16..19) — demand-reserved
///     per <tt>(class, numa_node)</tt> pair on the first allocation that
///     needs them; retire-eligible. When both \c bytes_committed and
///     \c active_chunks drop to zero on a partition, the decrementing
///     thread inlines the retire path and the 4 GiB VA returns to the OS.
///   * VA-index partitions (slots 20..31) — eagerly reserved at process
///     bring-up on the node-agnostic key; pinned. Used by the POSIX-visible
///     VA index (skiplist height buckets, adaptive-radix-tree node sizes,
///     region descriptors, per-leaf arena state).
///
/// The class is fundamentally a hardening primitive: type-isolated VA
/// disjointness across classes (a use-after-free on one type cannot land
/// inside another partition's slab), compact-pointer support (32-bit
/// offsets within a 4 GiB partition), NUMA-aware per-class replicas, and
/// \c PAGE_NOACCESS guard bands bracketing each partition. Reverse-lookup
/// ("is this VA allocator-owned?") falls out of the layout as a secondary
/// consequence.
///
/// The size-class taxonomy descends from mimalloc (Leijen, Zorn, de Moura,
/// APLAS 2019) and PartitionAlloc (Chromium); the per-class typed-isolation
/// discipline mirrors PartitionAlloc's type partitions.
///
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

/// Stable 16-bit identity for one type-isolated partition class.
///
/// Encoded into the pagemap entry's \c consumer_tag high byte (so each
/// class must fit in 8 bits). The numeric values are part of the on-process
/// ABI; see the file-level brief for the renumbering contract.
///
/// Bands:
///   * 1..15  — core libc-internal, eager + pinned.
///   * 16..19 — user-facing allocator, demand + retire-eligible.
///   * 20..31 — VA-index, eager + pinned.
enum class PartitionClass : uint16_t {
  Empty = 0,

  // ---- Core libc-internal partitions (slots 1..15) ----
  // Eager-reserved on the node-agnostic key; pinned for process lifetime.

  ThreadScratchPool = 1,
  FdTable = 2,
  IoringStorage = 3,
  SkiplistNode = 4,         ///< VA index, skiplist leaf nodes.
  ArtNode = 5,              ///< VA index, adaptive-radix-tree outer nodes.
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

  // ---- User-facing allocator partitions (slots 16..19) ----
  // Demand-reserved per (class, numa_node); retire-eligible on empty.

  AllocSmall = 16,
  AllocMedium = 17,
  AllocLarge = 18,
  AllocHuge = 19,

  // ---- VA-index partitions (slots 20..31) — pinned ----
  //
  // One class per shape of VA-index descriptor, so the partition contract
  // (the chunk owner is the sole writer of any pagemap entry inside its
  // partition's VA range) holds even though all VA-index descriptors live
  // logically in the same module. Each class gets its own 4 GiB window
  // with chunk-state-machine reuse.
  //
  //   4 skiplist-height buckets (heights 1-2 / 3-4 / 5-8 / 9-16);
  //   4 adaptive-radix-tree node sizes (Node4 / Node16 / Node48 / Node256);
  //   1 region descriptor (POSIX-visible mapping payload);
  //   1 arena (single-arena-per-leaf design);
  //   1 shared kernel-state backing (refcounted; one per source mapping,
  //   shared across fragmenting commits).
  //
  // Slot 29 is intentionally unused to keep numbering contiguous if a
  // future revision reintroduces per-CPU arenas; no pagemap entry is ever
  // stamped with that value.

  VaTrackerSkiplist1_2  = 20,
  VaTrackerSkiplist3_4  = 21,
  VaTrackerSkiplist5_8  = 22,
  VaTrackerSkiplist9_16 = 23,
  VaTrackerArtNode4     = 24,
  VaTrackerArtNode16    = 25,
  VaTrackerArtNode48    = 26,
  VaTrackerArtNode256   = 27,
  VaTrackerRegionDesc   = 28,
  VaTrackerArena        = 30,    ///< Per-leaf arena state (320 B slot).
  VaTrackerDescBacking  = 31,    ///< Refcounted backing, shared per source.

  // Range 32..254 is reserved for future hardening / compactor / GWP-ASan
  // classes. Numeric range 256..0xFFFE is deliberately unused — the type
  // is uint16_t to make the encoder/decoder bit-shifts cheap, not to
  // enable more than 256 classes. Add new classes at the end of the
  // appropriate band, never in the gap.

  Sentinel = 0xFFFF,
};

/// Sentinel passed in place of a NUMA node id when the partition does not
/// bind to a specific node.
///
/// Used by the pinned bands (core libc-internal and VA-index) which were
/// reserved at process bring-up without an
/// \c MemExtendedParameterNumaNode hint; physical pages fault in on the
/// requesting CPU's node by NT default placement.
inline constexpr uint16_t kNodeAgnostic = 0xFFFF;

//===----------------------------------------------------------------------===//
// Pagemap consumer_tag encoding.
//
// Each pagemap entry carries a 16-bit consumer_tag formed from two
// disjoint 8-bit fields:
//
//   bits 0..7  -- VaChunkConsumer (the chunk-owning mechanism: see
//                 VaChunkConsumer in pagemap.h for the taxonomy --
//                 BuddyDirect / ThreadHeap / HugeDirect / PartitionGuard
//                 / GwpAsan / Misc).
//   bits 8..15 -- PartitionClass low byte (which 4 GiB partition the chunk
//                 lives in; stamped by the chunk owner at register time).
//
// The split lets the SIGSEGV classifier answer both "what mechanism" and
// "what partition" from a single pagemap load.
//
// Both fields fit in 8 bits today (PartitionClass <= 31; VaChunkConsumer
// <= 5 plus Misc=0xFF). When extending either taxonomy, keep the two
// ranges disjoint at <= 0xFE; the 0xFF byte is reserved as "not in any
// partition" / "not from any chunk consumer" sentinel.
//===----------------------------------------------------------------------===//

/// Sentinel low-byte value meaning "no partition class assigned".
inline constexpr uint16_t kPartitionClassNone = 0xFF;

/// Pack \p chunk_consumer_low8 (chunk-owner mechanism) and \p cls into one
/// 16-bit pagemap \c consumer_tag.
[[nodiscard]] LIBC_INLINE constexpr uint16_t
encode_consumer_tag(uint16_t chunk_consumer_low8, PartitionClass cls) {
  return static_cast<uint16_t>(
      (chunk_consumer_low8 & 0xFFu) |
      ((static_cast<uint16_t>(cls) & 0xFFu) << 8));
}

/// Extract the chunk-owner mechanism's low 8 bits from \p consumer_tag.
[[nodiscard]] LIBC_INLINE constexpr uint16_t
decode_chunk_consumer_low8(uint16_t consumer_tag) {
  return static_cast<uint16_t>(consumer_tag & 0xFFu);
}

/// Extract the \c PartitionClass from \p consumer_tag.
[[nodiscard]] LIBC_INLINE constexpr PartitionClass
decode_partition_class(uint16_t consumer_tag) {
  return static_cast<PartitionClass>((consumer_tag >> 8) & 0xFFu);
}

//===----------------------------------------------------------------------===//
// Geometry — 4 GiB-aligned partitions, 32-bit shift.
//===----------------------------------------------------------------------===//

/// Base-2 log of the partition stride. 32 yields a 4 GiB stride, which
/// makes a 32-bit offset within a partition addressable as a compact
/// pointer (snmalloc, Liétar et al., ISMM 2019).
inline constexpr uint8_t kPartitionShift = 32;

/// Partition stride in bytes. 4 GiB.
inline constexpr size_t kPartitionBytes = static_cast<size_t>(1)
                                          << kPartitionShift;

/// Capacity of the open-addressed reserve table.
///
/// 128 entries x 8 B = 1024 B = 16 cache lines — L1-resident. Power of 2
/// so probe-step modulo collapses to <tt>& (size - 1)</tt>. The four
/// classes of entries that can land here are:
///
///   * 12 core libc-internal partitions on \c kNodeAgnostic (slots 1..15
///     populated minus 3 reserved = 12).
///   * 11 VA-index partitions on \c kNodeAgnostic (slots 20..31 minus the
///     reserved slot 29 = 11, including the refcounted shared backing
///     class).
///   * 4 user-facing allocator classes replicated per active NUMA node.
///
/// User-replication dominates on big SKUs; the worst observed case
/// (16-CCD Threadripper Pro 7995WX in NPS4 mode) lands at 23 + 4x16 = 87
/// entries, a 68% load factor that an open-addressed table tolerates with
/// headroom. The previous size (64) overflowed at that point.
inline constexpr uint32_t kReserveTableSize = 128;

/// Mask used by the open-addressing probe sequence to wrap an index into
/// the reserve table.
inline constexpr uint32_t kReserveTableMask = kReserveTableSize - 1;

/// Capacity of the coarse pagemap reservation, in entries.
///
/// 47-bit user VA divided by the 4 GiB stride yields 2^15 entries. Each
/// entry is one pointer = 8 B; total 256 KiB reserved up front, lazily
/// committed per touched 4 KiB OS page (one page covers 512 entries =
/// 2 TiB of VA). The reservation size is rounded up to NT's 64 KiB
/// allocation granularity below.
inline constexpr size_t kCoarseMaxEntries = static_cast<size_t>(1) << 15;

/// Coarse pagemap reservation size in bytes, rounded up to 64 KiB so that
/// the placeholder size matches what \c MEM_REPLACE_PLACEHOLDER expects on
/// the subsequent commit call.
inline constexpr size_t kCoarseBytes =
    ((kCoarseMaxEntries * sizeof(void *) + 64) + 0xFFFF) &
    ~static_cast<size_t>(0xFFFF);

/// Descriptor pool capacity. 256 x 128 B = 32 KiB; provisioned for ~16
/// core + ~16 user x up to 4 NUMA nodes plus headroom.
inline constexpr uint32_t kPartitionDescPoolCapacity = 256;

/// Guard band size at each partition's head and tail.
///
/// 64 KiB matches NT's allocation granularity so that the usable range
/// inside the partition remains 64 KiB-aligned inside the 4 GiB
/// reservation. The guard band is mapped \c PAGE_NOACCESS so an OOB read
/// or write past a partition boundary traps deterministically rather than
/// silently landing in a sibling partition's slab.
inline constexpr size_t kPartitionGuardBytes = 64 * 1024;

//===----------------------------------------------------------------------===//
// Reserve-table key packing.
//
// A partition is uniquely identified by (PartitionClass, numa_node). The
// pair is packed into one 32-bit key so the open-addressing primary slot
// derives from a single 64-bit Splitmix64 finalizer call.
//===----------------------------------------------------------------------===//

/// Pack \p cls and \p numa_node into the 32-bit reserve-table key.
[[nodiscard]] LIBC_INLINE constexpr uint32_t
pack_partition_key(PartitionClass cls, uint16_t numa_node) {
  return (static_cast<uint32_t>(cls) << 16) | numa_node;
}

/// Extract the class from a reserve-table key.
[[nodiscard]] LIBC_INLINE constexpr PartitionClass
unpack_class(uint32_t key) {
  return static_cast<PartitionClass>(key >> 16);
}

/// Extract the NUMA node from a reserve-table key.
[[nodiscard]] LIBC_INLINE constexpr uint16_t unpack_node(uint32_t key) {
  return static_cast<uint16_t>(key & 0xFFFFu);
}

/// Compute the primary slot for \p key in the reserve table.
///
/// Threads racing to register the same <tt>(class, node)</tt> pair must
/// all hash to the same starting slot so open-addressing's linear probe
/// converges on one CAS winner; using a deterministic mix here removes a
/// duplicate-publish window that a non-deterministic hash would expose.
/// Splitmix64's finalizer (Stafford 2013) is used for its avalanche on
/// the 32-bit input domain.
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
