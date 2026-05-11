//===- art_node_alloc.h - Slab-pool for ART internal nodes ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-type slab pool for ART internal nodes (\c ArtNode4 / 16 / 48 / 256).
///
/// The allocator backs the four ART internal-node types defined in
/// \c art_node.h with one independently-sized pool per type, satisfying
/// requests from the ROWEX tree operations in \c art_tree.cpp. Each pool
/// is backed by one pinned 4 GiB partition window
/// (\c PartitionClass::VaTrackerArtNode4..256). Chunks within a pool are
/// committed lazily and decommitted on full drain.
///
/// Retire goes through the Crystalline-W domain
/// \c g_va_tracker_art_domain (Nikolaev & Ravindran, PLDI 2024). The
/// domain's FreeFn is \c art_node_free, which performs metadata cleanup
/// only: it wipes the slot, clears the chunk's occupancy bit, decrements
/// the descriptor's live count, and on the last decrement publishes the
/// chunk's decommit. The FreeFn issues no \c nt_pal::* calls and no
/// \c NtClose; kernel-state lifecycle (placeholder split, commit, free)
/// lives in the synchronous mutator path \c art_alloc_node_raw. This is
/// the load-bearing invariant enforced across every Crystalline-W
/// FreeFn in the va_tracker layer.
///
/// Layout per node type:
///
///   * One \c PerNodeTypeState carries a \c chunk_table[256] of
///     lazy-CAS-installed \c ArtChunkDesc pointers plus a monotonic
///     \c next_chunk_id high-water hint.
///   * \c chunk_table[cid] doubles as the per-cid install mutex via the
///     \c kArtChunkInstalling sentinel.
///   * Each \c ArtChunkDesc lives in a flat BSS pool indexed by chunk
///     id; descriptor lifetime equals process lifetime.
///   * Chunks are committed lazily out of the type's pinned partition
///     via \c partition::commit_chunk. The 8-bit chunk id and 8-bit slot
///     index encode every live node identity in 16 bits.
///
/// Sizing rationale: 256 chunks of 256 slots gives 65 536 nodes per
/// type and 262 144 across all four types. The 8-bit×8-bit encoding
/// matches the chunk-id × slot-idx packing the va_tracker layer uses
/// for diagnostic dumps and for Crystalline batch-link codecs (see
/// \c BatchLinkCodec<ArtNodeBase> in \c art_node_alloc.cpp).
///
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

// Cap of 256 chunks per type × 256 slots per chunk = 65 536 nodes per
// type. The 8-bit chunk-id × 8-bit slot-idx encoding is shared with the
// interval_skiplist sibling pool so diagnostic dumps and BatchLinkCodec
// keys interleave on the same wire format.
inline constexpr uint32_t kArtMaxChunksPerType = 256;
inline constexpr uint32_t kArtSlotsPerChunk    = 256;
inline constexpr uint32_t kArtMaxNodesPerType  =
    kArtMaxChunksPerType * kArtSlotsPerChunk;

/// Descriptor for one committed chunk in a per-type pool.
///
/// Lifetime equals process lifetime — the descriptor lives in the flat
/// BSS pool \c ArtChunkDescPool and is recycled across chunk drains.
/// All mutable bookkeeping lives here; the backing pages live at
/// \p chunk_base inside the type's pinned partition.
struct alignas(64) ArtChunkDesc {
  void *chunk_base = nullptr;
  uint32_t slot_size = 0;
  uint32_t slot_capacity = 0;
  /// Composite \c (state:8 | count:24 | gen:32) — see
  /// \c va_tracker_chunk_state.h. Allocator count bump and releaser
  /// drain transition linearise on one 64-bit CAS so a chunk
  /// transitioning Live → Draining cannot race a peer allocator into
  /// a decommit-then-memset access violation.
  cpp::Atomic<uint64_t> live_state{0};
  ::LIBC_NAMESPACE::internal::alloc_primitives::AtomicBitmap<
      kArtSlotsPerChunk, /*trap_on_collision=*/true>
      occupancy;
  ::LIBC_NAMESPACE::windows::alloc::partition::PartitionDescriptor *partition =
      nullptr;
  uint64_t chunk_canary = 0;
};

/// One per node-type sub-pool; four instances total
/// (Node4 / Node16 / Node48 / Node256).
///
/// Holds the chunk table, the monotonic chunk-id high-water mark, the
/// round-robin allocation hint, and references to the type's partition
/// and slot/chunk sizing.
struct alignas(64) PerNodeTypeState {
  cpp::Atomic<ArtChunkDesc *> chunk_table[kArtMaxChunksPerType]{};
  /// High-water mark of "max chunk id ever published"; monotonic.
  ///
  /// Bounds the first-pass scan in \c art_alloc_node_raw. Updated by
  /// a monotonic CAS at chunk-install time. Drained slots are NOT
  /// decremented — they stay inside <tt>[0, next_chunk_id)</tt> and
  /// are skipped on subsequent scans by the null check. Reuse of
  /// drained slots happens via the second-pass full-table scan plus
  /// sentinel CAS.
  cpp::Atomic<uint32_t> next_chunk_id{0};
  cpp::Atomic<uint32_t> alloc_hint{0};
  ::LIBC_NAMESPACE::windows::alloc::partition::PartitionDescriptor *partition =
      nullptr;
  uint32_t slot_size = 0;
  uint32_t chunk_bytes = 0;
};

/// Reserve the four pinned 4 GiB VA windows and stamp per-type sizing.
///
/// Called from \c art_index_init during memory-primitives bring-up
/// after the partition layer comes online. One-shot; not idempotent.
void art_alloc_init();

/// Refresh every live chunk's canary against the rotated
/// \c partition_secret and reset per-type allocation hints.
///
/// Called from \c art_index_fork_reinit after Zone 0b cookie rotation.
void art_alloc_fork_reinit();

/// Allocate one zeroed node-sized slot from \p node_type's pool.
///
/// Memory is zeroed on entry — either by the previous occupant's
/// FreeFn \c art_node_free (memset on free) or by OS-zero on first
/// commit. Caller placement-constructs the node into the returned
/// storage.
///
/// \returns the slot's base address on success, or nullptr if every
///          chunk-table slot in the type's pool is either fully
///          occupied or contended by peer installers.
[[nodiscard]] void *art_alloc_node_raw(ArtNodeType node_type);

/// Stamp the per-slot canary after placement-construction.
///
/// The constructor zero-initialises \c node_canary via default member
/// initialisation; the canary's value depends on
/// <tt>(chunk_id, slot_idx)</tt> which only the allocator knows.
/// Decodes the slot through the same address arithmetic
/// \c art_node_free uses, so it is sound on any node returned by
/// \c art_alloc_node_raw — and only on such nodes; tagged-pointer
/// leaves and foreign pointers trap.
void art_stamp_node_canary(ArtNodeBase *node, ArtNodeType node_type);

// art_node_free is the Crystalline FreeFn entry point for
// g_va_tracker_art_domain. Declared in art_node.h to keep the FreeFn
// signature visible at the domain's instantiation site.

struct ArtNodeTypeStats {
  uint32_t live_chunks;
  uint32_t live_nodes;
};

/// Diagnostic snapshot of a per-type pool's live chunk count and live
/// node count. Sampled with relaxed ordering; values are advisory.
[[nodiscard]] ArtNodeTypeStats art_index_stats(ArtNodeType t);

/// Allocate, placement-construct, stamp the per-slot canary, and
/// register the node with \c g_va_tracker_art_domain.
///
/// The \c Tag template parameter steers \c art_alloc_node_raw to the
/// right per-type pool without requiring the algorithm-layer caller
/// to know how node-type tags map to sub-pools.
///
/// \returns the constructed node on success, or nullptr on chunk-pool
///          exhaustion.
template <class NodeT, ArtNodeType Tag>
[[nodiscard]] LIBC_INLINE NodeT *make_node(uint32_t level,
                                             const ArtPrefix &pfx) {
  void *raw = art_alloc_node_raw(Tag);
  if (raw == nullptr)
    return nullptr;
  auto *n = ::new (raw) NodeT(level, pfx);
  // Stamp after placement-new — the constructor would overwrite the
  // field.
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

/// Type-erased dispatch entry: route to the right \c make_node
/// specialization at runtime. Used by the algorithm-layer helpers
/// \c insertGrow / \c insertCompact / \c removeAndShrink (Leis ICDE
/// 2013 grow / shrink rules) where the destination type is computed
/// from the source type and the live-count threshold.
[[nodiscard]] ArtNodeBase *make_node_for_type(ArtNodeType t, uint32_t level,
                                                const ArtPrefix &pfx);

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ART_NODE_ALLOC_H
