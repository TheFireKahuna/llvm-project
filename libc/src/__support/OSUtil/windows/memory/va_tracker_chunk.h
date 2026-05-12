//===- va_tracker_chunk.h - Shared per-region chunk allocator ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-2-GiB-region Arena chunks shared by the va_tracker's interval skiplist
/// and ROWEX ART leaves.
///
/// Each ART-leaf in the va_tracker owns one of these chunks; the chunk is the
/// unit of per-CPU sharding (128 per-CPU arena slots in the upstream Kim,
/// Kwon, Kang design — "Scalable Address Spaces using Concurrent Interval
/// Skiplist", SOSP 2025). Within a chunk, fixed-size slots are claimed via a
/// per-chunk occupancy bitmap and a packed lifecycle word.
///
/// The chunk has three roles:
///
///   * Backing-pool host. A process-lifetime BSS pool of `VaChunkDesc`
///     records, lazy-committed via demand-fault and managed lock-free.
///   * Commit driver. `commit_new_va_chunk_for` performs partition-aware
///     placement of a new chunk's VA, CAS-installs the descriptor into a
///     caller-supplied `chunk_table`, and recovers gracefully when a peer
///     wins the publish race.
///   * Drain orchestrator. `release_slot_in_va_chunk` runs the
///     LIVE -> RETIRING -> RETIRED state machine on `live_count -> 0`,
///     clearing `chunk_table` before the descriptor is retired so concurrent
///     readers cannot land on freshly-decommitted pages.
///
/// The chunk-table backing is hot on reader paths because every skiplist
/// walk dereferences through it; descriptor lifetime is governed by
/// Crystalline-W (Nikolaev and Ravindran, "A Family of Fast and Memory
/// Efficient Lock- and Wait-Free Reclamation", PLDI 2024) via
/// `g_va_tracker_skiplist_chunk_domain` and `g_va_tracker_art_chunk_domain`.
///
/// Pool sizing. Per-class chunk capacity is bounded by the substrate Link
/// encoding (8-bit chunk_id x 8-bit slot_idx = 65 536 nodes per class, 256
/// chunks per class). The pool is sized for the worst-case sum across
/// consumers:
///
///   * 4 skiplist height buckets x 256 chunks = 1024
///   * 1 RegionDesc class        x 256 chunks =  256
///   * 1 Arena class             x 256 chunks =  256
///   * 1 DescBacking class       x 256 chunks =  256
///   * 4 ART node-type classes   x 256 chunks = 1024
///
/// 2816 entries x 128 B = 352 KiB lazy-committed; idle cost is approximately
/// zero.
///
/// Pool-bucket-id taxonomy. The pool is class-flat — one VaChunkDesc array
/// serves all consumers. Each VaChunkDesc carries a `bucket_id` byte that
/// identifies its owning partition class for canary derivation:
///
///   * 0..3  — VaTrackerSkiplist1_2 .. VaTrackerSkiplist9_16
///   * 4     — VaTrackerRegionDesc
///   * 5     — VaTrackerArena
///   * 6     — VaTrackerDescBacking (shared kernel-state backing)
///   * 7..10 — VaTrackerArtNode4 .. VaTrackerArtNode256
///
/// The FreeFn consults `bucket_id` to recover the owning `PartitionClass`
/// for canary computation and to pick the Crystalline domain through which
/// the descriptor will retire:
///
///   * g_va_tracker_skiplist_chunk_domain — bucket_id 0..6 (skiplist height
///     buckets, RegionDesc, Arena, DescBacking — all on the leaf side)
///   * g_va_tracker_art_chunk_domain      — bucket_id 7..10 (ART node-type
///     partitions)
///
/// Both domains share the same FreeFn `va_chunk_desc_free` and dispatch
/// only for canary derivation; the Crystalline grace machinery is per-domain
/// so the skiplist's reader-race window is independent of the ART's.
///
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

/// Pool bucket for skiplist height buckets 1..16. The four buckets map to
/// `VaTrackerSkiplist1_2` through `VaTrackerSkiplist9_16` — see
/// `va_class_for_pool_bucket`.
inline constexpr uint8_t kPoolBucketSkiplist1_2  = 0;
inline constexpr uint8_t kPoolBucketSkiplist3_4  = 1;
inline constexpr uint8_t kPoolBucketSkiplist5_8  = 2;
inline constexpr uint8_t kPoolBucketSkiplist9_16 = 3;
/// Pool bucket for `RegionDesc` records — interval skiplist leaf payload.
inline constexpr uint8_t kPoolBucketRegionDesc   = 4;
/// Pool bucket for per-region `Arena` records (the chunk's own metadata).
inline constexpr uint8_t kPoolBucketArena        = 5;
/// Pool bucket for kernel-state backing shared across va_tracker subsystems.
inline constexpr uint8_t kPoolBucketDescBacking  = 6;
/// Pool buckets for ART (Adaptive Radix Tree, Leis 2013) node-type
/// partitions. The four buckets map to `VaTrackerArtNode4`..`ArtNode256`.
inline constexpr uint8_t kPoolBucketArtNode4     = 7;
inline constexpr uint8_t kPoolBucketArtNode16    = 8;
inline constexpr uint8_t kPoolBucketArtNode48    = 9;
inline constexpr uint8_t kPoolBucketArtNode256   = 10;
inline constexpr uint8_t kPoolBucketCount        = 11;

/// Per-bucket chunk capacity. Bounded by the substrate Link encoding's
/// 8-bit chunk_id field. Multiplied by `kPoolBucketCount` for total pool
/// size — 11 x 256 = 2816 descriptors.
inline constexpr uint32_t kChunksPerPoolBucket = 256;
inline constexpr uint32_t kTotalVaChunkDescPoolSize =
    kPoolBucketCount * kChunksPerPoolBucket;

//===----------------------------------------------------------------------===//
//  VaChunkDesc
//===----------------------------------------------------------------------===//

/// Substrate cap on slots per chunk, dictated by the Link encoding's 8-bit
/// `slot_idx` field.
inline constexpr uint32_t kMaxSlotsPerChunk = 256;

/// One cache line of metadata for a committed va_tracker chunk.
///
/// The descriptor carries the LIVE / RETIRING / RETIRED state byte from
/// `va_tracker_chunk_state.h`, the per-chunk occupancy bitmap, and partition
/// / canary metadata. It inherits `CrystallineNode` so it can ride either
/// chunk-Crystalline domain; the slot capacity is parameterised at commit
/// time so each consumer's chunks can carry different slot counts.
///
/// All state transitions are CAS on the packed `live_state` word so that
/// the state byte, allocator-count, and generation update atomically. See
/// `va_tracker_chunk_state.h` for the (state:8 | count:24 | gen:32) layout.
///
/// Lifetime: Crystalline-W-managed in `g_va_tracker_skiplist_chunk_domain`
/// (bucket_id 0..6) or `g_va_tracker_art_chunk_domain` (bucket_id 7..10).
/// Body reads under a domain pin remain valid until the pin drops; kernel
/// pages backing the chunk stay committed across the grace window because
/// `va_chunk_desc_free` performs the decommit and pages reclaim only after
/// every reservation cell on the retire batch has drained.
struct alignas(64) VaChunkDesc
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
    // [0..19] Intrusive Crystalline-W runtime fields emitted directly
    // so VaChunkDesc is standard-layout. `chunk_base` is 8-aligned at
    // offset 24; natural 4-byte pad at [20..23] follows batch_link.
    LIBC_CRYSTALLINE_NODE_FIELDS(VaChunkDesc);

    void *chunk_base{};
    uint32_t slot_size{};
    uint32_t slot_capacity{};
    /// Pagemap-aligned chunk size as registered with
    /// `partition::commit_chunk`. Can exceed `slot_size * slot_capacity`
    /// when slots do not pack cleanly into 64 KiB; the FreeFn uses this to
    /// pass the commit-time `cbytes` value to `decommit_chunk`.
    uint32_t chunk_bytes{};
    uint32_t reserved_c_{};
    /// Packed lifecycle word: (state:8 | count:24 | gen:32). One atomic so
    /// state transitions linearise with the count changes that trigger
    /// them — see `va_tracker_chunk_state.h` for the helper accessors and
    /// the linearization argument. Updated by `try_va_chunk_reserve`,
    /// `release_va_chunk_slot`, `mark_dead`, `init_live`.
    cpp::Atomic<uint64_t> live_state{0};
    uint8_t  bucket_id{};                            ///< 0..kPoolBucketCount-1
    uint8_t  chunk_id{};                             ///< self-id within class
    uint8_t  reserved_a_{};
    uint8_t  reserved_b_{};
    ::LIBC_NAMESPACE::internal::alloc_primitives::AtomicBitmap<
        kMaxSlotsPerChunk, /*trap_on_collision=*/true>
        occupancy{};
    alloc::partition::PartitionDescriptor *partition{};
    /// Per-chunk canary `partition_secret ^ (class_id << 16 | chunk_id << 8)`.
    /// Validated in `va_chunk_desc_free` and in every allocator hot path
    /// via `validate_chunk_or_trap`. Defeats heap-spray of a recycled pool
    /// slot — `partition_secret` is ProcessPrng-derived and lives in
    /// Zone 0b of the PCB, unreachable to user code.
    uint64_t chunk_canary{};
};

static_assert(sizeof(VaChunkDesc) <= 128,
              "VaChunkDesc must fit in 2 cache lines");
static_assert(alignof(VaChunkDesc) == 64,
              "VaChunkDesc must be cache-line aligned");

//===----------------------------------------------------------------------===//
//  Crystalline-W reclamation
//===----------------------------------------------------------------------===//

/// Crystalline-W FreeFn for `VaChunkDesc`.
///
/// Runs once the descriptor's retire batch passes grace under whichever
/// chunk domain it retired through. The body is identical for both
/// domains; dispatch on `desc->bucket_id` is only for canary derivation.
void va_chunk_desc_free(VaChunkDesc *desc);

/// Per-batch retire frequency for the chunk domains.
///
/// Low enough that grace cycles are timely on workloads with bursty
/// retirement, high enough that the per-retire batch-link overhead is
/// amortised across multiple drains.
inline constexpr uint32_t kVaChunkRetireFreq = 4;

/// Crystalline-W domain covering the va_tracker leaf side: skiplist height
/// buckets, `RegionDesc`, `Arena`, and `DescBacking`. Separating this
/// domain from the ART domain keeps the skiplist's reader-race grace
/// window independent of the ART's.
extern ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    VaChunkDesc, &va_chunk_desc_free, kVaChunkRetireFreq>
    g_va_tracker_skiplist_chunk_domain;

/// Crystalline-W domain covering ART node-type partitions (Node4 / Node16 /
/// Node48 / Node256).
extern ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    VaChunkDesc, &va_chunk_desc_free, kVaChunkRetireFreq>
    g_va_tracker_art_chunk_domain;

/// True when \p bucket_id identifies an ART node-type bucket
/// (`kPoolBucketArtNode4..kPoolBucketArtNode256`).
///
/// Bucket id is the unambiguous indicator of which chunk domain a
/// descriptor retires through: skiplist-side buckets (0..6) live in
/// `g_va_tracker_skiplist_chunk_domain`, ART-side buckets (7..10) live in
/// `g_va_tracker_art_chunk_domain`. The split keeps the skiplist's chunk-
/// table reader-race grace window independent of the ART's.
[[nodiscard]] LIBC_INLINE constexpr bool
is_art_chunk_bucket(uint8_t bucket_id) {
    return bucket_id >= kPoolBucketArtNode4;
}

/// Returns the Crystalline-W chunk domain owning descriptors of bucket
/// \p bucket_id. Used at every chunk-domain dispatch site
/// (`commit_new_va_chunk_for::init_node`, `release_slot_in_va_chunk::retire`,
/// `va_chunk_acquire_slot::protect/retire`) so the skiplist/ART split is
/// expressed once and stays consistent across the file.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    VaChunkDesc, &va_chunk_desc_free, kVaChunkRetireFreq> &
pick_chunk_domain(uint8_t bucket_id) {
    return is_art_chunk_bucket(bucket_id) ? g_va_tracker_art_chunk_domain
                                          : g_va_tracker_skiplist_chunk_domain;
}

//===----------------------------------------------------------------------===//
//  Pool API
//===----------------------------------------------------------------------===//

/// Returns the base of the process-lifetime VaChunkDesc pool.
///
/// The pool itself is a file-scope BSS array in `va_tracker_chunk.cpp`;
/// this accessor exposes its base without exposing the array symbol so
/// that the `PagemapTraits` specialization below can resolve a pagemap
/// entry's `slot_idx` to a `VaChunkDesc *`.
[[nodiscard]] VaChunkDesc *va_chunk_desc_pool_base();

/// Returns the pool's capacity in entries (always
/// `kTotalVaChunkDescPoolSize`).
[[nodiscard]] size_t va_chunk_desc_pool_capacity();

/// Claims an unused slot from the pool and zero-initialises it.
///
/// Lock-free over a bitmap of free slots; falls through to the next bit
/// on CAS-loss. Pool exhaustion is reported by returning nullptr — the
/// caller's outer loop must surface this as a structural failure (the
/// pool is sized for worst-case demand across every consumer).
///
/// \returns the freshly-zeroed descriptor, or nullptr on pool exhaustion.
[[nodiscard]] VaChunkDesc *va_chunk_desc_pool_claim();

/// Returns a slot to the pool's freelist. Used by `va_chunk_desc_free`
/// after the descriptor's Crystalline grace window passes.
void va_chunk_desc_pool_release(VaChunkDesc *cd);

/// Idempotent one-shot initialiser. Sets every bit of the freelist
/// bitmap, marking all pool entries free. Safe to call concurrently.
void va_chunk_desc_pool_init_once();

//===----------------------------------------------------------------------===//
//  Commit driver
//===----------------------------------------------------------------------===//

/// Commits a new chunk for \p cls and publishes its descriptor into
/// \p chunk_table.
///
/// Walks \p chunk_table (capacity `kChunksPerPoolBucket`) starting from
/// the rotating hint; on the first null slot:
///
///   1. Reserves or grows the partition for \p cls and picks a NUMA target.
///   2. Computes the chunk's deterministic VA from
///      `partition.base + guard + cid * chunk_bytes`.
///   3. Claims a `VaChunkDesc` from the pool BEFORE committing pages: the
///      pool index becomes the pagemap entry's `slot_idx`, and
///      `commit_chunk` publishes the pagemap entry as part of the unified
///      commit transaction.
///   4. Calls `partition::commit_chunk` (split placeholder + commit_replace
///      + partition counter + pagemap register + pagemap publish, with
///      rollback on each failure).
///   5. Populates the descriptor (state = LIVE via `init_live`, canary
///      derived from `partition_secret ^ class_id ^ chunk_id`).
///   6. Initialises the Crystalline node header in the matching domain.
///   7. CAS-installs the descriptor into `chunk_table[cid]`.
///
/// On race-loss (a peer publishes at `cid` first): rolls back the commit
/// (pages + counter + pagemap entry), releases the descriptor back to the
/// pool, and returns the peer's descriptor so the caller's outer alloc
/// loop can use it.
///
/// \param cls partition class to commit against.
/// \param chunk_table caller's per-class chunk table; CAS-installation
///        target. Capacity must be `kChunksPerPoolBucket`.
/// \param next_chunk_id_hint rotating hint advanced past the chosen `cid`
///        on success. RELAXED — a load-spreading suggestion, not a publish.
/// \param bucket_id pool bucket id; recorded in the descriptor so the
///        FreeFn can recover the partition class for canary validation.
/// \param slot_size byte size of each slot the consumer carves from the
///        chunk.
/// \param slots_per_chunk slot capacity recorded in the descriptor.
/// \param chunk_bytes pagemap-aligned commit size (multiple of
///        `kPagemapChunkBytes`); preserved on the descriptor so the FreeFn
///        passes the matching value to `decommit_chunk`.
///
/// \returns the newly-installed descriptor, the peer's descriptor on
///          race-loss, or nullptr on partition / pool / table exhaustion.
[[nodiscard]] VaChunkDesc *commit_new_va_chunk_for(
    alloc::partition::PartitionClass cls,
    cpp::Atomic<VaChunkDesc *> *chunk_table,
    cpp::Atomic<uint32_t> *next_chunk_id_hint,
    uint8_t bucket_id, uint32_t slot_size, uint32_t slots_per_chunk,
    uint32_t chunk_bytes);

//===----------------------------------------------------------------------===//
//  Drain orchestrator
//===----------------------------------------------------------------------===//

/// Marks slot \p slot_idx free and, if it was the last live slot, drains
/// the chunk.
///
/// Steps:
///
///   1. `occupancy.mark_dead(slot_idx)` — RELAXED bitmap clear.
///   2. `release_va_chunk_slot(live_state)` — atomic decrement; on
///      post-decrement count of 0 and prior state LIVE, simultaneously
///      transitions state to RETIRING. The CAS linearises the decrement
///      with the transition so a peer reservation that observed
///      `(LIVE, n)` and CASes to `(LIVE, n+1)` cannot race past the
///      drain — the releaser's CAS expecting `(LIVE, 0)` fails because
///      the peer's increment moved the snapshot.
///   3. On drain-winner: clears `chunk_table[chunk_id]` (RELEASE — before
///      retire) and Crystalline-retires the descriptor through the
///      appropriate chunk domain.
///
/// Ordering of step 1 before step 2 is load-bearing: the just-released
/// bit must be visible to peer allocators that race in *before* the
/// count decrement settles. Reversing the order would leave a window in
/// which a peer that snapped the lower count and ran `try_va_chunk_reserve`
/// finds the bitmap still full and bounces off.
///
/// Page decommit happens inside `va_chunk_desc_free` after Crystalline
/// grace, not here, so chunk pages stay committed for the entire grace
/// window. Any pinned reader's slot dereferences still land on live pages.
///
/// \param cd descriptor whose slot is being released; must have been
///        validated by `validate_chunk_or_trap` on the acquire side.
/// \param slot_idx slot to mark free.
/// \param chunk_table chunk table that holds \p cd. Caller must supply
///        the table matching the descriptor's owning class.
void release_slot_in_va_chunk(VaChunkDesc *cd, uint32_t slot_idx,
                               cpp::Atomic<VaChunkDesc *> *chunk_table);

//===----------------------------------------------------------------------===//
//  Shared chunk-bitmap allocator scaffold
//===----------------------------------------------------------------------===//

/// Pin slot used by every `chunk_table` cd-load on the calling thread.
///
/// All chunk-table reads in `va_chunk_acquire_slot` share this single
/// pin slot of the chunk domain — the most recent `protect()` pins the
/// active cd while the caller dereferences it. None of the current
/// consumers nest allocators on the same thread; nested consumers would
/// need a distinct pin slot.
inline constexpr uint32_t kVaChunkPinSlot = 0;

/// Sentinel published into a chunk_table slot while a CAS-mediated
/// install transaction is in flight. Distinguishes "no chunk here yet"
/// (nullptr) from "another installer has claimed this cid". Numerically
/// chosen at the low end of the address space — no valid descriptor can
/// alias the value because the descriptor pool lives well above the
/// first page.
///
/// Currently produced only by the skiplist's `commit_new_skiplist_chunk_for`
/// install path (which CAS-claims the shared flat chunk-table across
/// four height buckets); other consumers publish their descriptors
/// directly without an in-progress sentinel. The shared
/// `va_chunk_acquire_slot` treats the sentinel as "skip — install in
/// progress" so a concurrent scan never tries to reserve a slot in a
/// non-existent chunk.
[[nodiscard]] LIBC_INLINE VaChunkDesc *va_chunk_installing_sentinel() {
    return reinterpret_cast<VaChunkDesc *>(static_cast<uintptr_t>(1));
}

/// Per-class slot initialiser. Invoked once `va_chunk_acquire_slot` has
/// claimed and zero-filled the slot. The callback completes any
/// consumer-specific publication (canary stamp, link-traits state, etc.).
using VaChunkSlotInitFn = void (*)(void *slot, VaChunkDesc *cd,
                                   uint32_t chunk_id, uint32_t slot_idx,
                                   void *ctx);

/// Acquire-side configuration bundle. Held by value during one scan.
struct VaChunkAcquireSpec {
    alloc::partition::PartitionClass cls;
    cpp::Atomic<VaChunkDesc *> *chunk_table;
    cpp::Atomic<uint32_t> *next_chunk_id_hint;
    /// 255 for the skiplist (Link encoding reserves chunk_id 255 as a
    /// sentinel) or 256 for non-skiplist consumers.
    uint32_t chunk_count;
    /// Capacity passed to `try_acquire_first_free_slot`; usually equals
    /// `cd->slot_capacity` at commit time.
    uint32_t slots_per_chunk;
    /// Bucket id of the *calling consumer*. Serves two purposes:
    ///   (a) Filter — chunks in the table whose `cd->bucket_id` differs
    ///       are skipped. Trivially passes for homogeneous tables
    ///       (RegionDesc / Arena / DescBacking / ART-per-type) where every
    ///       chunk shares the consumer's bucket; load-bearing for the
    ///       skiplist's flat table shared across four height buckets.
    ///   (b) Domain selector — `pick_chunk_domain(consumer_bucket_id)`
    ///       picks `g_va_tracker_skiplist_chunk_domain` for buckets 0..6
    ///       and `g_va_tracker_art_chunk_domain` for buckets 7..10, so the
    ///       reservation pin and any drain-on-rollback retire land in the
    ///       descriptor's owning domain.
    uint8_t consumer_bucket_id;
    VaChunkSlotInitFn init;
    void *init_ctx;
};

/// Single-pass acquire over `spec.chunk_table` using the rotating-hint
/// pattern.
///
/// For each `cid` in the rotation: pin via the chunk domain selected by
/// `pick_chunk_domain(spec.consumer_bucket_id)`, attempt
/// `try_va_chunk_reserve`, attempt `try_acquire_first_free_slot`. The
/// reservation CAS on `cd->live_state` is the linearisation point for
/// slot ownership; the rotating hint is RELAXED. ART consumers
/// (buckets 7..10) pin and retire through `g_va_tracker_art_chunk_domain`;
/// skiplist-side consumers (buckets 0..6) pin and retire through
/// `g_va_tracker_skiplist_chunk_domain`.
///
/// On bitmap-raced-out (every free bit lost to a peer mid-CAS): rolls
/// back the reservation. If the rollback drives count to zero, mirrors
/// the regular drain path — clear `chunk_table[cid]`, retire `cd`.
///
/// \returns the initialised slot pointer, or nullptr if no live chunk
///          had a free slot after a full scan (caller commits a fresh
///          chunk and retries).
[[nodiscard]] void *va_chunk_acquire_slot(const VaChunkAcquireSpec &spec);

//===----------------------------------------------------------------------===//
//  Bitmap helpers
//===----------------------------------------------------------------------===//

/// Returns the index of the first free slot in `cd->occupancy`, or
/// \p cap_bits if every slot is full.
///
/// Common case: one word-load plus one CAS. Worst case: `word_count`
/// word-loads plus \p cap_bits CAS attempts (every bit lost to a peer).
/// The TZCNT-based scan walks the occupancy bitmap word-by-word and
/// peels exhausted bits from the local snapshot without reloading, so
/// the inner loop never re-reads under contention.
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
            // Race-loss on this bit; peel and try the next bit in the
            // local snapshot without reloading the word.
            free_mask &= ~(uint64_t{1} << bit);
        }
    }
    return cap_bits;
}

//===----------------------------------------------------------------------===//
//  Validation helpers
//===----------------------------------------------------------------------===//

// Forward declaration so `validate_chunk_or_trap` below sees the symbol
// before its definition further down in the header.
[[nodiscard]] LIBC_INLINE uint64_t
compute_va_chunk_canary(uint64_t partition_secret, uint16_t class_id,
                        uint8_t chunk_id);

/// Validates the descriptor's canary and base pointer; traps on mismatch.
///
/// Called immediately before slot init in every alloc path. Every
/// condition checked here is a structural violation, not a transient
/// race — trapping here keeps the debug context aligned with the bug
/// instead of surfacing it as a fault inside the eventual
/// `__builtin_memset` against unmapped pages.
///
/// Caller passes the cached \p partition_secret so this header does not
/// pull in `process_control_block.h`; every call site already has the
/// secret nearby (for the matching slot-canary derivation in `init`).
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

/// Traps if \p slot is out of range for the descriptor's slot capacity.
LIBC_INLINE void validate_slot_or_trap(VaChunkDesc *cd, uint32_t slot) {
    if (LIBC_UNLIKELY(slot >= cd->slot_capacity))
        __builtin_trap();
}

//===----------------------------------------------------------------------===//
//  Class metadata
//===----------------------------------------------------------------------===//

/// Recovers the owning `PartitionClass` from a pool bucket id. Used by
/// the FreeFn for canary derivation.
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

/// Computes the per-chunk canary
/// `partition_secret ^ (class_id << 16 | chunk_id << 8)`.
///
/// The 8-bit chunk_id and 16-bit class_id live in disjoint lanes of the
/// low 32 bits so the fold is collision-free across `(class, chunk)`
/// tuples. See `compute_va_node_canary` in `va_tracker_chunk_state.h`
/// for the per-slot extension; the two derivations share a common
/// pattern and differ only in which low-lane bytes are populated.
/// Validated in every `*_free`.
[[nodiscard]] LIBC_INLINE uint64_t
compute_va_chunk_canary(uint64_t partition_secret, uint16_t class_id,
                        uint8_t chunk_id) {
    return partition_secret ^
           ((uint64_t{class_id} << 16) | (uint64_t{chunk_id} << 8));
}

//===----------------------------------------------------------------------===//
//  Init and fork
//===----------------------------------------------------------------------===//

/// Initialises both chunk domains and the descriptor pool. Idempotent.
void va_chunk_init();

/// Post-fork hook. Clears Crystalline state in both chunk domains.
///
/// Per-bucket canary refresh is the responsibility of each consumer's
/// own fork-reinit hook (the consumer walks its chunk_table to recompute
/// canaries against the rotated `partition_secret`).
void va_chunk_fork_reinit();

/// Returns the total live chunk descriptors across all pool buckets.
/// Diagnostic only — read with no synchronisation, may transiently
/// over- or under-count under concurrent claim / release.
[[nodiscard]] uint32_t total_live_va_chunks();

//===----------------------------------------------------------------------===//
//  Pool iteration (fork-only)
//===----------------------------------------------------------------------===//

/// Visitor signature for `for_each_claimed_va_chunk_desc`.
using VaChunkDescVisitFn = void (*)(VaChunkDesc *cd, void *ctx);

/// Iterates every currently-claimed pool slot, single-threaded.
///
/// Used by per-subsystem `*_fork_reinit` hooks to locate descriptors
/// that became unreachable from their `chunk_table` — typically because
/// a dead thread's Crystalline reservation cell was holding the
/// descriptor's retire batch when the parent forked, so the FreeFn
/// never got to run in the child. Reachable descriptors are identified
/// by walking each consumer's chunk_table; the pool walk surfaces every
/// claimed slot regardless of reachability, and the consumer reclaims
/// the leftover.
///
/// \pre the process is single-threaded for the duration of the walk
///      and any reclamations driven from the visitor. No
///      synchronisation is provided against concurrent `claim_slot` or
///      `va_chunk_desc_pool_release`.
void for_each_claimed_va_chunk_desc(VaChunkDescVisitFn visit, void *ctx);

/// Recovers the pool slot index of a descriptor. Used by leak-reclaim
/// loops to mark which slots a chunk_table walk found reachable.
[[nodiscard]] uint32_t va_chunk_pool_index_of(VaChunkDesc *cd);

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

//===----------------------------------------------------------------------===//
//  BatchLinkCodec specialization
//===----------------------------------------------------------------------===//

/// Encodes a `VaChunkDesc *` as its pool index (offset by +1 so 0 stays
/// the unretired sentinel) for use in Crystalline-W batch links.
///
/// Pool size 2816 fits in 12 bits, well clear of bit 31 reserved by the
/// codec. Both `g_va_tracker_skiplist_chunk_domain` and
/// `g_va_tracker_art_chunk_domain` are declared `extern` above, so any
/// translation unit using either domain pulls this specialization in
/// transitively.
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

/// Wires the typed pagemap load (`pagemap_load_descriptor<VaTrackerVaChunk>`)
/// to the shared `g_va_chunk_desc_pool`.
///
/// `slot_idx` is the dense index into the pool (range
/// `[0, kTotalVaChunkDescPoolSize)`); the descriptor's `bucket_id` field
/// encodes which subsystem owns the chunk (skiplist / arena / region_desc
/// / ART). Pool base and capacity are read through the accessors above
/// rather than PCB Zone 0 — the pool is BSS-backed and immutable in
/// shape across process lifetime, so a sealed redirect would be overkill.
/// Defense-in-depth still rests on the cookie-XOR'd encoded entry word.
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
