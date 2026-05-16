//===- art_node_alloc.h - Slab-pool for ART internal nodes ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One per-type slab pool per ART internal-node class (Node4/16/48/256),
// each backed by its own pinned 4 GiB partition window. Chunks commit
// lazily and decommit on full drain.
//
// Retire goes through Crystalline-W (Nikolaev & Ravindran, PLDI 2024).
// The FreeFn does pure metadata cleanup (slot wipe, bitmap clear, live
// count decrement, drain-winner decommit) and issues no NT calls — the
// SMR layer exposes no synchronous grace, so kernel-state lifecycle
// must live entirely in the synchronous mutator path. This invariant
// is shared across every Crystalline-W FreeFn in the va_tracker layer.
//
// The chunk_table doubles as the per-cid install mutex via a sub-aligned
// sentinel pointer; descriptors live in a flat BSS pool indexed by chunk
// id, so descriptor lifetime equals process lifetime. The 8-bit chunk id
// × 8-bit slot index encoding interleaves with the interval_skiplist
// sibling pool on diagnostic dumps and Crystalline batch-link codecs.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ART_NODE_ALLOC_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ART_NODE_ALLOC_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/new.h"
#include "src/__support/OSUtil/windows/alloc/partition_class.h"
#include "src/__support/OSUtil/windows/alloc/primitives/occupancy_bitmap.h"
#include "src/__support/OSUtil/windows/memory/art_node.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {
namespace partition {
struct PartitionDescriptor;
} // namespace partition
} // namespace alloc
namespace va_tracker {

inline constexpr uint32_t kArtMaxChunksPerType = 256;
inline constexpr uint32_t kArtSlotsPerChunk    = 256;
inline constexpr uint32_t kArtMaxNodesPerType  =
    kArtMaxChunksPerType * kArtSlotsPerChunk;

// Descriptors live in a flat BSS pool indexed by chunk id and are
// recycled across drains, so descriptor lifetime equals process
// lifetime. Backing pages live at chunk_base inside the type's pinned
// partition.
struct alignas(64) ArtChunkDesc {
  void *chunk_base = nullptr;
  uint32_t slot_size = 0;
  uint32_t slot_capacity = 0;
  // Composite (state:8 | count:24 | gen:32). The allocator count bump
  // and the releaser drain transition linearise on one 64-bit CAS so a
  // chunk transitioning Live -> Draining cannot race a peer allocator
  // into a decommit-then-memset access violation. Only the
  // {init_live, try_va_chunk_reserve, release_va_chunk_slot, load_count,
  // state_of, count_of} helpers from va_tracker_chunk_state.h may touch
  // this word — direct load/store/CAS breaks the Live↔Draining
  // linearisation.
  cpp::Atomic<uint64_t> live_state{0};
  ::LIBC_NAMESPACE::internal::alloc_primitives::AtomicBitmap<
      kArtSlotsPerChunk, /*trap_on_collision=*/true>
      occupancy;
  ::LIBC_NAMESPACE::windows::alloc::partition::PartitionDescriptor *partition =
      nullptr;
  uint64_t chunk_canary = 0;
};

struct alignas(64) PerNodeTypeState {
  cpp::Atomic<ArtChunkDesc *> chunk_table[kArtMaxChunksPerType]{};
  // Monotonic high-water mark of any cid ever published; bounds the
  // first-pass scan in art_alloc_node_raw. Drained slots are NOT
  // decremented — they stay inside [0, next_chunk_id) and are skipped
  // on subsequent scans by the null check. Reuse of drained slots
  // happens via the second-pass full-table sentinel CAS.
  cpp::Atomic<uint32_t> next_chunk_id{0};
  cpp::Atomic<uint32_t> alloc_hint{0};
  ::LIBC_NAMESPACE::windows::alloc::partition::PartitionDescriptor *partition =
      nullptr;
  uint32_t slot_size = 0;
  uint32_t chunk_bytes = 0;
};

// One-shot; not idempotent. Runs after the partition layer is online.
void art_alloc_init();

// Runs after Zone 0b cookie rotation; refreshes every live chunk's
// canary against the rotated partition_secret and resets allocation
// hints.
void art_alloc_fork_reinit();

// Returns a zeroed node-sized slot — either zeroed on free by the
// previous occupant's FreeFn or OS-zero on first commit. Caller
// placement-constructs into the returned storage. Returns nullptr if
// every chunk-table slot is full or contended.
[[nodiscard]] void *art_alloc_node_raw(ArtNodeType node_type);

// Stamp the per-slot canary after placement-new (the constructor's
// default member init would otherwise zero it). Decodes the slot
// through the same address arithmetic art_node_free uses, so it is
// sound on any node returned by art_alloc_node_raw and only such
// nodes — tagged-pointer leaves and foreign pointers trap.
void art_stamp_node_canary(ArtNodeBase *node, ArtNodeType node_type);

// art_node_free is the Crystalline FreeFn entry point; declared in
// art_node.h to keep its signature visible at the domain's
// instantiation site.

struct ArtNodeTypeStats {
  uint32_t live_chunks;
  uint32_t live_nodes;
};

// Sampled with relaxed ordering; values are advisory.
[[nodiscard]] ArtNodeTypeStats art_index_stats(ArtNodeType t);

// The Tag template parameter steers art_alloc_node_raw to the right
// per-type pool without requiring the algorithm-layer caller to know
// how node-type tags map to sub-pools.
template <class NodeT, ArtNodeType Tag>
[[nodiscard]] LIBC_INLINE NodeT *make_node(uint32_t level,
                                             const ArtPrefix &pfx) {
  void *raw = art_alloc_node_raw(Tag);
  if (raw == nullptr)
    return nullptr;
  auto *n = ::new (raw) NodeT(level, pfx);
  // Stamp after placement-new — the constructor would overwrite it.
  art_stamp_node_canary(n, Tag);
  g_va_tracker_art_domain.init_node(n);
  return n;
}

template <class NodeT, ArtNodeType Tag>
[[nodiscard]] LIBC_INLINE NodeT *make_node(uint32_t level, const uint8_t *pfx,
                                             uint32_t pfx_len) {
  void *raw = art_alloc_node_raw(Tag);
  if (raw == nullptr)
    return nullptr;
  auto *n = ::new (raw) NodeT(level, pfx, pfx_len);
  art_stamp_node_canary(n, Tag);
  g_va_tracker_art_domain.init_node(n);
  return n;
}

// Runtime dispatch over the node-type tag for grow/shrink helpers
// (Leis ICDE 2013) where the destination type is computed from the
// source type and the live-count threshold.
[[nodiscard]] ArtNodeBase *make_node_for_type(ArtNodeType t, uint32_t level,
                                                const ArtPrefix &pfx);

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ART_NODE_ALLOC_H
