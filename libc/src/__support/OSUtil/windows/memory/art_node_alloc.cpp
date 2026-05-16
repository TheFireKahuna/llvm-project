//===- art_node_alloc.cpp - Slab-pool allocator for ART internal nodes ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-type slab pool implementation. The two visible entry points
// bracket one ART node's lifecycle: art_alloc_node_raw is the
// synchronous mutator path (chunk install + bitmap acquire);
// art_node_free is the Crystalline-W FreeFn (metadata cleanup +
// drain-winner decommit). The FreeFn never touches nt_pal — the SMR
// layer exposes no synchronous grace, so kernel-state lifecycle has
// to live in the synchronous mutator path (Nikolaev & Ravindran,
// PLDI 2024).
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

// Size pins below assume Itanium ABI tail-padding reuse: a derived
// class's leading members can occupy the unused trailing padding of an
// alignas(64) base when they fit. ArtNodeBase has 56 B of tangible
// data, leaving 8 B of trailing alignment pad that ArtNode4's keys[4]
// occupy in place.
static_assert(sizeof(ArtNode4) == 128,
              "ArtNode4: 64 hdr + 4 keys (in 8 B tail pad) + 32 children -> 128");
static_assert(sizeof(ArtNode16) == 256,
              "ArtNode16: 64 hdr + 16 keys + 128 children -> 208 -> align(64) -> 256");
static_assert(sizeof(ArtNode48) == 704,
              "ArtNode48: 64 hdr + 256 child_index + 384 children -> 704");
static_assert(sizeof(ArtNode256) == 64 + 256 * 8,
              "ArtNode256: 64 hdr + 2048 children -> 2112");

namespace {

//===----------------------------------------------------------------------===//
//  File-scope state
//===----------------------------------------------------------------------===//

// Process-lifetime BSS — CoW-inherited across fork, so fork-time work
// is canary refresh only (see art_alloc_fork_reinit).
PerNodeTypeState g_state_n4;
PerNodeTypeState g_state_n16;
PerNodeTypeState g_state_n48;
PerNodeTypeState g_state_n256;

// chunk_table[cid] carries one of: nullptr (free), kArtChunkInstalling
// (installer mid-build; concurrent allocators must skip), or a
// published ArtChunkDesc*. The CAS-install of the sentinel is the
// per-cid mutex acquisition for chunk creation, and chunk-id reuse
// after drain falls out of the same CAS — no separate lock table is
// needed because each PerNodeTypeState owns its chunk-id namespace.
// Sentinel value 1 is sub-aligned (every real desc is alignas(64)) so
// no real pointer can alias it.
//
// On fork mid-install the sentinel is CoW-inherited; fork-reinit walks
// chunk_table and rolls back any partial commit using desc->chunk_base
// (pages committed?) and desc->partition (registered?) — the two
// discriminators build_chunk_at writes in rollback-unambiguous order.
ArtChunkDesc *const kArtChunkInstalling =
    reinterpret_cast<ArtChunkDesc *>(static_cast<uintptr_t>(1));

// Caller must hold a chunk reservation (try_va_chunk_reserve) so the
// chunk's pages cannot be decommitted out from under us mid-scan.
// Returns SIZE_MAX on no-bit-available.
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

// Each ART node type owns its own 4 GiB partition window, satisfying
// the partition contract ("chunk owner is the sole writer of any chunk
// pagemap entry within the partition's range") trivially — every chunk
// in a given partition is owned by the same PerNodeTypeState.
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

// Zone 0b rotates the partition secret at the libc_fork reinit
// barrier before art_alloc_fork_reinit runs, so a fresh read here
// reflects the post-rotation value.
LIBC_INLINE uint64_t partition_secret() {
    return ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
}

[[nodiscard]] uint64_t art_compute_chunk_canary(ArtNodeType node_type,
                                                  uint32_t chunk_id) {
  return ::LIBC_NAMESPACE::windows::va_tracker::compute_va_chunk_canary(
      partition_secret(),
      static_cast<uint16_t>(partition_class_for_type(node_type)),
      static_cast<uint8_t>(chunk_id));
}

// art_node_free validates this canary BEFORE dereferencing the chunk
// descriptor: a heap-spray attacker can craft (chunk_id, slot_idx)
// arithmetic but cannot guess partition_secret.
[[nodiscard]] uint64_t art_compute_node_canary(ArtNodeType node_type,
                                                 uint32_t chunk_id,
                                                 uint32_t slot_idx) {
  return ::LIBC_NAMESPACE::windows::va_tracker::compute_va_node_canary(
      partition_secret(),
      static_cast<uint16_t>(partition_class_for_type(node_type)),
      static_cast<uint8_t>(chunk_id),
      static_cast<uint8_t>(slot_idx));
}

// Caller must have CAS-installed kArtChunkInstalling in
// chunk_table[chunk_id] and is responsible for publishing the result:
// store(desc, RELEASE) on success, store(nullptr, RELEASE) on the
// nullptr return.
//
// Fork-safety field ordering is load-bearing — art_alloc_fork_reinit
// reads (chunk_base, partition) to decide rollback. The store order
// must be: pages via commit_chunk, then desc->chunk_base ("committed"
// discriminator), then other fields, then RELEASE-store partition
// ("registered" discriminator). The 1-instruction window between
// commit_chunk returning and partition storing can leak one chunk's
// worth of partition-counter inflation on fork — diagnostic only on
// pinned ART partitions, which never retire.
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

  // Pre-zero discriminators — the desc lives in fixed BSS and stale
  // values may be CoW-inherited from a different parent generation.
  desc->chunk_base = nullptr;
  desc->partition = nullptr;

  // pagemap slot_idx packs node_type (0..3) and chunk_id (0..255) — 16
  // bits total.
  uint32_t pagemap_slot_idx =
      (static_cast<uint32_t>(node_type) << 8) | chunk_id;
  int rc = ::LIBC_NAMESPACE::windows::alloc::partition::commit_chunk(
      partition, chunk_base, state.chunk_bytes, PAGE_READWRITE,
      ::LIBC_NAMESPACE::windows::alloc::VaChunkConsumer::VaTrackerArtChunk,
      pagemap_slot_idx);
  if (rc != 0)
    return nullptr;

  // Stamp the "pages committed" discriminator first.
  desc->chunk_base = chunk_base;
  desc->slot_size = state.slot_size;
  desc->slot_capacity = state.chunk_bytes / state.slot_size;
  desc->occupancy.clear_all();
  desc->chunk_canary = art_compute_chunk_canary(node_type, chunk_id);
  init_live(desc->live_state);

  // RELEASE pairs with fork-reinit's ACQUIRE discriminator load.
  desc->partition = partition;
  return desc;
}

} // namespace

//===----------------------------------------------------------------------===//
//  Slot allocation — synchronous mutator path
//===----------------------------------------------------------------------===//

// Two-pass scan: Pass 1 walks existing chunks bounded by
// next_chunk_id; Pass 2 CAS-installs the sentinel and builds a fresh
// chunk. Pass 1 covers the common path; Pass 2 also handles reuse of
// drained slots (next_chunk_id stays monotonic, so drained cids
// inside the horizon are skipped on Pass 1 via the null check).
void *art_alloc_node_raw(ArtNodeType node_type) {
  PerNodeTypeState &state = state_for_type(node_type);
  uint32_t hint = state.alloc_hint.fetch_add(1, cpp::MemoryOrder::RELAXED) %
                  kArtMaxChunksPerType;

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

      // try_va_chunk_reserve atomically asserts (state == Live and
      // count < cap) and bumps count. Holding the reservation blocks
      // concurrent drain — release_va_chunk_slot's drain CAS expects
      // count == 0 and fails while we're reserved — so the chunk's
      // pages cannot be decommitted out from under our bitmap scan.
      if (!try_va_chunk_reserve(desc->live_state, desc->slot_capacity))
        continue;

      size_t acquired_slot = try_acquire_any_slot(desc);
      if (acquired_slot != static_cast<size_t>(-1)) {
        return static_cast<char *>(desc->chunk_base) +
               acquired_slot * state.slot_size;
      }
      // Bitmap raced out — peers won every bit. Release reservation;
      // on a drain win, decommit. next_chunk_id is intentionally NOT
      // decremented — monotonicity is bounded above by
      // kArtMaxChunksPerType and the null slot reopens via Pass 2.
      if (release_va_chunk_slot(desc->live_state)) {
        state.chunk_table[chunk_id].store(nullptr,
                                            cpp::MemoryOrder::RELEASE);
        ::LIBC_NAMESPACE::windows::alloc::partition::decommit_chunk(
            state.partition, desc->chunk_base, state.chunk_bytes);
      }
    }
  }

  for (uint32_t i = 0; i < kArtMaxChunksPerType; ++i) {
    uint32_t chunk_id = (hint + i) % kArtMaxChunksPerType;
    ArtChunkDesc *expected = nullptr;
    if (!state.chunk_table[chunk_id].compare_exchange_strong(
            expected, kArtChunkInstalling, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE)) {
      continue;
    }

    ArtChunkDesc *desc = build_chunk_at(state, node_type, chunk_id);
    if (desc == nullptr) {
      // Release the slot for peers to retry.
      state.chunk_table[chunk_id].store(nullptr, cpp::MemoryOrder::RELEASE);
      continue;
    }
    state.chunk_table[chunk_id].store(desc, cpp::MemoryOrder::RELEASE);

    // Monotonic max(prev, chunk_id + 1). Only the success edge needs
    // happens-before on the chunk_table publish above, already RELEASE.
    uint32_t prev = state.next_chunk_id.load(cpp::MemoryOrder::ACQUIRE);
    while (chunk_id + 1 > prev) {
      if (state.next_chunk_id.compare_exchange_weak(
              prev, chunk_id + 1, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::ACQUIRE))
        break;
    }

    // Try slot 0 — likely free on a fresh chunk; fall through if a
    // peer raced in via the publish above and grabbed it.
    if (!try_va_chunk_reserve(desc->live_state, desc->slot_capacity))
      continue;
    if (desc->occupancy.try_acquire(0))
      return desc->chunk_base;
    size_t acquired_slot = try_acquire_any_slot(desc);
    if (acquired_slot != static_cast<size_t>(-1)) {
      return static_cast<char *>(desc->chunk_base) +
             acquired_slot * state.slot_size;
    }
    (void)release_va_chunk_slot(desc->live_state);
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
//  Crystalline-W FreeFn
//===----------------------------------------------------------------------===//

// FreeFn for g_va_tracker_art_domain — bound by address from the
// domain template. Pure metadata cleanup; no nt_pal calls, no NtClose
// (see file banner).
void art_node_free(ArtNodeBase *node) {
  if (LIBC_UNLIKELY(node == nullptr))
    __builtin_trap();
  // Leaves are tagged Arena pointers owned by the inner-index layer.
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

  // Validate the per-slot canary BEFORE the chunk-table dereference —
  // a crafted (chunk_id, slot_idx) must not be allowed to redirect us
  // into a victim chunk's bitmap.
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

  // ART nodes are trivially destructible; the next make_node rewrites
  // the CrystallineNode header (next, birth_era) fresh.
  __builtin_memset(static_cast<void *>(node), 0, state.slot_size);

  desc->occupancy.mark_dead(slot_idx);

  // release_va_chunk_slot's single-CAS atomically decrements count and
  // transitions Live -> Draining iff post-decrement count is 0,
  // returning true only for the unique drain winner. The single-CAS
  // closes the window where a peer allocator's fetch_add could race
  // past a count == 0 check and end up with its pages decommitted.
  if (!release_va_chunk_slot(desc->live_state))
    return;

  // Drain winner. Clear chunk_table so new allocators stop reading
  // desc; concurrent allocators that already loaded desc bounce off
  // try_va_chunk_reserve (state is Draining now, not Live).
  state.chunk_table[chunk_id].store(nullptr, cpp::MemoryOrder::RELEASE);
  void *chunk_base = desc->chunk_base;
  size_t chunk_bytes = state.chunk_bytes;
  ::LIBC_NAMESPACE::windows::alloc::partition::decommit_chunk(
      partition, chunk_base, chunk_bytes);
}

//===----------------------------------------------------------------------===//
//  Canary stamping and runtime dispatch
//===----------------------------------------------------------------------===//

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

// pagemap_register_range requires chunk_base to align with the 64 KiB
// pagemap stamp granularity.
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

  // ART partitions are eager-reserved in kCorePartitions during Tier A
  // bring-up; reserve_or_grow here is an idempotent lookup.
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

  // The Crystalline domain's init_registration is owned by
  // va_tracker_init_fn, which runs before us so the domain is live by
  // our first make_node. init_registration is NOT idempotent — do
  // not call it from here.
}

//===----------------------------------------------------------------------===//
//  Fork-time leak reclaim
//===----------------------------------------------------------------------===//

// Fork-time leak reclaim. Dead parent threads' Crystalline cells held
// retire batches whose FreeFn would have cleared bitmap bits and
// decremented live_state count; in the child those threads are gone,
// the FreeFn never runs, and bits stay set with count inflated.
//
// The discriminator is the CrystallineNode header field batch_link:
// retire() sets it non-null at batch close, so a node with
// batch_link != 0 was retired but never reaped. The reclaim runs the
// FreeFn body in place (memset + mark_dead + release_va_chunk_slot)
// for each such slot. Drain-on-last-decrement terminates the loop
// safely — count == 0 implies no remaining set bits.

namespace {

// Roll back a partial chunk install observed mid-build at fork time
// (chunk_table[cid] == kArtChunkInstalling, no thread alive to
// finish). Cleanup obligation reads off the two build_chunk_at
// discriminators:
//   * partition != nullptr               — full rollback via decommit_chunk.
//   * chunk_base != nullptr, partition == nullptr — pages committed
//     but counters never incremented; decommit_preserve + pagemap retire.
//   * both nullptr                       — nothing to roll back.
//
// The narrow window between commit_chunk returning and partition
// storing can leak one chunk of counter inflation per concurrently-
// installing thread; diagnostic-only on pinned ART partitions.
void fork_reset_sentinel_slot(PerNodeTypeState &state, ArtNodeType node_type,
                                 uint32_t chunk_id) {
  ArtChunkDesc *desc = &chunk_pool_for_type(node_type).descs[chunk_id];

  void *committed_base = desc->chunk_base;
  ::LIBC_NAMESPACE::windows::alloc::partition::PartitionDescriptor
      *registered_partition = desc->partition;

  if (registered_partition != nullptr) {
    ::LIBC_NAMESPACE::windows::alloc::partition::decommit_chunk(
        registered_partition, committed_base, state.chunk_bytes);
  } else if (committed_base != nullptr) {
    ::LIBC_NAMESPACE::windows::alloc::pagemap_retire_range(
        committed_base, state.chunk_bytes);
    (void)::LIBC_NAMESPACE::nt_pal::decommit_preserve(committed_base,
                                                        state.chunk_bytes);
  }

  // Zero discriminators — defence-in-depth for any pre-publish reader;
  // build_chunk_at pre-zeros too.
  desc->chunk_base = nullptr;
  desc->partition = nullptr;

  state.chunk_table[chunk_id].store(nullptr, cpp::MemoryOrder::RELEASE);
}

// Finish an art_node_free teardown the parent started but didn't
// complete — release_va_chunk_slot returned true but the parent was
// scheduled out before chunk_table.store(nullptr) or decommit.
// Idempotent if chunk_table.store already happened.
void finish_interrupted_drain(PerNodeTypeState &state, uint32_t chunk_id,
                                ArtChunkDesc *desc) {
  state.chunk_table[chunk_id].store(nullptr, cpp::MemoryOrder::RELEASE);
  if (desc->chunk_base != nullptr) {
    ::LIBC_NAMESPACE::windows::alloc::partition::decommit_chunk(
        state.partition, desc->chunk_base, state.chunk_bytes);
  }
}

// Walk a Live chunk's bitmap and run the FreeFn body in place for any
// retired-but-not-freed slot (memset + mark_dead +
// release_va_chunk_slot). See block comment above the namespace.
void fork_reclaim_art_chunk(PerNodeTypeState &state, uint32_t chunk_id,
                              ArtChunkDesc *desc) {
  // Descs already in a terminal drain state (parent was
  // mid-art_node_free at fork) skip the bitmap walk —
  // release_va_chunk_slot would trap on count == 0.
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
      // batch_link lives on ArtNodeBase (CrystallineNode is an empty
      // tag base — intrusive fields are injected via
      // LIBC_CRYSTALLINE_NODE_FIELDS into the derived class).
      auto *cnode = reinterpret_cast<ArtNodeBase *>(slot_ptr);
      if (cnode->batch_link.load(cpp::MemoryOrder::ACQUIRE) == 0u)
        continue;

      __builtin_memset(slot_ptr, 0, state.slot_size);
      desc->occupancy.mark_dead(slot);
      if (release_va_chunk_slot(desc->live_state)) {
        // count == 0 implies no remaining set bits, so the outer loop
        // terminates without further access to the decommitted base.
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
      // Sentinel cleanup must precede canary refresh and reclaim —
      // both would otherwise dereference (ArtChunkDesc *)1 as a real
      // desc.
      if (desc == kArtChunkInstalling) {
        fork_reset_sentinel_slot(state, nt, i);
        continue;
      }
      desc->chunk_canary = art_compute_chunk_canary(nt, i);
      // Two repairs in one bitmap walk, both must precede
      // fork_reclaim_art_chunk (whose FreeFn body validates
      // node_canary):
      //
      //   (1) Per-slot canary refresh against the rotated
      //       partition_secret. Retired-but-unreaped slots are also
      //       re-stamped; the reclaim path's memset clears the field
      //       afterward.
      //
      //   (2) ROWEX writer-lock scrub. A parent thread that held an
      //       ART writer lock at fork left bit 1 of tVLO set in the
      //       child with no live releaser; child writers would spin
      //       forever in write_lock_or_restart. fetch_add(0b10) is
      //       the canonical writeUnlock — clears bit 1, carries into
      //       version[0], preserves bit 0 (obsolete). The forking
      //       thread cannot itself be the lock holder (its stack is
      //       inside libc_fork_dispatch, not any ART op), so every
      //       locked tVLO observed here is owned by a dead thread.
      //       Filtered to live nodes — retired slots get memset
      //       below.
      //
      // FIXME: if the dead thread was mid-write under lock, the node
      // may carry partial children/keys; after unlock + version-bump,
      // readers won't restart and may observe partial state. ART
      // writes hold for ~µs and fork-mid-write is rare; the
      // alternative (fetch_add(0b11) -> mark obsolete) forces readers
      // to restart from a parent that may no longer exist. Upgrade if
      // soak reveals correctness fallout.
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

            if (n->batch_link.load(cpp::MemoryOrder::ACQUIRE) == 0u) {
              uint64_t v = n->typeVersionLockObsolete.load(
                  cpp::MemoryOrder::ACQUIRE);
              if (ArtNodeBase::is_locked(v)) {
                // Child is single-threaded so the load+CAS race
                // doesn't exist; fetch_add is used for
                // hardware-ordering parity with normal write_unlock.
                n->typeVersionLockObsolete.fetch_add(
                    0b10ULL, cpp::MemoryOrder::ACQ_REL);
              }
            }
          }
        }
      }
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
      continue;
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

// Bodies for the Crystalline batch-link codec specialised to
// ArtNodeBase. Located here (not in art_node.cpp) because they need
// the anonymous-namespace state_for_type helper.
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

// Defined at file-end so the BatchLinkCodec<ArtNodeBase> specialization
// above (which the domain instantiation drags in) is already in scope.
// extern declaration lives in art_node.h.
::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    ArtNodeBase, &art_node_free, kArtRetireFreq, kArtMaxIdx>
    g_va_tracker_art_domain;

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
