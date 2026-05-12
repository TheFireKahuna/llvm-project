//===- art_node_alloc.cpp - Slab-pool allocator for ART internal nodes ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-type slab pool implementation. The allocator's two visible
// entry points (alloc and free) bracket the lifecycle of one ART node:
//
//   art_alloc_node_raw  — synchronous mutator path. Reserves a chunk
//                         (CAS-installs the sentinel, calls
//                         partition::commit_chunk), bitmap-acquires a
//                         slot, returns the zeroed slot to the caller.
//   art_node_free       — Crystalline-W FreeFn. Validates the per-slot
//                         canary, wipes the slot, clears the bitmap
//                         bit, decrements the chunk's live count, and
//                         on full drain publishes the chunk's
//                         decommit through partition::decommit_chunk.
//
// The FreeFn never issues nt_pal::* calls and never closes a handle —
// all kernel-state lifecycle (placeholder split / commit_replace /
// decommit / partition counters) is collapsed into the synchronous
// allocator paths via partition::commit_chunk and decommit_chunk. The
// FreeFn does pure metadata cleanup. This is the reclamation discipline
// the Crystalline-W layer requires (Nikolaev & Ravindran, PLDI 2024 §1
// — the algorithm is asynchronous and exposes no synchronous grace
// primitive, so reaching for nt_pal inside a FreeFn is structurally
// unachievable).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/art_node_alloc.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/partition.h"
#include "src/__support/OSUtil/windows/alloc/partition_class.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/memory/art_node.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk_state.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include "hdr/stdint_proxy.h"
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

//===----------------------------------------------------------------------===//
//  Node-type size pins
//===----------------------------------------------------------------------===//

// Sizes account for the Itanium ABI's tail-padding reuse — a derived
// class's leading members can occupy the unused trailing padding of an
// alignas(64) base when they fit. ArtNodeBase's tangible data is 56
// bytes (CrystallineNode 24 + tVLO 8 + prefix 8 + level 4 + count 2 +
// compact_count 2 + node_canary 8), leaving 8 bytes of trailing
// alignment pad.
//
//   ArtNode4   keys[4] (4 B) fits in the 8 B base tail pad; children[4]
//              (32 B) extends past → 96, rounded up to 128.
//   ArtNode16  keys[16] starts at offset 64 (node_canary occupies half
//              the base tail pad); children[16] (128 B) follows; node
//              ends at 208, rounded up to 256. Worst-case BSS impact
//              of heap-spray protection across the 65 K-node Node16
//              cap is ~4 MiB.
//   ArtNode48  child_index[256] starts at offset 64 (already past the
//              base tail pad); children[48] (384 B) follows
//              → 64 + 256 + 384 = 704.
//   ArtNode256 children[256] (2048 B) starts at offset 64
//              → 64 + 2048 = 2112.
static_assert(sizeof(ArtNode4) == 128,
              "ArtNode4 size pin: 64 hdr + 4 keys (in 8 B tail pad) + "
              "32 children + pad → 128");
static_assert(sizeof(ArtNode16) == 256,
              "ArtNode16 size pin: 64 hdr + 16 keys @ 64 + "
              "128 children → 208 → alignas(64) round → 256");
static_assert(sizeof(ArtNode48) == 704,
              "ArtNode48 size pin: 64 hdr + 256 child_index + "
              "384 children → 704");
static_assert(sizeof(ArtNode256) == 64 + 256 * 8,
              "ArtNode256 size pin: 64 hdr + 2048 children → 2112");

// The g_va_tracker_art_domain definition lives at the bottom of this
// TU: BatchLinkCodec<ArtNodeBase> needs the anonymous-namespace
// state_for_type helper, and the domain's instantiation depends on the
// codec. Ordering is anonymous-namespace state, then codec
// specialization, then the domain definition.

namespace {

//===----------------------------------------------------------------------===//
//  File-scope state
//===----------------------------------------------------------------------===//

// Four per-type pools + four flat ArtChunkDesc pools. 4 × 256 = 1024
// descriptors, 64 KiB BSS. Lifetime equals process lifetime, so the
// descriptors are CoW-inherited across fork; the only fork-time work
// is canary refresh (see art_alloc_fork_reinit). Chunk content and
// chunk_table publishes are CoW-correct in the child without further
// fixup.
PerNodeTypeState g_state_n4;
PerNodeTypeState g_state_n16;
PerNodeTypeState g_state_n48;
PerNodeTypeState g_state_n256;

// Install-in-progress sentinel for chunk_table entries.
//
// chunk_table[cid] carries one of three values:
//   nullptr             — slot is free.
//   kArtChunkInstalling — slot is reserved by an installer mid-build;
//                         concurrent allocators must skip it.
//   ArtChunkDesc *      — slot holds a fully-published descriptor.
//
// The CAS-install of the sentinel is the per-cid mutex acquisition for
// chunk creation; chunk-id reuse after drain falls out of the same
// CAS. The skiplist sibling pool uses a process-global lock table to
// solve the equivalent problem (g_link_chunk_table[256]); ART uses
// chunk_table itself as the install mutex because each
// PerNodeTypeState owns its chunk-id namespace independently and the
// global-table bookkeeping is therefore not load-bearing here.
// Sentinel value 1 is sub-aligned (every real desc is alignas(64)) so
// no real pointer can alias it.
//
// Fork safety: the sentinel may be CoW-inherited if the parent forks
// mid-install. art_alloc_fork_reinit walks chunk_table and rolls back
// any partial commit through fork_reset_sentinel_slot. The
// discriminators it uses are desc->chunk_base (pages committed?) and
// desc->partition (registered with partition layer?), written by
// build_chunk_at in the order that makes the rollback unambiguous.
ArtChunkDesc *const kArtChunkInstalling =
    reinterpret_cast<ArtChunkDesc *>(static_cast<uintptr_t>(1));

// Scan the occupancy bitmap for any unset bit and try to atomically
// acquire it.
//
// Caller must hold a chunk reservation (\c try_va_chunk_reserve) so the
// chunk's pages cannot be decommitted out from under us mid-scan.
//
// \returns the acquired slot index on success, or SIZE_MAX on
//          no-bit-available.
[[nodiscard]] LIBC_INLINE size_t
try_acquire_any_slot(ArtChunkDesc *desc) {
    constexpr size_t WORDS = decltype(desc->occupancy)::word_count;
    for (size_t w = 0; w < WORDS; ++w) {
        uint64_t free_bits =
            ~desc->occupancy.word_at<cpp::MemoryOrder::RELAXED>(w);
        while (free_bits) {
            unsigned bit =
                static_cast<unsigned>(__builtin_ctzll(free_bits));
            size_t slot_idx = w * 64u + bit;
            if (LIBC_UNLIKELY(slot_idx >= desc->slot_capacity)) {
                free_bits &= free_bits - 1;
                continue;
            }
            if (desc->occupancy.try_acquire(slot_idx))
                return slot_idx;
            free_bits &= free_bits - 1;
        }
    }
    return static_cast<size_t>(-1);
}

struct alignas(64) ArtChunkDescPool {
  ArtChunkDesc descs[kArtMaxChunksPerType];
};
ArtChunkDescPool g_chunk_pool_n4;
ArtChunkDescPool g_chunk_pool_n16;
ArtChunkDescPool g_chunk_pool_n48;
ArtChunkDescPool g_chunk_pool_n256;

LIBC_INLINE PerNodeTypeState &state_for_type(ArtNodeType t) {
  switch (t) {
  case ArtNodeType::N4:
    return g_state_n4;
  case ArtNodeType::N16:
    return g_state_n16;
  case ArtNodeType::N48:
    return g_state_n48;
  case ArtNodeType::N256:
    return g_state_n256;
  }
  __builtin_trap();
}

LIBC_INLINE ArtChunkDescPool &chunk_pool_for_type(ArtNodeType t) {
  switch (t) {
  case ArtNodeType::N4:
    return g_chunk_pool_n4;
  case ArtNodeType::N16:
    return g_chunk_pool_n16;
  case ArtNodeType::N48:
    return g_chunk_pool_n48;
  case ArtNodeType::N256:
    return g_chunk_pool_n256;
  }
  __builtin_trap();
}

// Each ART node type owns its own 4 GiB partition window. The
// partition contract — "the chunk owner is the sole writer of any
// chunk pagemap entry within the partition's range" — is satisfied
// because every chunk in a given partition is owned by the same
// PerNodeTypeState. Used by canary derivation and pagemap
// consumer_tag stamping.
LIBC_INLINE
::LIBC_NAMESPACE::windows::alloc::partition::PartitionClass
partition_class_for_type(ArtNodeType t) {
  using ::LIBC_NAMESPACE::windows::alloc::partition::PartitionClass;
  switch (t) {
  case ArtNodeType::N4:
    return PartitionClass::VaTrackerArtNode4;
  case ArtNodeType::N16:
    return PartitionClass::VaTrackerArtNode16;
  case ArtNodeType::N48:
    return PartitionClass::VaTrackerArtNode48;
  case ArtNodeType::N256:
    return PartitionClass::VaTrackerArtNode256;
  }
  __builtin_trap();
}

// Read the Zone-0b partition secret used by the canary formulas.
//
// Shared shape with va_tracker_chunk.cpp and interval_skiplist.cpp so
// per-slot canary derivation rotates in lockstep with the rest of the
// va_tracker layer on fork (Zone 0b rotation happens at the libc_fork
// reinit barrier before art_alloc_fork_reinit runs).
LIBC_INLINE uint64_t partition_secret() {
    return ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
}

// Derive a chunk-level canary from the partition secret and the chunk's
// (class_id, chunk_id) coordinates.
//
//   chunk_canary = secret ^ ((class_id << 8) | chunk_id)
//
// Identical formula in va_tracker_chunk.cpp and interval_skiplist.cpp;
// fork-reinit rotates partition_secret first and then re-derives every
// live chunk's canary against the rotated value, so the va_tracker
// layer rotates uniformly.
[[nodiscard]] uint64_t art_compute_chunk_canary(ArtNodeType node_type,
                                                  uint32_t chunk_id) {
  return ::LIBC_NAMESPACE::windows::va_tracker::compute_va_chunk_canary(
      partition_secret(),
      static_cast<uint16_t>(partition_class_for_type(node_type)),
      static_cast<uint8_t>(chunk_id));
}

// Derive a per-slot canary. A heap-spray attacker who scribbles into a
// freed-but-not-yet-reused slot can craft chunk_id / slot_idx
// arithmetic but cannot guess partition_secret. art_node_free validates
// this canary BEFORE dereferencing the chunk descriptor.
[[nodiscard]] uint64_t art_compute_node_canary(ArtNodeType node_type,
                                                 uint32_t chunk_id,
                                                 uint32_t slot_idx) {
  return ::LIBC_NAMESPACE::windows::va_tracker::compute_va_node_canary(
      partition_secret(),
      static_cast<uint16_t>(partition_class_for_type(node_type)),
      static_cast<uint8_t>(chunk_id),
      static_cast<uint8_t>(slot_idx));
}

// Build a fresh chunk at \p chunk_id under the caller's exclusive
// install reservation.
//
// Contract:
//
//   1. Caller has CAS-installed kArtChunkInstalling in
//      chunk_table[chunk_id], holding exclusive ownership of the slot
//      for the duration of the call.
//   2. On success returns the fully-initialised desc; caller publishes
//      via chunk_table[chunk_id].store(desc, RELEASE).
//   3. On failure returns nullptr; caller publishes
//      chunk_table[chunk_id].store(nullptr, RELEASE) to release.
//
// Fork-safety field ordering — load-bearing:
//
//   1. Pages first via partition::commit_chunk (split + commit_replace
//      + partition counter increment + pagemap register + pagemap
//      publish, with rollback on failure).
//   2. Stamp desc->chunk_base — fork discriminator "pages committed".
//   3. Initialise the other desc fields.
//   4. RELEASE-stamp desc->partition — fork discriminator
//      "partition registered".
//
// art_alloc_fork_reinit reads (chunk_base, partition) to decide which
// rollback steps to run. The 1-instruction window between
// commit_chunk returning and desc->partition storing leaves at most
// one chunk's worth of partition-counter drift on fork — diagnostic
// only on pinned ART partitions, which never retire.
[[nodiscard]] ArtChunkDesc *build_chunk_at(PerNodeTypeState &state,
                                              ArtNodeType node_type,
                                              uint32_t chunk_id) {
  ::LIBC_NAMESPACE::windows::alloc::partition::PartitionDescriptor *partition =
      state.partition;
  if (LIBC_UNLIKELY(partition == nullptr))
    return nullptr;

  void *chunk_base = static_cast<char *>(partition->base) +
                     ::LIBC_NAMESPACE::windows::alloc::partition::
                         kPartitionGuardBytes +
                     static_cast<uintptr_t>(chunk_id) * state.chunk_bytes;

  ArtChunkDesc *desc = &chunk_pool_for_type(node_type).descs[chunk_id];

  // Pre-zero the fork discriminators. The desc lives in a fixed BSS
  // slot; after a previous drain the fields may be stale and
  // CoW-inherited from a different parent generation.
  desc->chunk_base = nullptr;
  desc->partition = nullptr;

  // Unified chunk commit: split + commit_replace + partition counter +
  // pagemap register + pagemap publish, with rollback on failure. The
  // pagemap slot_idx packs (node_type, chunk_id) — node_type is 0..3,
  // chunk_id is 0..255, so both fit in 16 bits.
  uint32_t pagemap_slot_idx =
      (static_cast<uint32_t>(node_type) << 8) | chunk_id;
  int rc = ::LIBC_NAMESPACE::windows::alloc::partition::commit_chunk(
      partition, chunk_base, state.chunk_bytes, PAGE_READWRITE,
      ::LIBC_NAMESPACE::windows::alloc::VaChunkConsumer::VaTrackerArtChunk,
      pagemap_slot_idx);
  if (rc != 0)
    return nullptr;

  // commit_chunk succeeded. Stamp chunk_base first — fork
  // discriminator. The 1-instruction window between commit_chunk
  // returning and these stores leaves a chunk's worth of partition
  // counter inflation if fork hits there; bounded by N
  // concurrently-installing threads at fork time, diagnostic-only on
  // pinned ART partitions.
  desc->chunk_base = chunk_base;
  desc->slot_size = state.slot_size;
  desc->slot_capacity = state.chunk_bytes / state.slot_size;
  desc->occupancy.clear_all();
  desc->chunk_canary = art_compute_chunk_canary(node_type, chunk_id);
  // Composite live_state init: (Live, count=0, gen=0).
  init_live(desc->live_state);

  // RELEASE-publish the partition pointer — fork-reinit's
  // discriminator load is ACQUIRE.
  desc->partition = partition;
  return desc;
}

} // namespace

//===----------------------------------------------------------------------===//
//  Slot allocation — synchronous mutator path
//===----------------------------------------------------------------------===//

// Two-pass scan over the type's chunk table.
//
// Pass 1: walk existing chunks bounded by next_chunk_id (the monotonic
//         high-water mark). Skip null and sentinel slots; on a
//         published desc, reserve via try_va_chunk_reserve and
//         try_acquire a free bit.
//
// Pass 2: full-table scan from the hint. CAS-install
//         kArtChunkInstalling into a null slot; on success build the
//         chunk via build_chunk_at, publish desc, and monotonically
//         bump next_chunk_id.
//
// next_chunk_id is a "max-installed cid" hint, updated by monotonic
// CAS every time Pass 2 publishes a fresh chunk. Drained slots inside
// the high-water window are skipped on Pass 1 via the null check;
// reuse happens through Pass 2's full-table sentinel CAS.
void *art_alloc_node_raw(ArtNodeType node_type) {
  PerNodeTypeState &state = state_for_type(node_type);
  uint32_t hint = state.alloc_hint.fetch_add(1, cpp::MemoryOrder::RELAXED) %
                  kArtMaxChunksPerType;

  // Pass 1: existing chunks. Bounded by the high-water mark to avoid
  // touching the entire 256-entry table when only a few are installed.
  uint32_t horizon = state.next_chunk_id.load(cpp::MemoryOrder::ACQUIRE);
  if (horizon > 0) {
    for (uint32_t i = 0; i < horizon; ++i) {
      uint32_t chunk_id = (hint + i) % horizon;
      ArtChunkDesc *desc =
          state.chunk_table[chunk_id].load(cpp::MemoryOrder::ACQUIRE);
      if (LIBC_UNLIKELY(desc == nullptr))
        continue;
      if (LIBC_UNLIKELY(desc == kArtChunkInstalling))
        continue;

      // try_va_chunk_reserve atomically establishes
      // (state == Live ∧ count < cap) and increments count. The
      // reservation is the count bump; bitmap acquire follows.
      // Holding the reservation prevents concurrent drain —
      // release_va_chunk_slot's drain CAS expects count == 0 and
      // fails while we're reserved, so the chunk's pages cannot be
      // decommitted under us.
      if (!try_va_chunk_reserve(desc->live_state, desc->slot_capacity))
        continue;

      size_t acquired_slot = try_acquire_any_slot(desc);
      if (acquired_slot != static_cast<size_t>(-1)) {
        return static_cast<char *>(desc->chunk_base) +
               acquired_slot * state.slot_size;
      }
      // Bitmap raced out (peer allocators won every bit). Release
      // the reservation; on a drain win, decommit the chunk.
      if (release_va_chunk_slot(desc->live_state)) {
        state.chunk_table[chunk_id].store(nullptr,
                                            cpp::MemoryOrder::RELEASE);
        ::LIBC_NAMESPACE::windows::alloc::partition::decommit_chunk(
            state.partition, desc->chunk_base, state.chunk_bytes);
        // next_chunk_id is intentionally not decremented. The
        // high-water mark is monotonic by design — a subsequent
        // allocation may re-claim the now-null slot via Pass 2's
        // sentinel CAS — and next_chunk_id is bounded above by
        // kArtMaxChunksPerType anyway, so monotonicity is free.
      }
    }
  }

  // Pass 2: scan for null slots, CAS-install sentinel, build a fresh
  // chunk. Bounded by kArtMaxChunksPerType (the BSS-fixed chunk_table
  // size).
  for (uint32_t i = 0; i < kArtMaxChunksPerType; ++i) {
    uint32_t chunk_id = (hint + i) % kArtMaxChunksPerType;
    ArtChunkDesc *expected = nullptr;
    if (!state.chunk_table[chunk_id].compare_exchange_strong(
            expected, kArtChunkInstalling, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE)) {
      // Peer owns this slot (sentinel or published desc).
      continue;
    }

    ArtChunkDesc *desc = build_chunk_at(state, node_type, chunk_id);
    if (desc == nullptr) {
      // Build failed (placeholder split / commit_replace / partition
      // register). Release the slot for peers to retry.
      state.chunk_table[chunk_id].store(nullptr, cpp::MemoryOrder::RELEASE);
      continue;
    }
    state.chunk_table[chunk_id].store(desc, cpp::MemoryOrder::RELEASE);

    // Monotonic high-water update:
    //   next_chunk_id := max(prev, chunk_id + 1).
    // RELAXED on the CAS-failure reload — only the success edge needs
    // a happens-before on the chunk_table publish above, which is
    // already RELEASE.
    uint32_t prev = state.next_chunk_id.load(cpp::MemoryOrder::ACQUIRE);
    while (chunk_id + 1 > prev) {
      if (state.next_chunk_id.compare_exchange_weak(
              prev, chunk_id + 1, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::ACQUIRE))
        break;
    }

    // Try slot 0 first (likely free since the chunk is fresh). Peer
    // allocators may have raced in via the chunk_table publish above
    // and grabbed slot 0 first — fall through to the bitmap scan if
    // so.
    if (!try_va_chunk_reserve(desc->live_state, desc->slot_capacity))
      continue; // Chunk already drained back to Draining (extreme race).
    if (desc->occupancy.try_acquire(0))
      return desc->chunk_base;
    size_t acquired_slot = try_acquire_any_slot(desc);
    if (acquired_slot != static_cast<size_t>(-1)) {
      return static_cast<char *>(desc->chunk_base) +
             acquired_slot * state.slot_size;
    }
    // No bit available (256 peers grabbed slots 0..255 between our
    // build_chunk_at and our own try_acquire). Release reservation
    // and try another slot.
    (void)release_va_chunk_slot(desc->live_state);
  }

  // Saturated: every chunk_table slot is either full of live chunks
  // with 100% bitmaps or every install attempt collided with peers.
  // Caller surfaces as -ENOMEM.
  return nullptr;
}

//===----------------------------------------------------------------------===//
//  Crystalline-W FreeFn
//===----------------------------------------------------------------------===//

// Public entry point for g_va_tracker_art_domain. The domain template
// binds to it by address.
//
// Body is pure metadata cleanup: validate canaries, wipe the slot,
// clear the occupancy bit, decrement live_count, and on full drain
// publish the chunk's decommit via partition::decommit_chunk. No
// nt_pal::* calls; no NtClose.
void art_node_free(ArtNodeBase *node) {
  if (LIBC_UNLIKELY(node == nullptr))
    __builtin_trap();
  // Leaves are tagged Arena pointers managed by the inner-index
  // layer; they never reach this FreeFn.
  if (LIBC_UNLIKELY(art_is_leaf(node)))
    __builtin_trap();

  ArtNodeType node_type = node->node_type();
  PerNodeTypeState &state = state_for_type(node_type);

  ::LIBC_NAMESPACE::windows::alloc::partition::PartitionDescriptor *partition =
      state.partition;
  if (LIBC_UNLIKELY(partition == nullptr))
    __builtin_trap();

  uintptr_t partition_base_addr = reinterpret_cast<uintptr_t>(partition->base);
  uintptr_t node_addr = reinterpret_cast<uintptr_t>(node);
  uintptr_t off = node_addr - partition_base_addr -
                   ::LIBC_NAMESPACE::windows::alloc::partition::
                       kPartitionGuardBytes;
  uint32_t chunk_id = static_cast<uint32_t>(off / state.chunk_bytes);
  uint32_t slot_idx = static_cast<uint32_t>(
      (off - static_cast<uintptr_t>(chunk_id) * state.chunk_bytes) /
      state.slot_size);

  if (LIBC_UNLIKELY(chunk_id >= kArtMaxChunksPerType))
    __builtin_trap();
  if (LIBC_UNLIKELY(slot_idx >= kArtSlotsPerChunk))
    __builtin_trap();

  // Validate the per-slot canary BEFORE the chunk-table dereference.
  // A heap-spray attacker can craft chunk_id / slot_idx arithmetic
  // but cannot guess partition_secret; doing the check first prevents
  // a crafted slot from redirecting FreeFn into a victim chunk's
  // bitmap.
  uint64_t expected_node_canary =
      art_compute_node_canary(node_type, chunk_id, slot_idx);
  if (LIBC_UNLIKELY(node->node_canary != expected_node_canary))
    __builtin_trap();

  ArtChunkDesc *desc =
      state.chunk_table[chunk_id].load(cpp::MemoryOrder::ACQUIRE);
  if (LIBC_UNLIKELY(desc == nullptr))
    __builtin_trap();

  uint64_t expected_chunk_canary =
      art_compute_chunk_canary(node_type, chunk_id);
  if (LIBC_UNLIKELY(desc->chunk_canary != expected_chunk_canary))
    __builtin_trap();

  // Wipe slot for the next allocator round. ART nodes are trivially
  // destructible — memset zeroes everything including the
  // CrystallineNode header (next / birth_era are written fresh by the
  // next make_node).
  __builtin_memset(static_cast<void *>(node), 0, state.slot_size);

  desc->occupancy.mark_dead(slot_idx);

  // release_va_chunk_slot atomically decrements count and transitions
  // Live → Draining iff post-decrement count is 0. Returns true only
  // for the unique drain winner; the single-CAS publish closes the
  // window where an allocator's fetch_add could race past a
  // count == 0 check and have its slot pages decommitted under it.
  if (!release_va_chunk_slot(desc->live_state))
    return;

  // Unique drain winner. Clear chunk_table so new allocators stop
  // reading desc; concurrent allocators that already loaded desc
  // bounce off try_va_chunk_reserve (state is now Draining, not
  // Live). Then decommit via partition::decommit_chunk (pagemap
  // retire + decommit_preserve + counter unregister, atomic against
  // partition mutators).
  state.chunk_table[chunk_id].store(nullptr, cpp::MemoryOrder::RELEASE);
  void *chunk_base = desc->chunk_base;
  size_t chunk_bytes = state.chunk_bytes;
  ::LIBC_NAMESPACE::windows::alloc::partition::decommit_chunk(
      partition, chunk_base, chunk_bytes);
}

//===----------------------------------------------------------------------===//
//  Canary stamping and runtime dispatch
//===----------------------------------------------------------------------===//

// Decode (chunk_id, slot_idx) from the node address using the same
// arithmetic art_node_free uses, then write the per-slot canary.
// Called by make_node after placement-new (the constructor would
// otherwise overwrite the field).
void art_stamp_node_canary(ArtNodeBase *node, ArtNodeType node_type) {
  if (LIBC_UNLIKELY(node == nullptr))
    __builtin_trap();
  if (LIBC_UNLIKELY(art_is_leaf(node)))
    __builtin_trap();

  PerNodeTypeState &state = state_for_type(node_type);
  ::LIBC_NAMESPACE::windows::alloc::partition::PartitionDescriptor *partition =
      state.partition;
  if (LIBC_UNLIKELY(partition == nullptr))
    __builtin_trap();

  uintptr_t partition_base_addr = reinterpret_cast<uintptr_t>(partition->base);
  uintptr_t node_addr = reinterpret_cast<uintptr_t>(node);
  uintptr_t off = node_addr - partition_base_addr -
                   ::LIBC_NAMESPACE::windows::alloc::partition::
                       kPartitionGuardBytes;
  uint32_t chunk_id = static_cast<uint32_t>(off / state.chunk_bytes);
  uint32_t slot_idx = static_cast<uint32_t>(
      (off - static_cast<uintptr_t>(chunk_id) * state.chunk_bytes) /
      state.slot_size);
  if (LIBC_UNLIKELY(chunk_id >= kArtMaxChunksPerType))
    __builtin_trap();
  if (LIBC_UNLIKELY(slot_idx >= kArtSlotsPerChunk))
    __builtin_trap();

  node->node_canary = art_compute_node_canary(node_type, chunk_id, slot_idx);
}

// Runtime dispatch over the node-type tag. Used by the grow / shrink
// helpers in art_node.cpp where the destination type is computed at
// runtime from the current type's live count.
ArtNodeBase *make_node_for_type(ArtNodeType t, uint32_t level,
                                  const ArtPrefix &pfx) {
  switch (t) {
  case ArtNodeType::N4:
    return make_node<ArtNode4, ArtNodeType::N4>(level, pfx);
  case ArtNodeType::N16:
    return make_node<ArtNode16, ArtNodeType::N16>(level, pfx);
  case ArtNodeType::N48:
    return make_node<ArtNode48, ArtNodeType::N48>(level, pfx);
  case ArtNodeType::N256:
    return make_node<ArtNode256, ArtNodeType::N256>(level, pfx);
  }
  __builtin_trap();
}

//===----------------------------------------------------------------------===//
//  Init / fork
//===----------------------------------------------------------------------===//

namespace {

// Round chunk_bytes up to the pagemap stamp granularity (64 KiB) so
// chunk_base aligns with pagemap entry boundaries. Required by
// pagemap_register_range.
LIBC_INLINE constexpr size_t pagemap_chunk_round_up(size_t n) {
  constexpr size_t GRAN = 64u * 1024u;
  return (n + GRAN - 1) & ~(GRAN - 1);
}

void init_per_type(PerNodeTypeState &state, size_t slot_size) {
  state.slot_size = static_cast<uint32_t>(slot_size);
  size_t want_bytes = kArtSlotsPerChunk * slot_size;
  state.chunk_bytes =
      static_cast<uint32_t>(pagemap_chunk_round_up(want_bytes));
  state.next_chunk_id.store(0, cpp::MemoryOrder::RELAXED);
  state.alloc_hint.store(0, cpp::MemoryOrder::RELAXED);
}

} // namespace

void art_alloc_init() {
  using ::LIBC_NAMESPACE::windows::alloc::partition::PartitionClass;
  using ::LIBC_NAMESPACE::windows::alloc::partition::kNodeAgnostic;
  using ::LIBC_NAMESPACE::windows::alloc::partition::reserve_or_grow;

  // Each ART node type owns its own partition window, so the
  // partition contract ("chunk owner is the sole writer of any chunk
  // pagemap entry within the partition's range") holds within ART
  // trivially. The ART slots in the kCorePartitions table are
  // eager-reserved during Tier A bring-up; reserve_or_grow here is an
  // idempotent lookup.
  auto *p4 = reserve_or_grow(PartitionClass::VaTrackerArtNode4, kNodeAgnostic);
  auto *p16 = reserve_or_grow(PartitionClass::VaTrackerArtNode16,
                                kNodeAgnostic);
  auto *p48 = reserve_or_grow(PartitionClass::VaTrackerArtNode48,
                                kNodeAgnostic);
  auto *p256 = reserve_or_grow(PartitionClass::VaTrackerArtNode256,
                                 kNodeAgnostic);
  if (LIBC_UNLIKELY(p4 == nullptr || p16 == nullptr || p48 == nullptr ||
                     p256 == nullptr))
    __builtin_trap();

  init_per_type(g_state_n4, sizeof(ArtNode4));
  init_per_type(g_state_n16, sizeof(ArtNode16));
  init_per_type(g_state_n48, sizeof(ArtNode48));
  init_per_type(g_state_n256, sizeof(ArtNode256));
  g_state_n4.partition = p4;
  g_state_n16.partition = p16;
  g_state_n48.partition = p48;
  g_state_n256.partition = p256;

  // The Crystalline domain's init_registration is the responsibility
  // of va_tracker_init_fn — called before art_index_init so the
  // domain is live by the time our first make_node calls init_node
  // on the tree root. init_registration is not idempotent; do not
  // call it from here.
}

//===----------------------------------------------------------------------===//
//  Fork-time leak reclaim
//===----------------------------------------------------------------------===//

// Pre-fork, dead parent threads' Crystalline cells held retire batches
// whose art_node_free FreeFn would have cleared bitmap bits and
// decremented live_state count. In the child those threads are gone,
// so the FreeFn never runs — the bits stay set, count stays inflated,
// and the chunk's pages stay committed even when "logically empty."
//
// Discriminator: a node that was retired but whose FreeFn never ran
// has batch_link != nullptr in its CrystallineNode header (set by
// Crystalline retire() at batch close). A genuinely live node has
// batch_link == nullptr. The reclaim walks each Live chunk's bitmap;
// for each set bit whose node has non-null batch_link it runs the
// FreeFn body in place (memset + mark_dead + release_va_chunk_slot).
// Drain on the last decrement is safe because count == 0 implies no
// remaining set bits in the bitmap, so the loop terminates naturally.
//
// Only descriptors reachable via chunk_table are walked. A leaked
// per-type descriptor would manifest as a chunk_table entry pointing
// to a desc with state == Draining and non-null chunk_base; those are
// already drained and not reclaimed here. The repair targets the
// bitmap-vs-count inconsistency on Live descs.

namespace {

// Roll back a partial chunk install left by a now-dead parent thread.
//
// Called when fork-reinit observes
// chunk_table[cid] == kArtChunkInstalling but no thread is alive to
// publish desc/null. The desc itself is the state machine —
// build_chunk_at writes:
//
//   1. desc->chunk_base = chunk_base  (after pages committed)
//   2. desc->partition  = partition   (after partition register)
//
// in that order; either, both, or neither may be set in the child.
//
// Cleanup obligations by parent's progress:
//   * chunk_base == nullptr               — no decommit, no unregister.
//   * chunk_base != nullptr, partition == nullptr — decommit_preserve,
//                                            NO unregister (counters
//                                            never incremented).
//   * both != nullptr                     — full rollback.
//
// The 1-instruction window between commit_chunk returning and
// desc->partition storing leaves a chunk's worth of partition-counter
// inflation if fork hits there; bounded by N concurrently-installing
// threads, diagnostic-only on pinned ART partitions.
void fork_reset_sentinel_slot(PerNodeTypeState &state, ArtNodeType node_type,
                                 uint32_t chunk_id) {
  ArtChunkDesc *desc = &chunk_pool_for_type(node_type).descs[chunk_id];

  void *committed_base = desc->chunk_base;
  ::LIBC_NAMESPACE::windows::alloc::partition::PartitionDescriptor
      *registered_partition = desc->partition;

  // commit_chunk's atomicity collapses the old "pages committed but
  // partition not registered" intermediate state — either both
  // chunk_base and partition are stamped (full success → full
  // rollback) or neither is (commit_chunk failed → nothing to roll
  // back). The narrow window between commit_chunk returning and
  // desc->partition being stamped can still leak counter inflation;
  // the pinned-partition workaround above covers it.
  if (registered_partition != nullptr) {
    ::LIBC_NAMESPACE::windows::alloc::partition::decommit_chunk(
        registered_partition, committed_base, state.chunk_bytes);
  } else if (committed_base != nullptr) {
    // Defensive path for the narrow race above: pages committed but
    // partition pointer not yet stamped. Decommit pages + retire the
    // pagemap entry, no counter unregister (counter increment
    // accounted via the bounded leak above).
    ::LIBC_NAMESPACE::windows::alloc::pagemap_retire_range(
        committed_base, state.chunk_bytes);
    (void)::LIBC_NAMESPACE::nt_pal::decommit_preserve(committed_base,
                                                        state.chunk_bytes);
  }

  // Zero the discriminators so any future build_chunk_at at this cid
  // starts clean. The desc lives in fixed BSS; build_chunk_at also
  // pre-zeros these, but defence-in-depth keeps the slot consistent
  // for any pre-publish reader.
  desc->chunk_base = nullptr;
  desc->partition = nullptr;

  state.chunk_table[chunk_id].store(nullptr, cpp::MemoryOrder::RELEASE);
}

// Finish an art_node_free teardown that the parent started but didn't
// complete before fork — release_va_chunk_slot returned true but the
// parent was scheduled out before chunk_table.store(nullptr) or
// before decommit. Idempotent against the case where the parent
// finished chunk_table.store but not decommit.
void finish_interrupted_drain(PerNodeTypeState &state, uint32_t chunk_id,
                                ArtChunkDesc *desc) {
  state.chunk_table[chunk_id].store(nullptr, cpp::MemoryOrder::RELEASE);
  if (desc->chunk_base != nullptr) {
    ::LIBC_NAMESPACE::windows::alloc::partition::decommit_chunk(
        state.partition, desc->chunk_base, state.chunk_bytes);
  }
}

// Walk a Live chunk's bitmap and reclaim any retired-but-not-freed
// nodes left over from dead parent threads' Crystalline cells. Runs
// the FreeFn body in place for each such slot (memset + mark_dead +
// release_va_chunk_slot). See the discipline note above the namespace.
void fork_reclaim_art_chunk(PerNodeTypeState &state, uint32_t chunk_id,
                              ArtChunkDesc *desc) {
  // Guard against descs already in a terminal drain state — happens
  // when a parent thread was mid-art_node_free at fork time. Skip
  // the bitmap walk (release_va_chunk_slot would trap on count == 0)
  // and finish the teardown the parent didn't.
  uint64_t snap = desc->live_state.load(cpp::MemoryOrder::ACQUIRE);
  if (state_of(snap) != static_cast<uint8_t>(VaChunkState::Live) ||
      count_of(snap) == 0) {
    finish_interrupted_drain(state, chunk_id, desc);
    return;
  }

  constexpr uint32_t kBitsPerWord = 64;
  const uint32_t cap_bits = desc->slot_capacity;
  const uint32_t word_count =
      (cap_bits + kBitsPerWord - 1) / kBitsPerWord;
  uintptr_t base = reinterpret_cast<uintptr_t>(desc->chunk_base);
  for (uint32_t w = 0; w < word_count; ++w) {
    uint64_t bits =
        desc->occupancy.template word_at<cpp::MemoryOrder::RELAXED>(w);
    const uint32_t word_lo = w * kBitsPerWord;
    if (word_lo + kBitsPerWord > cap_bits) {
      const uint32_t valid = cap_bits - word_lo;
      bits &= (valid == kBitsPerWord) ? ~uint64_t{0}
                                       : ((uint64_t{1} << valid) - 1);
    }
    while (bits != 0) {
      const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(bits));
      bits &= bits - 1;
      const uint32_t slot = word_lo + bit;
      void *slot_ptr = reinterpret_cast<void *>(
          base + static_cast<uintptr_t>(slot) *
                     static_cast<uintptr_t>(state.slot_size));
      // CrystallineNode is an empty tag base; the intrusive runtime
      // fields (incl. `batch_link`) live in the derived class via
      // LIBC_CRYSTALLINE_NODE_FIELDS, so we cast to ArtNodeBase to
      // reach `batch_link`. All ART variants derive from ArtNodeBase
      // so the cast is well-defined for any slot in this chunk.
      auto *cnode = reinterpret_cast<ArtNodeBase *>(slot_ptr);
      if (cnode->batch_link.load(cpp::MemoryOrder::ACQUIRE) == 0u)
        continue; // Genuinely live — leave alone.

      // Retired but FreeFn never ran. Mimic art_node_free's body.
      __builtin_memset(slot_ptr, 0, state.slot_size);
      desc->occupancy.mark_dead(slot);
      if (release_va_chunk_slot(desc->live_state)) {
        // Drain win — same teardown as art_node_free's drain branch.
        // count == 0 implies no remaining set bits in the bitmap, so
        // the outer loop terminates without further access to the
        // now-decommitted chunk_base.
        state.chunk_table[chunk_id].store(nullptr,
                                            cpp::MemoryOrder::RELEASE);
        ::LIBC_NAMESPACE::windows::alloc::partition::decommit_chunk(
            state.partition, desc->chunk_base, state.chunk_bytes);
        return;
      }
    }
  }
}

} // namespace

void art_alloc_fork_reinit() {
  g_state_n4.alloc_hint.store(0, cpp::MemoryOrder::RELAXED);
  g_state_n16.alloc_hint.store(0, cpp::MemoryOrder::RELAXED);
  g_state_n48.alloc_hint.store(0, cpp::MemoryOrder::RELAXED);
  g_state_n256.alloc_hint.store(0, cpp::MemoryOrder::RELAXED);

  for (uint32_t t = 0; t < 4; ++t) {
    ArtNodeType nt = static_cast<ArtNodeType>(t);
    PerNodeTypeState &state = state_for_type(nt);
    for (uint32_t i = 0; i < kArtMaxChunksPerType; ++i) {
      ArtChunkDesc *desc =
          state.chunk_table[i].load(cpp::MemoryOrder::RELAXED);
      if (desc == nullptr)
        continue;
      // Sentinel cleanup: parent was mid-install at fork time. Roll
      // back any committed pages / partition register and clear the
      // sentinel. Must run before canary refresh and before
      // fork_reclaim_art_chunk — both would otherwise dereference
      // the sentinel value (ArtChunkDesc *)1 as a real desc.
      if (desc == kArtChunkInstalling) {
        fork_reset_sentinel_slot(state, nt, i);
        continue;
      }
      desc->chunk_canary = art_compute_chunk_canary(nt, i);
      // Per-slot bitmap walk doing two repairs in one pass:
      //
      //   (1) Per-slot canary refresh: re-stamp every live node's
      //       canary against the rotated partition_secret. Retired-
      //       but-unreaped slots (batch_link non-null) are also
      //       re-stamped; the reclaim path's memset clears the field
      //       afterward.
      //
      //   (2) ROWEX writer-lock scrub: if a parent thread held an
      //       ART writer lock at fork time, the lock bit (bit 1) of
      //       tVLO is set in the child but no thread is alive to
      //       release it. Child writers calling
      //       write_lock_or_restart would spin forever on
      //       is_locked(v). Clear via fetch_add(0b10), the canonical
      //       writeUnlock sequence — clears bit 1, carries into the
      //       low bit of the version counter, preserves bit 0
      //       (obsolete). Filtered to live nodes (batch_link == 0);
      //       retired slots will be memset by fork_reclaim_art_chunk
      //       so their lock state is moot.
      //
      //       The forking thread cannot be the lock holder — its
      //       stack at fork-reinit time is inside libc_fork_dispatch,
      //       not inside any ART operation. Every locked tVLO
      //       observed here is owned by a dead thread.
      //
      // FIXME: if the dead thread was mid-write under lock, the
      // node's children/keys may carry partial state. After unlock +
      // version-bump, readers won't restart and may observe the
      // partial state. ART writes hold the lock for ~µs and fork
      // mid-write is rare; the alternative (fetch_add(0b11) → mark
      // obsolete) would force readers to restart from a parent that
      // may no longer exist. Upgrade to obsolete-marking if soak
      // reveals correctness fallout.
      //
      // Both repairs must run BEFORE fork_reclaim_art_chunk — that
      // helper invokes the FreeFn body which validates node_canary.
      {
        constexpr uint32_t kBitsPerWord = 64;
        const uint32_t cap_bits = desc->slot_capacity;
        const uint32_t word_count =
            (cap_bits + kBitsPerWord - 1) / kBitsPerWord;
        uintptr_t base = reinterpret_cast<uintptr_t>(desc->chunk_base);
        for (uint32_t w = 0; w < word_count; ++w) {
          uint64_t bits =
              desc->occupancy.template word_at<cpp::MemoryOrder::RELAXED>(w);
          const uint32_t word_lo = w * kBitsPerWord;
          if (word_lo + kBitsPerWord > cap_bits) {
            const uint32_t valid = cap_bits - word_lo;
            bits &= (valid == kBitsPerWord) ? ~uint64_t{0}
                                             : ((uint64_t{1} << valid) - 1);
          }
          while (bits != 0) {
            const uint32_t bit =
                static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            const uint32_t slot = word_lo + bit;
            ArtNodeBase *n = reinterpret_cast<ArtNodeBase *>(
                base + static_cast<uintptr_t>(slot) *
                           static_cast<uintptr_t>(state.slot_size));
            n->node_canary = art_compute_node_canary(nt, i, slot);

            // Lock-scrub on live nodes only. `batch_link` lives on
            // ArtNodeBase directly (CrystallineNode is an empty tag);
            // `n` is already ArtNodeBase* so a separate downcast isn't
            // needed.
            if (n->batch_link.load(cpp::MemoryOrder::ACQUIRE) == 0u) {
              uint64_t v = n->typeVersionLockObsolete.load(
                  cpp::MemoryOrder::ACQUIRE);
              if (ArtNodeBase::is_locked(v)) {
                // fetch_add(0b10) is the canonical writeUnlock:
                // clears bit 1 (lock), carries into bit 2
                // (version[0]), preserves bit 0 (obsolete).
                // Single-threaded in the child during fork-reinit so
                // the load+CAS race window doesn't exist; fetch_add
                // is used for hardware-ordering parity with normal
                // write_unlock.
                n->typeVersionLockObsolete.fetch_add(
                    0b10ULL, cpp::MemoryOrder::ACQ_REL);
              }
            }
          }
        }
      }
      // Reclaim retired-but-not-freed nodes left over from dead
      // threads' Crystalline cells. Must run after chunk_canary
      // refresh — release_va_chunk_slot's drain path doesn't
      // validate canary, but defence-in-depth keeps the descriptor
      // consistent for any later observers.
      fork_reclaim_art_chunk(state, i, desc);
    }
  }
}

ArtNodeTypeStats art_index_stats(ArtNodeType t) {
  ArtNodeTypeStats s = {0, 0};
  PerNodeTypeState &state = state_for_type(t);
  for (uint32_t i = 0; i < kArtMaxChunksPerType; ++i) {
    ArtChunkDesc *desc = state.chunk_table[i].load(cpp::MemoryOrder::RELAXED);
    if (desc == nullptr)
      continue;
    if (desc == kArtChunkInstalling)
      continue; // Mid-install sentinel — not a published chunk.
    ++s.live_chunks;
    s.live_nodes += load_count(desc->live_state);
  }
  return s;
}

} // namespace va_tracker
} // namespace windows

//===----------------------------------------------------------------------===//
//  BatchLinkCodec<ArtNodeBase>
//===----------------------------------------------------------------------===//

// Out-of-line bodies for the Crystalline batch-link codec specialised
// to ArtNodeBase. Declarations live in art_node.h; the bodies need
// access to the anonymous-namespace state_for_type helper defined
// above, so they live here.
namespace concurrent {

uint32_t
BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::ArtNodeBase>::encode(
    CrystallineNode *n) noexcept {
  using Node = ::LIBC_NAMESPACE::windows::va_tracker::ArtNodeBase;
  auto *node = static_cast<Node *>(n);
  auto type = node->node_type();
  auto &state = ::LIBC_NAMESPACE::windows::va_tracker::state_for_type(type);
  uintptr_t partition_base =
      reinterpret_cast<uintptr_t>(state.partition->base);
  uintptr_t off = reinterpret_cast<uintptr_t>(node) - partition_base -
                   ::LIBC_NAMESPACE::windows::alloc::partition::
                       kPartitionGuardBytes;
  uint32_t chunk_id = static_cast<uint32_t>(off / state.chunk_bytes);
  uint32_t slot_idx = static_cast<uint32_t>(
      (off - static_cast<uintptr_t>(chunk_id) * state.chunk_bytes) /
      state.slot_size);
  return 1u + ((static_cast<uint32_t>(type) << 16) |
               (chunk_id << 8) | slot_idx);
}

CrystallineNode *
BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::ArtNodeBase>::decode(
    uint32_t code) noexcept {
  using Node = ::LIBC_NAMESPACE::windows::va_tracker::ArtNodeBase;
  uint32_t v = code - 1u;
  auto type = static_cast<::LIBC_NAMESPACE::windows::va_tracker::ArtNodeType>(
      (v >> 16) & 0x3u);
  uint32_t chunk_id = (v >> 8) & 0xFFu;
  uint32_t slot_idx = v & 0xFFu;
  auto &state = ::LIBC_NAMESPACE::windows::va_tracker::state_for_type(type);
  auto *desc = state.chunk_table[chunk_id].load(cpp::MemoryOrder::ACQUIRE);
  auto *base = static_cast<char *>(desc->chunk_base);
  return reinterpret_cast<Node *>(base + slot_idx * state.slot_size);
}

} // namespace concurrent

namespace windows {
namespace va_tracker {

// Crystalline domain definition. Declaration is `extern` in art_node.h.
// Defined here at file-end so the BatchLinkCodec<ArtNodeBase>
// specialization above (which references state_for_type) is in scope
// at instantiation time.
::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    ArtNodeBase, &art_node_free, kArtRetireFreq, kArtMaxIdx>
    g_va_tracker_art_domain;

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
