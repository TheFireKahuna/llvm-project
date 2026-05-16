//===- va_tracker_chunk.h - Shared per-region chunk allocator ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-2-GiB-region chunks shared by the va_tracker interval skiplist
// (Kim/Kwon/Kang, SOSP 2025) and the ROWEX ART (Leis 2016). Hosts the
// process-lifetime VaChunkDesc pool, the commit driver, the drain
// orchestrator, and the two Crystalline-W chunk domains.
//
// The skiplist-side and ART-side domains share the same FreeFn but have
// independent grace machinery so reader-race windows do not couple.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_TRACKER_CHUNK_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_TRACKER_CHUNK_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/OSUtil/windows/alloc/partition.h"
#include "src/__support/OSUtil/windows/alloc/partition_class.h"
#include "src/__support/OSUtil/windows/alloc/primitives/occupancy_bitmap.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk_state.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

//===----------------------------------------------------------------------===//
//  Pool-bucket-id taxonomy
//===----------------------------------------------------------------------===//

// One flat VaChunkDesc pool serves every consumer; `bucket_id` identifies
// the owning PartitionClass for canary derivation and selects the
// retire-side Crystalline domain (skiplist 0..6 vs ART 7..10). The bucket
// ids are dense so `va_class_for_pool_bucket` is a single switch.
inline constexpr uint8_t kPoolBucketSkiplist1_2  = 0;
inline constexpr uint8_t kPoolBucketSkiplist3_4  = 1;
inline constexpr uint8_t kPoolBucketSkiplist5_8  = 2;
inline constexpr uint8_t kPoolBucketSkiplist9_16 = 3;
inline constexpr uint8_t kPoolBucketRegionDesc   = 4;
inline constexpr uint8_t kPoolBucketArena        = 5;
inline constexpr uint8_t kPoolBucketDescBacking  = 6;
inline constexpr uint8_t kPoolBucketArtNode4     = 7;
inline constexpr uint8_t kPoolBucketArtNode16    = 8;
inline constexpr uint8_t kPoolBucketArtNode48    = 9;
inline constexpr uint8_t kPoolBucketArtNode256   = 10;
inline constexpr uint8_t kPoolBucketCount        = 11;

// 256 chunks/bucket is bounded by the substrate Link encoding's 8-bit
// chunk_id. 11 x 256 x 128 B = 352 KiB lazy-committed.
inline constexpr uint32_t kChunksPerPoolBucket = 256;
inline constexpr uint32_t kTotalVaChunkDescPoolSize =
    kPoolBucketCount * kChunksPerPoolBucket;

//===----------------------------------------------------------------------===//
//  VaChunkDesc
//===----------------------------------------------------------------------===//

// Substrate cap dictated by the Link encoding's 8-bit slot_idx.
inline constexpr uint32_t kMaxSlotsPerChunk = 256;

// Crystalline-managed; body reads under a domain pin stay valid until
// the pin drops, and chunk pages stay committed until va_chunk_desc_free
// runs after the retire batch's grace window closes.
struct alignas(64) VaChunkDesc
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
    // Emitted directly (rather than reused via composition) so VaChunkDesc
    // is standard-layout. Natural 4-byte pad at [20..23] follows batch_link.
    LIBC_CRYSTALLINE_NODE_FIELDS(VaChunkDesc);

    void *chunk_base{};
    uint32_t slot_size{};
    uint32_t slot_capacity{};
    // Pagemap-aligned commit size. Can exceed `slot_size * slot_capacity`
    // when slots do not pack cleanly into 64 KiB; the FreeFn passes this
    // verbatim to decommit_chunk to match the commit-time `cbytes`.
    uint32_t chunk_bytes{};
    uint32_t reserved_c_{};
    // (state:8 | count:24 | gen:32) — see va_tracker_chunk_state.h.
    cpp::Atomic<uint64_t> live_state{0};
    uint8_t  bucket_id{};
    uint8_t  chunk_id{};
    uint8_t  reserved_a_{};
    uint8_t  reserved_b_{};
    ::LIBC_NAMESPACE::internal::alloc_primitives::AtomicBitmap<
        kMaxSlotsPerChunk, /*trap_on_collision=*/true>
        occupancy{};
    alloc::partition::PartitionDescriptor *partition{};
    // partition_secret ^ (class_id << 16 | chunk_id << 8). Defeats
    // heap-spray of a recycled pool slot — partition_secret lives in PCB
    // Zone 0b, unreachable to user code.
    uint64_t chunk_canary{};
};

static_assert(sizeof(VaChunkDesc) <= 128,
              "VaChunkDesc must fit in 2 cache lines");
static_assert(alignof(VaChunkDesc) == 64,
              "VaChunkDesc must be cache-line aligned");

//===----------------------------------------------------------------------===//
//  Crystalline-W reclamation
//===----------------------------------------------------------------------===//

// Body is identical for both chunk domains; dispatch on bucket_id is
// only for canary derivation.
void va_chunk_desc_free(VaChunkDesc *desc);

inline constexpr uint32_t kVaChunkRetireFreq = 4;

// Both domains use only kVaChunkPinSlot = 0 (one pin per
// va_chunk_acquire_slot / resolve_link_target_impl call).
inline constexpr uint32_t kVaChunkMaxIdx = 1;

// Two domains so the skiplist's chunk-table reader-race grace window is
// independent of the ART's.
extern ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    VaChunkDesc, &va_chunk_desc_free, kVaChunkRetireFreq, kVaChunkMaxIdx>
    g_va_tracker_skiplist_chunk_domain;

extern ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    VaChunkDesc, &va_chunk_desc_free, kVaChunkRetireFreq, kVaChunkMaxIdx>
    g_va_tracker_art_chunk_domain;

[[nodiscard]] LIBC_INLINE constexpr bool
is_art_chunk_bucket(uint8_t bucket_id) {
    return bucket_id >= kPoolBucketArtNode4;
}

// Centralises the skiplist/ART split so the bucket-id threshold lives in
// exactly one place across every commit/retire/protect site.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    VaChunkDesc, &va_chunk_desc_free, kVaChunkRetireFreq, kVaChunkMaxIdx> &
pick_chunk_domain(uint8_t bucket_id) {
    return is_art_chunk_bucket(bucket_id) ? g_va_tracker_art_chunk_domain
                                          : g_va_tracker_skiplist_chunk_domain;
}

//===----------------------------------------------------------------------===//
//  Pool API
//===----------------------------------------------------------------------===//

// Accessors rather than direct symbol export so the PagemapTraits
// specialization below can resolve slot_idx -> VaChunkDesc* without
// pulling in the file-scope BSS array.
[[nodiscard]] VaChunkDesc *va_chunk_desc_pool_base();
[[nodiscard]] size_t va_chunk_desc_pool_capacity();

// Lock-free over a freelist bitmap; CAS-loss falls through to the next
// bit. Returns nullptr on pool exhaustion (a structural failure — the
// pool is sized for worst-case demand across every consumer).
[[nodiscard]] VaChunkDesc *va_chunk_desc_pool_claim();

void va_chunk_desc_pool_release(VaChunkDesc *cd);

// Idempotent; safe under concurrent call.
void va_chunk_desc_pool_init_once();

//===----------------------------------------------------------------------===//
//  Commit driver
//===----------------------------------------------------------------------===//

// Walks `chunk_table` from the rotating hint; on the first null slot,
// reserves a partition, claims a descriptor, runs the unified commit
// transaction (split + commit_replace + counter + pagemap), and
// CAS-installs the descriptor. The descriptor MUST be claimed BEFORE the
// chunk commit because its pool index is the pagemap entry's slot_idx,
// which commit_chunk publishes as part of the same transaction.
//
// `chunk_bytes` is preserved on the descriptor so va_chunk_desc_free can
// pass the matching size to decommit_chunk (slot_size * slot_capacity is
// not enough when slots do not pack cleanly into the pagemap quantum).
//
// On race-loss (peer published at `cid` first) rolls back this thread's
// commit, releases the descriptor, and returns the peer's descriptor so
// the outer alloc loop can use it. Returns nullptr on partition / pool /
// table exhaustion.
[[nodiscard]] VaChunkDesc *commit_new_va_chunk_for(
    alloc::partition::PartitionClass cls,
    cpp::Atomic<VaChunkDesc *> *chunk_table,
    cpp::Atomic<uint32_t> *next_chunk_id_hint,
    uint8_t bucket_id, uint32_t slot_size, uint32_t slots_per_chunk,
    uint32_t chunk_bytes);

//===----------------------------------------------------------------------===//
//  Drain orchestrator
//===----------------------------------------------------------------------===//

// Marks the slot free and, if it was the last live slot, drains the
// chunk through release_va_chunk_slot (combined decrement +
// Live->Draining CAS) and retires the descriptor.
//
// Bitmap clear MUST precede the count decrement: a peer that observes
// the lower count then runs try_va_chunk_reserve must see the freed bit,
// otherwise it bounces off a "full" bitmap and bails. `cd` must have
// passed `validate_chunk_or_trap` on the acquire side; page decommit is
// deferred to va_chunk_desc_free so pinned-reader dereferences across
// the grace window still land on live pages.
void release_slot_in_va_chunk(VaChunkDesc *cd, uint32_t slot_idx,
                               cpp::Atomic<VaChunkDesc *> *chunk_table);

//===----------------------------------------------------------------------===//
//  Shared chunk-bitmap allocator scaffold
//===----------------------------------------------------------------------===//

// Single pin slot shared by every chunk_table cd-load on the calling
// thread. Nested allocators on the same thread would need a distinct
// slot — none of the current consumers nest.
inline constexpr uint32_t kVaChunkPinSlot = 0;

// Distinguishes "no chunk here" (nullptr) from "install in progress" in
// the skiplist's flat shared chunk-table. The value `1` cannot collide
// with a real descriptor because the pool lives well above the first
// page. Only the skiplist install path produces it; other consumers
// publish directly.
[[nodiscard]] LIBC_INLINE VaChunkDesc *va_chunk_installing_sentinel() {
    return reinterpret_cast<VaChunkDesc *>(static_cast<uintptr_t>(1));
}

// Invoked once va_chunk_acquire_slot has claimed and zero-filled the
// slot; completes any consumer-specific publication (canary stamp, etc.).
using VaChunkSlotInitFn = void (*)(void *slot, VaChunkDesc *cd,
                                   uint32_t chunk_id, uint32_t slot_idx,
                                   void *ctx);

struct VaChunkAcquireSpec {
    alloc::partition::PartitionClass cls;
    cpp::Atomic<VaChunkDesc *> *chunk_table;
    // Rotating starting offset only — RELAXED-stored on the commit side;
    // the reservation CAS on `cd->live_state` is the actual linearisation
    // point for slot ownership. Not a publish channel.
    cpp::Atomic<uint32_t> *next_chunk_id_hint;
    // 255 for the skiplist (Link encoding reserves chunk_id 255 as a
    // sentinel) or 256 elsewhere.
    uint32_t chunk_count;
    uint32_t slots_per_chunk;
    // Doubles as (a) bucket filter — load-bearing for the skiplist's
    // flat table shared across four height buckets, trivially passes
    // for homogeneous tables; (b) Crystalline domain selector via
    // pick_chunk_domain so reservation pin and drain-on-rollback land
    // in the descriptor's owning domain.
    uint8_t consumer_bucket_id;
    VaChunkSlotInitFn init;
    void *init_ctx;
};

// Single-pass acquire over `spec.chunk_table` using rotating-hint. The
// reservation CAS on `cd->live_state` is the linearisation point for
// slot ownership. On bitmap-raced-out, rolls back the reservation and
// mirrors the regular drain path if rollback drives count to zero.
// Returns nullptr if no live chunk had a free slot after a full scan —
// caller commits a fresh chunk and retries.
[[nodiscard]] void *va_chunk_acquire_slot(const VaChunkAcquireSpec &spec);

//===----------------------------------------------------------------------===//
//  Bitmap helpers
//===----------------------------------------------------------------------===//

// Returns the claimed slot index, or `cap_bits` if every slot is full.
// The inner loop peels lost-CAS bits from a local snapshot rather than
// reloading the word, so contention does not amplify into repeated
// word-reads.
[[nodiscard]] LIBC_INLINE uint32_t
try_acquire_first_free_slot(VaChunkDesc *cd, uint32_t cap_bits) {
    constexpr uint32_t kBitsPerWord = 64;
    const uint32_t word_count =
        (cap_bits + kBitsPerWord - 1) / kBitsPerWord;
    for (uint32_t w = 0; w < word_count; ++w) {
        uint64_t occ = cd->occupancy.template word_at<
            cpp::MemoryOrder::RELAXED>(w);
        uint64_t free_mask = ~occ;
        const uint32_t word_lo = w * kBitsPerWord;
        if (word_lo + kBitsPerWord > cap_bits) {
            const uint32_t valid = cap_bits - word_lo;
            free_mask &= (valid == kBitsPerWord)
                             ? ~uint64_t{0}
                             : ((uint64_t{1} << valid) - 1);
        }
        while (free_mask != 0) {
            const uint32_t bit = static_cast<uint32_t>(
                __builtin_ctzll(free_mask));
            const uint32_t slot = word_lo + bit;
            if (cd->occupancy.try_acquire(slot))
                return slot;
            free_mask &= ~(uint64_t{1} << bit);
        }
    }
    return cap_bits;
}

//===----------------------------------------------------------------------===//
//  Validation helpers
//===----------------------------------------------------------------------===//

[[nodiscard]] LIBC_INLINE uint64_t
compute_va_chunk_canary(uint64_t partition_secret, uint16_t class_id,
                        uint8_t chunk_id);

// Trapping here aligns the debug context with the structural violation
// instead of surfacing it inside the eventual `__builtin_memset` against
// unmapped pages. `partition_secret` is passed in so this header does
// not pull in process_control_block.h — every call site has it cached
// for the matching slot-canary derivation.
LIBC_INLINE void
validate_chunk_or_trap(VaChunkDesc *cd, alloc::partition::PartitionClass cls,
                       uint32_t expected_cid, uint64_t partition_secret) {
    uint64_t expected_canary = compute_va_chunk_canary(
        partition_secret, static_cast<uint16_t>(cls),
        static_cast<uint8_t>(expected_cid));
    if (LIBC_UNLIKELY(cd->chunk_canary != expected_canary))
        __builtin_trap();
    if (LIBC_UNLIKELY(cd->chunk_base == nullptr))
        __builtin_trap();
}

LIBC_INLINE void validate_slot_or_trap(VaChunkDesc *cd, uint32_t slot) {
    if (LIBC_UNLIKELY(slot >= cd->slot_capacity))
        __builtin_trap();
}

//===----------------------------------------------------------------------===//
//  Slot recovery and fork canary refresh helpers
//===----------------------------------------------------------------------===//

struct RecoveredSlot {
    VaChunkDesc *cd;
    uint32_t chunk_id;
    uint32_t slot_idx;
};

// Generic FreeFn-side recovery. `chunk_bytes` MUST be a power of two —
// the chunk-base derivation masks `addr & ~(chunk_bytes - 1)`. Every
// step that violates a structural invariant traps; canary validation is
// the caller's job (the pool-specific class id is not derivable here).
[[nodiscard]] LIBC_INLINE RecoveredSlot
recover_slot_from_va(const void *slot_va, uint32_t chunk_bytes,
                     uint32_t slot_size, uint32_t slots_per_chunk,
                     uint32_t chunk_table_size,
                     cpp::Atomic<VaChunkDesc *> *chunk_table) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(slot_va);
    uintptr_t chunk_base_addr =
        addr & ~static_cast<uintptr_t>(chunk_bytes - 1);
    size_t slot_off = static_cast<size_t>(addr - chunk_base_addr);
    if (LIBC_UNLIKELY(slot_off % slot_size != 0))
        __builtin_trap();
    uint32_t slot_idx = static_cast<uint32_t>(slot_off / slot_size);
    if (LIBC_UNLIKELY(slot_idx >= slots_per_chunk))
        __builtin_trap();

    alloc::partition::PartitionDescriptor *part = alloc::partition::lookup(
        reinterpret_cast<void *>(chunk_base_addr));
    if (LIBC_UNLIKELY(part == nullptr))
        __builtin_trap();
    uintptr_t partition_base = reinterpret_cast<uintptr_t>(part->base);
    uintptr_t off_in_partition =
        chunk_base_addr - partition_base -
        static_cast<uintptr_t>(alloc::partition::kPartitionGuardBytes);
    if (LIBC_UNLIKELY(off_in_partition % chunk_bytes != 0))
        __builtin_trap();
    uint32_t chunk_id =
        static_cast<uint32_t>(off_in_partition / chunk_bytes);
    if (LIBC_UNLIKELY(chunk_id >= chunk_table_size))
        __builtin_trap();

    // ACQUIRE pairs with the RELEASE-CAS that publishes a fresh chunk
    // descriptor in `commit_new_va_chunk_for`.
    VaChunkDesc *cd = chunk_table[chunk_id].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(cd == nullptr))
        __builtin_trap();

    return {cd, chunk_id, slot_idx};
}

// Per-slot canary is checked first. Both checks require partition_secret
// (PCB Zone 0b, ProcessPrng-derived, not observable to user code) so a
// heap-spray attacker cannot forge either; trapping at the per-slot
// check preserves the strongest signal in the debug context.
LIBC_INLINE void
validate_slot_canaries_or_trap(uint64_t slot_node_canary, VaChunkDesc *cd,
                               uint16_t cls_id, uint8_t chunk_id,
                               uint8_t slot_idx, uint64_t partition_secret) {
    uint64_t expected_node_canary = compute_va_node_canary(
        partition_secret, cls_id, chunk_id, slot_idx);
    if (LIBC_UNLIKELY(slot_node_canary != expected_node_canary))
        __builtin_trap();
    uint64_t expected_chunk_canary = compute_va_chunk_canary(
        partition_secret, cls_id, chunk_id);
    if (LIBC_UNLIKELY(cd->chunk_canary != expected_chunk_canary))
        __builtin_trap();
}

// The pool implementation writes the freshly-rotated canary into the
// slot and performs any additional pool-specific post-fork repair (e.g.
// the Arena pool clears stale LOCKED state on the head's level-0 link).
using SlotCanaryRefreshFn = void (*)(void *slot_va, uint64_t fresh_canary);

// Single-threaded by contract — runs in the child after
// RtlCloneUserProcess before any other thread reaches a pool code path,
// so RELAXED bitmap loads suffice. Per-chunk canary refresh and
// chunk_table reachability are the caller's responsibility; this helper
// only walks the bitmap and re-derives slot canaries.
LIBC_INLINE void
refresh_slot_canaries_in_chunk(VaChunkDesc *cd, uint16_t cls_id,
                               uint8_t chunk_id, uint32_t slots_per_chunk,
                               uint64_t partition_secret,
                               SlotCanaryRefreshFn per_slot) {
    constexpr uint32_t kBitsPerWord = 64;
    const uint32_t cap_bits = slots_per_chunk;
    const uint32_t word_count =
        (cap_bits + kBitsPerWord - 1) / kBitsPerWord;
    uintptr_t base = reinterpret_cast<uintptr_t>(cd->chunk_base);
    for (uint32_t w = 0; w < word_count; ++w) {
        uint64_t bits = cd->occupancy.template word_at<
            cpp::MemoryOrder::RELAXED>(w);
        const uint32_t word_lo = w * kBitsPerWord;
        // Tail-mask: stale set bits past slot_capacity must not be
        // dispatched to the per-slot callback.
        if (word_lo + kBitsPerWord > cap_bits) {
            const uint32_t valid = cap_bits - word_lo;
            bits &= (valid == kBitsPerWord)
                        ? ~uint64_t{0}
                        : ((uint64_t{1} << valid) - 1);
        }
        while (bits != 0) {
            const uint32_t bit =
                static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            const uint32_t slot = word_lo + bit;
            void *slot_va = reinterpret_cast<void *>(
                base + static_cast<uintptr_t>(slot) *
                           static_cast<uintptr_t>(cd->slot_size));
            uint64_t fresh = compute_va_node_canary(
                partition_secret, cls_id, chunk_id,
                static_cast<uint8_t>(slot));
            per_slot(slot_va, fresh);
        }
    }
}

//===----------------------------------------------------------------------===//
//  Class metadata
//===----------------------------------------------------------------------===//

[[nodiscard]] LIBC_INLINE alloc::partition::PartitionClass
va_class_for_pool_bucket(uint8_t bucket_id) {
    using PC = alloc::partition::PartitionClass;
    switch (bucket_id) {
    case kPoolBucketSkiplist1_2:  return PC::VaTrackerSkiplist1_2;
    case kPoolBucketSkiplist3_4:  return PC::VaTrackerSkiplist3_4;
    case kPoolBucketSkiplist5_8:  return PC::VaTrackerSkiplist5_8;
    case kPoolBucketSkiplist9_16: return PC::VaTrackerSkiplist9_16;
    case kPoolBucketRegionDesc:   return PC::VaTrackerRegionDesc;
    case kPoolBucketArena:        return PC::VaTrackerArena;
    case kPoolBucketDescBacking:  return PC::VaTrackerDescBacking;
    case kPoolBucketArtNode4:     return PC::VaTrackerArtNode4;
    case kPoolBucketArtNode16:    return PC::VaTrackerArtNode16;
    case kPoolBucketArtNode48:    return PC::VaTrackerArtNode48;
    case kPoolBucketArtNode256:   return PC::VaTrackerArtNode256;
    default:                      return PC::Empty;
    }
}

// class_id and chunk_id occupy disjoint 16/8-bit lanes so the fold is
// collision-free across (class, chunk) tuples. The per-slot extension
// (compute_va_node_canary) adds slot_idx into a third disjoint lane.
[[nodiscard]] LIBC_INLINE uint64_t
compute_va_chunk_canary(uint64_t partition_secret, uint16_t class_id,
                        uint8_t chunk_id) {
    return partition_secret ^
           ((uint64_t{class_id} << 16) | (uint64_t{chunk_id} << 8));
}

//===----------------------------------------------------------------------===//
//  Init and fork
//===----------------------------------------------------------------------===//

// Idempotent.
void va_chunk_init();

// Clears Crystalline state in both chunk domains. Per-bucket canary
// refresh is each consumer's own fork-reinit hook's responsibility.
void va_chunk_fork_reinit();

// Diagnostic. Unsynchronised; may transiently over/under-count under
// concurrent claim/release.
[[nodiscard]] uint32_t total_live_va_chunks();

//===----------------------------------------------------------------------===//
//  Pool iteration (fork-only)
//===----------------------------------------------------------------------===//

using VaChunkDescVisitFn = void (*)(VaChunkDesc *cd, void *ctx);

// Single-threaded by precondition (fork-reinit only); no synchronisation
// against concurrent claim/release. Used to find descriptors that became
// unreachable from their chunk_table because a dead thread's Crystalline
// reservation cell was holding the retire batch at fork — the FreeFn
// never ran in the child, so the consumer's reinit reclaims the leftover.
void for_each_claimed_va_chunk_desc(VaChunkDescVisitFn visit, void *ctx);

[[nodiscard]] uint32_t va_chunk_pool_index_of(VaChunkDesc *cd);

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

//===----------------------------------------------------------------------===//
//  BatchLinkCodec specialization
//===----------------------------------------------------------------------===//

// Encodes a `VaChunkDesc *` as `pool_index + 1` so 0 stays the
// unretired sentinel. Pool size 2816 fits in 12 bits, well clear of
// bit 31 reserved by the codec.
namespace LIBC_NAMESPACE_DECL {
namespace concurrent {
template <>
struct BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::VaChunkDesc> {
  using Node = ::LIBC_NAMESPACE::windows::va_tracker::VaChunkDesc;
  LIBC_INLINE static uint32_t encode(CrystallineNode *n) noexcept {
    Node *base =
        ::LIBC_NAMESPACE::windows::va_tracker::va_chunk_desc_pool_base();
    return 1u + static_cast<uint32_t>(static_cast<Node *>(n) - base);
  }
  LIBC_INLINE static CrystallineNode *decode(uint32_t code) noexcept {
    Node *base =
        ::LIBC_NAMESPACE::windows::va_tracker::va_chunk_desc_pool_base();
    return &base[code - 1u];
  }
};
} // namespace concurrent
} // namespace LIBC_NAMESPACE_DECL

//===----------------------------------------------------------------------===//
//  PagemapTraits specialization for VaTrackerVaChunk
//===----------------------------------------------------------------------===//

// `slot_idx` is the dense pool index; bucket_id on the descriptor
// identifies the owning subsystem. Pool base/capacity come from
// accessors rather than PCB Zone 0 — the pool is BSS-backed and
// shape-immutable, so a sealed redirect adds no value; defence-in-depth
// rests on the cookie-XOR'd pagemap entry word.
namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

template <>
struct PagemapTraits<VaChunkConsumer::VaTrackerVaChunk> {
  using Descriptor = ::LIBC_NAMESPACE::windows::va_tracker::VaChunkDesc;
  [[nodiscard]] LIBC_INLINE static size_t pool_capacity() {
    return ::LIBC_NAMESPACE::windows::va_tracker::va_chunk_desc_pool_capacity();
  }
  [[nodiscard]] LIBC_INLINE static Descriptor *pool_base() {
    return ::LIBC_NAMESPACE::windows::va_tracker::va_chunk_desc_pool_base();
  }
};

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_TRACKER_CHUNK_H
