//===- interval_skiplist.h - Concurrent interval skiplist ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Inner concurrent interval skiplist of the va_tracker — the per-arena
/// structure that owns the `[start, end) -> RegionDesc *` map for one
/// (ART-leaf, CPU) pair. Clean-room reproduction of the algorithm
/// presented in Kim, Kwon, and Kang, "Scalable Address Spaces using
/// Concurrent Interval Skiplist," SOSP 2025. No source-level derivation
/// from the authors' GPL-licensed Linux artifact.
///
/// The substrate's defining property is interval-scoped locking encoded
/// in the `state` byte of each node's `next[0].val` `Link` word
/// (`SkiplistNodeState::{LIVE, LOCKED, INVALIDATED, IDLE}` — see
/// skiplist_link_traits.h). A Lock-acquisition CAS simultaneously bumps
/// the substrate's ABA tag, flips the state to LOCKED, and sets
/// `LINK_ALERT_FIRED_BIT` (the `with_state_alerting` wither). The paired
/// `futex_addr::wake(&link, INT_MAX)` issues `NtAlertThreadByThreadId`
/// against any parker that captured the pre-CAS snap via
/// `futex_addr::wait<uint64_t>(&link, snap, nullptr)`. Disjoint
/// operations on disjoint VA intervals proceed in parallel — the
/// property that yields the paper's 13.1x mmap-microbench, 4.49x
/// LevelDB, 3.19x Apache, 1.47x Metis, and 1.27x Psearchy wins versus
/// Linux 6.8 + maple tree at 48 cores.
///
/// Load-bearing invariant: no standalone `MmapLock` instance per node,
/// no global `g_mmap_lock`. Lock state, ABA tag, chain pointer, and
/// the parking address all share the single 64-bit link word. A
/// separate rwlock primitive cannot be reintroduced without breaking
/// the substrate's T1 tag-monotonicity invariant — the lock state must
/// participate in the same atomic that linearises every other write to
/// the link, or in-place reuse becomes observable.
///
/// Init-order safety. `futex_addr::wait` on the link word is invoked
/// only on contention, and contention requires at least two threads.
/// va_tracker init runs Tier A single-threaded
/// (`LIBC_REGISTER_MEMORY_PRIMITIVE` phase 6) and emits no Lock
/// operations during bring-up; the wait_slot pool comes online before
/// the first auxiliary libc thread (the reactor IOCP drain) spawns,
/// so the parking lot is live the first time a Lock can contend.
///
/// Lock-hold envelope across syscalls. When a mutation envelope
/// includes NT syscalls (placeholder reservation, view replacement,
/// placeholder free, view unmap), LOCKED is held for the full
/// envelope: Lock + syscalls + Swap-publish + Unlock. There is no
/// TRANSIENT state and no two-phase publish; the substrate stays at
/// LIVE / LOCKED / INVALIDATED / IDLE per the paper. The kernel VAD
/// lock would serialise the underlying VM syscalls anyway, so the
/// LOCKED hold imposes no contention beyond what the kernel already
/// enforces, and disjoint-range parallelism is preserved because
/// disjoint mutators take disjoint LOCKED sets.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_INTERVAL_SKIPLIST_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_INTERVAL_SKIPLIST_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/partition.h"
#include "src/__support/OSUtil/windows/alloc/partition_class.h"
#include "src/__support/OSUtil/windows/alloc/primitives/occupancy_bitmap.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/concurrent/lock_free_linkage.h"
#include "src/__support/OSUtil/windows/memory/chunked_append_store.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk_state.h"
#include "src/__support/OSUtil/windows/memory/skiplist_link_traits.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

//===----------------------------------------------------------------------===//
//  Capacity constants
//===----------------------------------------------------------------------===//

/// Maximum skiplist tower height. Geometric distribution at `p = 1/2`
/// yields `Pr(h >= H) = 1 / 2^H`, so the probability of needing
/// `h >= 16` in a 65K-node arena is below 0.001. Beyond 65K nodes the
/// per-class substrate cap is exhausted anyway. Revisitable; widening
/// only affects Bucket 3.
inline constexpr uint8_t kMaxHeight = 16;

/// Number of height buckets. Buckets {1..2, 3..4, 5..8, 9..16} cover
/// `[1, kMaxHeight]`; each bucket owns its own `PartitionClass`.
inline constexpr uint32_t kBucketCount = 4;

/// Per-CPU arena count. Above 128 logical CPUs, threads share via a
/// TID-hash modulo 128.
inline constexpr uint32_t kArenaCount = 128;
inline constexpr uint32_t kArenaMask = kArenaCount - 1;

/// Per-chunk slot count. Capped at 256 by the substrate's 8-bit
/// `slot_idx` field in `Link::next`.
inline constexpr uint32_t kSlotsPerChunk = 256;

/// Per-class chunk count. Capped at 256 by the substrate's 8-bit
/// `chunk_id` field in `Link::next`.
inline constexpr uint32_t kChunksPerBucket = 256;

/// Process-global addressable chunk ids per skiplist. `Link::next`
/// reserves `0xFFFF` as the null sentinel, so node allocation never
/// installs `chunk_id == 0xFF`. The remaining 255 chunk ids form a
/// single process-global pool shared by every height bucket —
/// tall-node-heavy workloads draw from the same pool as
/// short-node-heavy workloads instead of exhausting a per-height
/// partition early.
inline constexpr uint32_t kSkiplistAddressableChunks = 0xFFu;

/// Maximum number of live skiplist nodes a process can host.
inline constexpr uint32_t kMaxSkiplistNodes =
    kSlotsPerChunk * kSkiplistAddressableChunks;

/// Capacity-derived corruption-guard step ceiling for every walker
/// loop. Not a retry budget — exceeding it means the chain is
/// structurally broken and `__builtin_trap()` is the right response.
inline constexpr uint32_t kTraversalStepLimit =
    kMaxSkiplistNodes + kMaxHeight;

/// Crystalline-W retire frequency for skiplist nodes (high-churn).
inline constexpr uint32_t kSkiplistRetireFreq = 16;

/// Crystalline-W retire frequency for chunk descriptors (sparse-retire;
/// only on `live_count` transitioning to zero).
inline constexpr uint32_t kSkiplistChunkRetireFreq = 4;

//===----------------------------------------------------------------------===//
//  Bucket geometry
//===----------------------------------------------------------------------===//

/// Static geometry for one height bucket.
///
/// Each bucket owns its own `PartitionClass`; chunk sizes are
/// `slot_size * kSlotsPerChunk` rounded up to 4 KiB page alignment.
///
/// Slot bytes derive from the fixed 72 B node header plus
/// `height * 8` bytes of flexible `next[]` tail:
///
/// \code
///   header  = 24 (CrystallineNode) + 8 lo + 8 hi + 8 value + 1 height
///           + 1 bucket + 1 chunk_id + 1 slot_idx + 4 _pad
///           + 8 owning_arena + 8 node_canary
///           = 72 B
///   slot    = align_up(header + max_height_in_bucket * 8, 16)
/// \endcode
struct BucketGeometry {
  alloc::partition::PartitionClass cls; ///< Partition class for this bucket.
  uint32_t slot_size;                   ///< Bytes per slot (header + tail).
  uint32_t slots_per_chunk;             ///< Slot count, capped by substrate.
  uint32_t chunk_bytes;                 ///< Bytes per chunk (pagemap-aligned).
  uint8_t  max_height_in_bucket;        ///< Highest tower in this bucket.
};

/// Returns the geometry record for the given height bucket index
/// (`0..kBucketCount - 1`).
///
/// `chunk_bytes` is pinned at 64 KiB for every bucket because the
/// pagemap requires 64 KiB-aligned chunk reservations; the per-bucket
/// slot count is capped at the 8-bit substrate `slot_idx` limit
/// (256), so per-chunk payload (`slot_size * 256`) is always smaller
/// than 64 KiB and the remainder is committed-but-unused. Bounded
/// commit-charge cost (~30 MiB worst case across all buckets at
/// saturation; pages stay zero-backed until touched, so no RSS hit).
///
/// `chunk_bytes` is an allocator storage-domain constant — it dictates
/// how many `SkiplistNodeBase` slots fit in one pagemap-stamped chunk,
/// nothing more. It is **orthogonal** to the VA-range granularity of
/// the intervals those nodes describe; `[lo, hi)` is arbitrary
/// `uintptr_t`, and the substrate routes 4 KiB-granular intervals
/// through the same buckets as 64 KiB-granular ones with no change in
/// node layout, pin discipline, or retire path.
///
/// Static asserts in interval_skiplist.cpp pin the derived sizes.
[[nodiscard]] LIBC_INLINE constexpr BucketGeometry
bucket_geometry(uint32_t bucket) {
    using PC = alloc::partition::PartitionClass;
    return bucket == 0 ? BucketGeometry{PC::VaTrackerSkiplist1_2,
                                          /*slot=*/96,  256,
                                          /*chunk=*/64u * 1024u, 2}
         : bucket == 1 ? BucketGeometry{PC::VaTrackerSkiplist3_4,
                                          /*slot=*/112, 256,
                                          /*chunk=*/64u * 1024u, 4}
         : bucket == 2 ? BucketGeometry{PC::VaTrackerSkiplist5_8,
                                          /*slot=*/144, 256,
                                          /*chunk=*/64u * 1024u, 8}
                       : BucketGeometry{PC::VaTrackerSkiplist9_16,
                                          /*slot=*/208, 256,
                                          /*chunk=*/64u * 1024u, 16};
}

/// Maps a sampled tower height to the index of the bucket that owns
/// allocation for it.
[[nodiscard]] LIBC_INLINE constexpr uint8_t bucket_for_height(uint8_t h) {
    return h <= 2  ? 0
         : h <= 4  ? 1
         : h <= 8  ? 2
                   : 3;
}

/// Returns the substrate `PartitionClass` id for the given bucket.
[[nodiscard]] LIBC_INLINE constexpr uint16_t
skiplist_class_id_for_bucket(uint8_t bucket) {
    return static_cast<uint16_t>(bucket_geometry(bucket).cls);
}

//===----------------------------------------------------------------------===//
//  Substrate Link::next encoding
//===----------------------------------------------------------------------===//
//
//  `Link::next` is a 16-bit field that packs an 8-bit process-global
//  skiplist `chunk_id` (high byte) and an 8-bit `slot_idx` (low byte).
//  The chunk id names a process-global direct-table entry, so resolving
//  an encoded successor is O(1) — no probing across the four height
//  buckets is required.

/// Encodes a `(chunk_id, slot_idx)` pair into the substrate's 16-bit
/// `Link::next` field.
[[nodiscard]] LIBC_INLINE constexpr uint16_t
encode_link_next(uint8_t chunk_id, uint8_t slot_idx) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(chunk_id) << 8) | slot_idx);
}

/// Extracts the chunk id from a substrate `Link::next` encoding.
[[nodiscard]] LIBC_INLINE constexpr uint8_t
decode_link_chunk_id(uint16_t enc) {
    return static_cast<uint8_t>((enc >> 8) & 0xFFu);
}

/// Extracts the slot index from a substrate `Link::next` encoding.
[[nodiscard]] LIBC_INLINE constexpr uint8_t
decode_link_slot_idx(uint16_t enc) {
    return static_cast<uint8_t>(enc & 0xFFu);
}

/// Sentinel "no successor" encoding. `chunk_id == 0xFF` /
/// `slot_idx == 0xFF` is reserved as null and skiplist allocation
/// never claims `chunk_id == 0xFF`.
inline constexpr uint16_t kLinkNullEncoding = 0xFFFFu;

// `VaChunkDesc` is defined in `va_tracker_chunk.h`. The skiplist uses
// the shared pool: pool-bucket-ids 0..3 own the four height-bucket
// allocators, pool-bucket-id 4 owns RegionDesc, pool-bucket-id 5 owns
// Arena.

//===----------------------------------------------------------------------===//
//  SkiplistNodeBase — per-interval skiplist node
//===----------------------------------------------------------------------===//

/// One interval-skiplist node, covering the half-open VA range
/// `[lo, hi)` and pointing at the owning `RegionDesc *`.
///
/// Layout (header 72 B; flexible-tail `next[height]`):
///
/// \code
///   offset  size  field
///        0    24  CrystallineNode header
///       24     8  lo                       interval low (inclusive)
///       32     8  hi                       interval high (exclusive)
///       40     8  value                    cpp::Atomic<RegionDesc *>
///       48     1  height                   [1..kMaxHeight]
///       49     1  bucket                   0..3
///       50     1  chunk_id                 self-id in chunk_table
///       51     1  slot_idx                 self-id within chunk
///       52     4  _pad                     align owning_arena to 8
///       56     8  owning_arena             Arena *
///       64     8  node_canary              partition_secret XOR
///       72   Hx8  next[0..height-1]        substrate Link words
/// \endcode
///
/// `alignas(16)` is required for the substrate's tag-monotonicity
/// invariant on the embedded `Link` words.
///
/// Layout invariants (statically asserted below):
///   * `next[]` starts at byte 72 — the chunk-bitmap allocator
///     derives per-bucket slot sizes from this offset.
///   * `sizeof(SkiplistNodeBase) == 80` — the header plus the
///     mandatory `next[1]` tail; per-bucket allocations extend the
///     tail to 96 / 112 / 144 / 208 B managed by the class
///     `slot_size`.
struct Arena;  // forward — defined below.

struct alignas(16) SkiplistNodeBase
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
    // [0..19] Intrusive Crystalline-W runtime fields emitted directly
    // so SkiplistNodeBase is standard-layout. The 4-byte slot at
    // [20..23] is unused (lo is 8-aligned uintptr_t at offset 24);
    // matches the pre-refactor inherited-base layout byte-for-byte.
    LIBC_CRYSTALLINE_NODE_FIELDS(SkiplistNodeBase);

    uintptr_t lo{};
    uintptr_t hi{};
    cpp::Atomic<RegionDesc *> value{nullptr};
    uint8_t  height{};
    uint8_t  bucket{};
    uint8_t  chunk_id{};
    uint8_t  slot_idx{};
    uint32_t _pad{};
    Arena   *owning_arena{nullptr};
    uint64_t node_canary{};
    cpp::Atomic<::LIBC_NAMESPACE::linkage::Link> next[1]{};
};

// `__builtin_offsetof(SkiplistNodeBase, next)` would warn under
// `-Winvalid-offsetof` because the inherited `CrystallineNode` carries
// unions and makes the type non-standard-layout. The flexible-array
// `next[1]` tail has fixed `sizeof = sizeof(Link)`, so
// `sizeof(SkiplistNodeBase) - sizeof(next)` is the portable identity
// for the trailing field's offset.
inline constexpr size_t kSkiplistNodeHeaderBytes = 72;
static_assert(sizeof(SkiplistNodeBase) -
                      sizeof(cpp::Atomic<::LIBC_NAMESPACE::linkage::Link>) ==
                  kSkiplistNodeHeaderBytes,
              "SkiplistNodeBase::next[] must start at byte 72 — "
              "chunk-bitmap allocator slot sizes derive from this");
static_assert(alignof(SkiplistNodeBase) == 16,
              "SkiplistNodeBase must be 16 B aligned for substrate Link "
              "tag-monotonicity");
static_assert(sizeof(SkiplistNodeBase) == 80,
              "SkiplistNodeBase header (incl. flexible[1]) must be 80 B; "
              "per-bucket allocations extend the tail to "
              "96/112/144/208 B managed by class slot_size");

//===----------------------------------------------------------------------===//
//  Arena — per-(leaf, cpu_index) skiplist arena
//===----------------------------------------------------------------------===//

/// One concurrent interval skiplist instance, covering the VA range
/// `[arena_lo, arena_hi)` for one (ART-leaf, cpu_index) pair.
///
/// Heap-allocated from `PartitionClass::VaTrackerArena` (320 B slot,
/// 80 KiB chunk). Inherits `CrystallineNode` so the arena retires
/// through the skiplist domain; the `FreeFn` drains any remaining
/// nodes from the chain before returning the body to its slot pool.
///
/// `head` is a sentinel `SkiplistNodeBase` with `height == kMaxHeight`
/// allocated INLINE in the Arena (not from a bucket) so the arena's
/// chain-head address is stable for `head_next(i)` indexing across
/// the arena's lifetime. Readers traverse `head.next[0]` for level 0
/// and `head_tail_links_[i - 1]` for levels `1..kMaxHeight - 1`; the
/// `head_next(i)` accessor abstracts over the split.
struct alignas(64) Arena
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
    // [0..19] Intrusive Crystalline-W runtime fields emitted directly
    // so Arena is standard-layout. `head` is alignas(16); the macro
    // ends at offset 20 and `head` lands at offset 32 with a 12-byte
    // natural pad — same layout as the pre-refactor inherited base.
    LIBC_CRYSTALLINE_NODE_FIELDS(Arena);

    /// Sentinel head node. The substrate Link word at level 0 lives
    /// in `head.next[0]`; levels `1..kMaxHeight - 1` live in
    /// `head_tail_links_[]` contiguously (see `head_next`).
    alignas(16) SkiplistNodeBase head;
    cpp::Atomic<::LIBC_NAMESPACE::linkage::Link>
        head_tail_links_[kMaxHeight - 1]{};

    /// Most-recent successful Alloc target — paper "Alloc" walk-start
    /// hint. CAS-cleared on every retire whose node was the current
    /// hint target, so the hint is conservatively stale at most until
    /// the next successful Alloc.
    cpp::Atomic<SkiplistNodeBase *> hint{nullptr};

    /// VA range this arena covers. Set at create from the owning ART
    /// leaf binding's `arena_lo` (4 GiB stride per leaf in the current
    /// ART key encoding).
    uintptr_t arena_lo{};
    uintptr_t arena_hi{};

    /// CPU index within the leaf's `arena_set[128]`. 0..127.
    uint32_t  cpu_index{};

    /// Per-Arena canary (`partition_secret` XOR). Defence-in-depth on
    /// top of the chunk-level canary.
    uint64_t  arena_canary{};

    /// Tail pad to `alignas(64)`.
    uint8_t   _pad_tail[16]{};

    /// Returns the substrate Link word for level `i` of the head
    /// sentinel. Level 0 lives in `head.next[0]`; levels `>= 1` live
    /// in `head_tail_links_`.
    ///
    /// \param i Skiplist level, must satisfy `i < kMaxHeight`.
    [[nodiscard]] LIBC_INLINE
    cpp::Atomic<::LIBC_NAMESPACE::linkage::Link> &head_next(uint32_t i) {
        LIBC_ASSERT(i < kMaxHeight);
        return i == 0 ? head.next[0] : head_tail_links_[i - 1];
    }
};

static_assert(sizeof(Arena) <= 320,
              "Arena must fit within its 320 B partition slot");

//===----------------------------------------------------------------------===//
//  Per-bucket allocator state
//===----------------------------------------------------------------------===//

/// One per-height-bucket allocator state record. One instance per
/// bucket; lives in PCB Zone 1 (mutable).
///
/// `chunk_table` is fixed at `kChunksPerBucket == 256` entries and
/// indexed by the process-global skiplist `chunk_id`. Entries are
/// installed lazily. `next_chunk_id_hint` points at the next likely
/// owned global id for this height bucket; `bucket_alloc_node` falls
/// back to a full scan when the hint misses.
struct alignas(64) PerBucketState {
    cpp::Atomic<VaChunkDesc *> chunk_table[kChunksPerBucket]{};
    alignas(64) cpp::Atomic<uint32_t> next_chunk_id_hint{0};
    uint32_t slot_size{};
    uint32_t slots_per_chunk{};
    uint32_t chunk_bytes{};
    uint32_t bucket_id{};
};

//===----------------------------------------------------------------------===//
//  LockedSet — typed result of Lock()
//===----------------------------------------------------------------------===//

/// Move-only typed result of a successful `Lock` (paper Algorithm 1).
///
/// On success (`valid()`): `pred` captures the level-0 predecessor
/// under LOCKED with snap `pred_snap`, and `at(0..count())` carries
/// the locked overlapping successors in ascending VA order.
///
/// On failure (`valid()` returns false): `errno_` reports `-EINVAL`
/// (bad input) or `-ENOMEM` (overflow-block allocation failure).
///
/// Successor records carry only the node pointer; `Swap` uses
/// `link_exchange_state` (an unconditional CAS-loop), so no per-record
/// snap is needed.
///
/// Storage backbone is
/// `ChunkedAppendStore<SkiplistNodeBase *, 32, 1024>` — 32 inline
/// records plus page-allocated 1024-record overflow blocks for
/// arbitrary-span Lock runs. `LockedSet` inherits privately and
/// re-exposes `count`, `at`, `push`, `clear`, and `release_storage`
/// under their generic names; there is no `succ_count` / `succ_at`
/// aliasing layer.
struct LockedSet : private ChunkedAppendStore<SkiplistNodeBase *, 32, 1024> {
private:
    using Store = ChunkedAppendStore<SkiplistNodeBase *, 32, 1024>;

public:
    Arena *arena{nullptr};
    uintptr_t lo{0};
    uintptr_t hi{0};

    SkiplistNodeBase *pred{nullptr};
    ::LIBC_NAMESPACE::linkage::Link pred_snap{};

    /// 0 on success; `-EINVAL` / `-ENOMEM` on failure.
    int errno_{0};

    LockedSet() = default;
    LockedSet(const LockedSet &) = delete;
    LockedSet &operator=(const LockedSet &) = delete;
    LockedSet(LockedSet &&other) noexcept;
    // Move-assign deleted: a LockedSet is acquired into an existing
    // slot via `acquire(arena, lo, hi)`, not by replacing the slot.
    // Reassigning a live locked set has no sensible semantics.
    LockedSet &operator=(LockedSet &&) = delete;
    ~LockedSet() = default;

    using Store::at;
    using Store::clear;
    using Store::count;
    using Store::empty;
    using Store::is_unused;
    using Store::push;
    using Store::release_storage;

    /// Populates this LockedSet by running paper Algorithm 1 over
    /// `[lo, hi)` against `arena`.
    ///
    /// Mutates `*this` in place rather than returning a fresh
    /// prvalue, so `RangeLockGuard` and other holders never need
    /// move-assign. Lock contention parks on the per-link 64-bit word
    /// via `futex_addr::wait` and is woken by the paired
    /// `futex_addr::wake` issued from every release-class CAS.
    ///
    /// \pre `*this` is a pristine (default-constructed) LockedSet.
    ///       Calling `acquire` on a populated LockedSet traps.
    /// \param arena Target arena.
    /// \param lo Inclusive low of the locked range.
    /// \param hi Exclusive high of the locked range.
    /// \returns true on success; on failure populates `errno_` and
    ///          leaves `pred == nullptr` so `valid()` is false.
    [[nodiscard]] bool acquire(Arena *arena, uintptr_t lo, uintptr_t hi);

    /// Returns true when this LockedSet holds a successful Lock
    /// result.
    [[nodiscard]] LIBC_INLINE bool valid() const { return pred != nullptr; }
};

//===----------------------------------------------------------------------===//
//  NewNodes — typed builder for the Swap splice
//===----------------------------------------------------------------------===//

/// One entry in a `NewNodes` builder.
///
/// `cleanup_value_on_abort` controls whether the FreeFn detaches and
/// releases the node's payload `RegionDesc *` on retire — false when
/// the caller has already transferred ownership of the desc out of
/// the unpublished node before the Swap CAS failed.
struct NewNodeRecord {
    SkiplistNodeBase *node{nullptr};
    bool cleanup_value_on_abort{true};
};

/// Move-only builder for the set of new nodes a Map visitor wants to
/// splice into the chain at Swap time.
///
/// Storage backbone is `ChunkedAppendStore<NewNodeRecord, 4, 1024>` —
/// 4 inline records cover the common single-insert + dual-fragment
/// shapes, with spill into page-allocated 1024-record overflow blocks
/// for arbitrary-span Swaps. The two-argument `push(node, cleanup)`
/// wraps the base store's single-argument push by composing a
/// `NewNodeRecord`.
struct NewNodes : private ChunkedAppendStore<NewNodeRecord, 4, 1024> {
private:
    using Store = ChunkedAppendStore<NewNodeRecord, 4, 1024>;

public:
    NewNodes() = default;
    NewNodes(const NewNodes &) = delete;
    NewNodes &operator=(const NewNodes &) = delete;
    NewNodes(NewNodes &&) noexcept = default;
    NewNodes &operator=(NewNodes &&) = delete;
    ~NewNodes() = default;

    using Store::at;
    using Store::clear;
    using Store::count;
    using Store::empty;
    using Store::release_storage;

    /// Appends a new node to the builder.
    ///
    /// \param n The freshly allocated, `init_node`'d but
    ///          not-yet-published skiplist node.
    /// \param cleanup_value_if_unpublished When true (default), a
    ///        subsequent retire of an unpublished node will release
    ///        its `value` payload via the standard FreeFn path. Pass
    ///        false when the caller has already transferred desc
    ///        ownership out of the node.
    /// \returns true on success; false on overflow-block allocation
    ///          failure.
    [[nodiscard]] LIBC_INLINE bool
    push(SkiplistNodeBase *n, bool cleanup_value_if_unpublished = true) {
        return Store::push(NewNodeRecord{n, cleanup_value_if_unpublished});
    }

    [[nodiscard]] LIBC_INLINE SkiplistNodeBase *node_at(uint32_t idx) const {
        return Store::at(idx).node;
    }
    [[nodiscard]] LIBC_INLINE bool cleanup_on_abort(uint32_t idx) const {
        return Store::at(idx).cleanup_value_on_abort;
    }
    [[nodiscard]] LIBC_INLINE SkiplistNodeBase *first() const {
        return count > 0 ? node_at(0) : nullptr;
    }
    [[nodiscard]] LIBC_INLINE SkiplistNodeBase *last() const {
        return count > 0 ? node_at(count - 1) : nullptr;
    }
};

//===----------------------------------------------------------------------===//
//  VisitorOutcome — Map visitor return type
//===----------------------------------------------------------------------===//

/// Return value from a Map visitor.
///
/// Visitor responsibilities:
///
///   * On success / erase (no replacement): `err == 0`, `nodes` is
///     the built replacement set (or empty for pure erase).
///   * On allocation failure: visitor releases every partial it owns
///     (descs via `region_desc_release`, nodes via
///     `g_va_tracker_skiplist_domain.retire`) and returns
///     `err == -ENOMEM` with empty `nodes`.
///
/// Map distinguishes "intentional erase" (caller is `is_erase`) from
/// "allocation failure" by the `err` field, NOT by the emptiness of
/// `nodes`.
struct VisitorOutcome {
    NewNodes nodes;
    SkiplistNodeBase *hint_after_success{nullptr};
    bool update_hint{false};
    int err = 0;
};

//===----------------------------------------------------------------------===//
//  Crystalline-W FreeFn signatures and skiplist domain
//===----------------------------------------------------------------------===//

// `va_chunk_desc_free` lives in `va_tracker_chunk.h` (shared between
// the skiplist and the ART). The chunk-domain instances
// (`g_va_tracker_skiplist_chunk_domain` and
// `g_va_tracker_art_chunk_domain`) are also declared there.

/// FreeFn for retired skiplist nodes — metadata-only. Validates the
/// node canary, releases the payload via `region_desc_release` when
/// `cleanup_value_on_abort` was true at retire time, clears the chunk
/// bitmap, decrements `live_count`, and returns the body to its slot
/// pool. Never issues `nt_pal::*` calls; kernel-state lifecycle for
/// the underlying mapping is owned by the synchronous mutator that
/// drove the retire (Nikolaev & Ravindran PLDI 2024 §1: Crystalline-W
/// offers no synchronous grace primitive, so kernel teardown cannot
/// live inside a FreeFn).
void skiplist_node_free(SkiplistNodeBase *node);

/// FreeFn for retired arenas — drains any remaining nodes (expected
/// to be empty when the leaf's binding-retire path is the caller;
/// defence-in-depth scrubs LOCKED-state Links to LIVE and retires
/// every reachable node) and returns the arena body to its slot pool.
void arena_free(Arena *arena);

/// MaxIdx for the skiplist Crystalline-W domain. Slots consumed:
///   0 — walker `kPinSlotPrev`.
///   1 — walker `kPinSlotCur` (rotated to prev on advance).
///   2 — `gather_level_bookmarks` `kPinSlotLevelPrev`.
///   3 — `gather_level_bookmarks` `kPinSlotLevelCur`.
///
/// Audited max index = 3, so MaxIdx = 4.
inline constexpr uint32_t kSkiplistMaxIdx = 4;

/// Crystalline-W domain governing skiplist-node body lifetime. Reader
/// paths (`is_walk`, `is_walk_range`) pin into this domain; writer
/// paths retire here after the substrate has proven the node is
/// off-chain via the CERT bit.
extern ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    SkiplistNodeBase, &skiplist_node_free, kSkiplistRetireFreq,
    kSkiplistMaxIdx>
    g_va_tracker_skiplist_domain;

} // namespace va_tracker
} // namespace windows

namespace concurrent {
// BatchLinkCodec specializations for SkiplistNodeBase and Arena.
// Method bodies are out-of-line in interval_skiplist.cpp because they
// need access to the anonymous-namespace `g_bucket_state` /
// `g_arena_state` / `kArenaSlotSize`. The declarations here let every
// TU that instantiates `CrystallineDomain<SkiplistNodeBase>` or
// `CrystallineDomain<Arena>` see the codec interface.
template <>
struct BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::SkiplistNodeBase> {
  static uint32_t encode(CrystallineNode *n) noexcept;
  static CrystallineNode *decode(uint32_t code) noexcept;
};
template <>
struct BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::Arena> {
  static uint32_t encode(CrystallineNode *n) noexcept;
  static CrystallineNode *decode(uint32_t code) noexcept;
};
} // namespace concurrent

namespace windows {
namespace va_tracker {

//===----------------------------------------------------------------------===//
//  Public operations — Kim, Kwon, Kang SOSP 2025 §4 surface
//===----------------------------------------------------------------------===//
//
//  Every public op takes `Arena *arena` explicitly. The va_tracker
//  composer dispatches via ART -> tagged-pointer Arena * (single
//  arena per leaf). The `is_*` prefix is deliberate — it avoids
//  collision with the Linux kernel's `mas_*` maple-tree API surface
//  and keeps blame logs unambiguous when comparing to upstream
//  POSIX-on-Linux audit traces.

// Lock — paper Algorithm 1. Acquires LOCKED on the level-0
// predecessor plus every overlapping node in `[lo, hi)`. Lock
// contention parks on the per-link 64-bit word via `futex_addr::wait`;
// pre-park release of the predecessor LOCK ensures disjoint-region
// parallelism (paper §4.4). Acquired into an existing LockedSet via
// `LockedSet::acquire(arena, lo, hi)` (defer-acquire-into-existing-
// slot pattern; see `MmapLockWriterGuard`).

/// Releases LOCKED on the locked-set members.
///
/// With `include_pred == false` (default): releases only
/// `set.at(0..count)`. With `include_pred == true`: also releases
/// `set.pred`. Map abandon paths (Swap-CAS failure, visitor error)
/// pass true; normal Lock/Swap pairing passes false because the
/// linearisation CAS consumes `pred`.
///
/// Each release CAS is `LOCKED -> LIVE`, which
/// `SkiplistLinkTraits::fires_alert` flags for the substrate's
/// `with_state_alerting` wither — `LINK_ALERT_FIRED_BIT` lands in the
/// post-CAS link word and the paired `futex_addr::wake` drains
/// parkers via `NtAlertThreadByThreadId`.
///
/// \param set The locked set to release.
/// \param include_pred When true, also release `set.pred`.
void Unlock(LockedSet &set, bool include_pred = false);

/// Atomically replaces the chain `[set.lo, set.hi)` with the new node
/// run in `new_nodes`. Paper Algorithm 2.
///
/// Linearisation point: a single
/// `link_cas_snap_relink<LIVE, SkiplistLinkTraits>` on
/// `set.pred->next[0]` publishes the new chain head AND clears LOCKED
/// in one tag bump. On success the old locked-set members are proven
/// unreachable from upper levels, OR-set to INVALIDATED, then retired
/// through `g_va_tracker_skiplist_domain`. Multi-level upper publish
/// (paper Fig. 8 collective CAS) splices new nodes into level
/// `i >= 1` chains under the same `LOCKED -> LOCKED` re-entrant edge.
///
/// \param set The locked set produced by a prior `acquire`.
/// \param new_nodes The replacement run built by the Map visitor.
/// \returns true on successful publish; false on linearisation-CAS
///          failure (caller's Map retries Lock from scratch).
[[nodiscard]] bool Swap(LockedSet &set, NewNodes &new_nodes);

/// Wait-free reader. Returns the `RegionDesc *` covering `key` or
/// `nullptr` if no interval covers `key`. Paper §"Query".
///
/// ACQUIRE-loads `next[i]` from `kMaxHeight - 1` down, skipping nodes
/// in state LOCKED and INVALIDATED (paper invariant I5 keeps the
/// payload of INVALIDATED nodes dereferenceable until the FreeFn
/// fires). Helps on a Harris MARK via substrate `link_cas_set_mark`
/// plus `link_finalize_after_splice`. Wait-free and AS-safe —
/// callable from VEH provided the caller holds the skiplist-domain
/// pin externally.
///
/// `LIBC_ASSERT(state == LIVE || state == LOCKED || state == INVALIDATED)`
/// holds throughout the walk — IDLE is the substrate T2
/// retire-Retry sentinel and is never reachable on a live chain.
///
/// \param arena Target arena.
/// \param key VA to resolve.
/// \param[out] out_node When non-null, the covering
///             `SkiplistNodeBase *` is written through. Used by VEH
///             callers that need to inspect the link-word state for
///             in-flight remap stalling. Pass nullptr when only the
///             desc is needed. The node pointer is held alive by the
///             same `kPinSlotCur` reservation as the returned desc.
/// \returns The covering desc, or `nullptr` if no interval covers
///          `key`.
[[nodiscard]] RegionDesc *Query(Arena *arena, uintptr_t key,
                                SkiplistNodeBase **out_node = nullptr);

/// Inserts a node of size `len` somewhere inside `[lo, hi)`. Paper
/// §"Alloc".
///
/// Walk-start uses `arena->hint`; insert-CAS at a gap of at least
/// `len` bytes within the arena's range.
///
/// \returns The allocated VA on success; zero on failure (no gap of
///          the requested size or substrate exhaustion).
[[nodiscard]] uintptr_t Alloc(Arena *arena, uintptr_t lo, uintptr_t hi,
                              size_t len, RegionDesc *value);

/// Retires every node in `nodes` that was allocated for a Swap but
/// never published. Entries with `cleanup_value_on_abort == false`
/// have their payload detached first so a failed CAS retry does not
/// release caller-owned `RegionDesc` state.
void retire_unpublished_nodes(NewNodes &nodes);

/// Point lookup — returns the `RegionDesc *` covering `key`. Paper
/// §8.2 ("look up the interval covering key").
///
/// Wait-free, AS-safe under a skiplist-domain pin held by the caller.
/// Thin wrapper over `Query`; see `Query` for the `out_node`
/// contract.
///
/// \param arena Target arena.
/// \param key VA to resolve.
/// \param[out] out_node Optional; receives the covering node pointer.
/// \returns The covering desc, or `nullptr` if no interval covers
///          `key`.
[[nodiscard]] LIBC_INLINE RegionDesc *
is_walk(Arena *arena, uintptr_t key,
        SkiplistNodeBase **out_node = nullptr) {
    return Query(arena, key, out_node);
}

/// Inserts `[lo, hi) -> value`. `Map(lo, hi, value)` wrapper.
///
/// LOCKED is held on the overlap set for the duration of the
/// insertion, including any NT syscalls the visitor issues (full
/// envelope: Lock + syscalls + Swap-publish + Unlock).
///
/// \param arena Target arena.
/// \param lo Inclusive low of the inserted interval.
/// \param hi Exclusive high of the inserted interval.
/// \param value The desc to install.
/// \returns 0 on success, `-EINVAL` for bad input, `-ENOMEM` on
///          allocation failure inside Lock or the visitor.
[[nodiscard]] int is_insert(Arena *arena, uintptr_t lo, uintptr_t hi,
                            RegionDesc *value);

/// Erases `[lo, hi)`. `Map(lo, hi, nullptr)` wrapper. Supports
/// partial removal — split into surviving prefix and/or suffix
/// fragments.
///
/// \param arena Target arena.
/// \param lo Inclusive low of the erased interval.
/// \param hi Exclusive high of the erased interval.
/// \returns 0 on success, `-EINVAL` for bad input, `-ENOMEM` on
///          allocation failure for the surviving-fragment clone path.
[[nodiscard]] int is_erase(Arena *arena, uintptr_t lo, uintptr_t hi);

/// Atomic mutation across overlapping nodes — paper Algorithm 2
/// driver. The visitor receives the locked set plus the operation
/// range and returns a `VisitorOutcome` carrying the replacement run.
///
/// Retries the full Lock/Visitor/Swap cycle on linearisation-CAS
/// failure; surfaces visitor allocation failure verbatim.
///
/// \param arena Target arena.
/// \param lo Inclusive low of the mutation range.
/// \param hi Exclusive high of the mutation range.
/// \param visitor Callable with signature
///        `VisitorOutcome(LockedSet &, uintptr_t lo, uintptr_t hi)`.
/// \returns 0 on success; `-EINVAL` for bad input; `-ENOMEM` if Lock
///          or the visitor's allocation chain failed.
template <class Visitor>
[[nodiscard]] int Map(Arena *arena, uintptr_t lo, uintptr_t hi,
                      Visitor &&visitor);

/// Ordered iteration over `[lo, hi)`. Used by `mincore`, fork
/// serialisation, `mallinfo2_ex`.
///
/// Reader-side under skiplist-domain pin. Visits LIVE and LOCKED
/// nodes whose interval overlaps `[lo, hi)` in ascending VA order.
/// INVALIDATED nodes are skipped — paper invariant I5 keeps their
/// `value` dereferenceable for point queries, but ordered iteration
/// follows the post-Swap chain only.
///
/// \param arena Target arena.
/// \param lo Inclusive low.
/// \param hi Exclusive high.
/// \param visitor Callable with signature
///        `void(SkiplistNodeBase *)`.
template <class Visitor>
void is_walk_range(Arena *arena, uintptr_t lo, uintptr_t hi,
                   Visitor &&visitor);

/// Atomic mutation across overlapping nodes — `Map` wrapper.
///
/// Equivalent to `Map(arena, lo, hi, visitor)`; named to keep the
/// public surface uniform with `is_walk` / `is_insert` / `is_erase`
/// / `is_walk_range`.
template <class Visitor>
[[nodiscard]] LIBC_INLINE int is_modify_range(Arena *arena, uintptr_t lo,
                                              uintptr_t hi, Visitor &&visitor) {
    return Map(arena, lo, hi, static_cast<Visitor &&>(visitor));
}

//===----------------------------------------------------------------------===//
//  Allocator surface
//===----------------------------------------------------------------------===//

/// Allocates one fresh skiplist node of the given height.
///
/// The returned node has been `init_node`'d through Crystalline
/// (birth era stamped, canary derived) but is NOT YET PUBLISHED into
/// any chain — the caller's `Swap` commits visibility.
///
/// \param height Sampled tower height in `[1, kMaxHeight]`.
/// \param owning_arena The arena the node will eventually belong to;
///                     stored in the node header so the FreeFn can
///                     reach back at retire time.
/// \returns The fresh node, or `nullptr` on process-global
///          link-encoding capacity exhaustion or substrate allocation
///          failure.
[[nodiscard]] SkiplistNodeBase *bucket_alloc_node(uint8_t height,
                                                  Arena *owning_arena);

/// Allocates one fresh `RegionDesc` from the `VaTrackerRegionDesc`
/// partition. Slot init: handles zeroed, `shape == NONE`,
/// `flags == 0`, canary derived. Caller populates fields then
/// publishes via `node->value.store(desc, RELEASE)`.
[[nodiscard]] RegionDesc *region_desc_alloc();

/// Releases a `RegionDesc` allocated by `region_desc_alloc`.
/// Metadata-only — validates canaries, zero-fills the slot, runs the
/// chunk-state-machine drain path. The desc carries no kernel
/// handles; the kernel-state lifecycle for the underlying mapping is
/// owned by the Transaction whose commit retired the desc, via the
/// backing's synchronous teardown inside the commit body.
///
/// \pre No concurrent reader holds a stale pointer past their pin —
///      skiplist-domain grace covers this via the parent node retire
///      that triggered the release.
void region_desc_release(RegionDesc *desc);

/// Clones a `RegionDesc` for a fragment of `src` covering
/// `[frag_lo, ...)` where `src` originally covered
/// `[src_lo, src_hi)`.
///
/// The clone shares the source's `BackingRef` verbatim — kernel
/// handles are never duplicated; the kernel placeholder is never
/// split; the backing object itself is not mutated. Backing
/// kernel-state lifetime is decided by the enclosing Transaction's
/// post-Swap survivor walk plus state CAS, not by any per-clone
/// bookkeeping. `section_offset` is shifted by `(frag_lo - src_lo)`
/// so the clone names the right backing offset for its VA range;
/// `shape`, `flags`, `view_prot`, and `numa_interleave_mask` are
/// copied verbatim. Callers patching flags or `view_prot` (e.g.
/// mutate visitors) overwrite after the clone returns.
///
/// \param src The source desc.
/// \param src_lo Inclusive low of the source's original VA range.
/// \param frag_lo Inclusive low of the fragment's VA range.
/// \returns The clone, or `nullptr` on `region_desc_alloc` failure.
///          A failure path that discards the clone leaks no state —
///          the FreeFn is metadata-only.
[[nodiscard]] RegionDesc *clone_region_desc_for_fragment(RegionDesc *src,
                                                          uintptr_t src_lo,
                                                          uintptr_t frag_lo);

/// Allocates a fresh Arena from the `VaTrackerArena` partition. Sets
/// `arena_lo` / `arena_hi` / `cpu_index` / `arena_canary`; the
/// sentinel `head` is initialised (`lo == arena_lo`, `hi == arena_hi`,
/// `height == kMaxHeight`, all `next[]` null) and `init_node` stamps
/// the birth era.
[[nodiscard]] Arena *arena_alloc(uintptr_t arena_lo, uint32_t cpu_index);

/// Retires an Arena through the arena Crystalline domain. The FreeFn
/// (`arena_free`) drains any remaining nodes — expected to be empty
/// if the caller is the leaf's binding-retire path; defence-in-depth
/// scrubs LOCKED-state Links to LIVE and retires every reachable
/// node.
void arena_retire(Arena *arena);

/// Resolves an encoded `Link::next` value to a `SkiplistNodeBase`
/// pointer.
///
/// \returns `nullptr` on the null-sentinel encoding (`0xFFFF`) or an
///          unresolvable direct chunk-table entry (e.g. chunk in
///          Draining state).
[[nodiscard]] SkiplistNodeBase *resolve_link_target(uint16_t enc);

//===----------------------------------------------------------------------===//
//  Init and fork hooks
//===----------------------------------------------------------------------===//

/// Tier A bring-up. Registered via `LIBC_REGISTER_MEMORY_PRIMITIVE`
/// phase 6.
///
/// Lazy-reserves the four skiplist partitions plus the Arena
/// partition plus the `RegionDesc` partition. Initialises the four
/// bucket states and the chunk-descriptor pool. Runs single-threaded
/// before any auxiliary libc thread spawns, satisfying the init-order
/// safety invariant for `futex_addr::wait` on the link word.
void interval_skiplist_init();

/// `.libcfork$M` hook, priority `kForkPrioVaTracker`.
///
/// Drains pending retires, re-derives every chunk canary from the
/// rotated `partition_secret`, and reseeds the per-thread height
/// PRNG. Defence-in-depth scrubbing of LOCKED-state Links is owned by
/// the va_tracker layer above.
void interval_skiplist_fork_reinit();

//===----------------------------------------------------------------------===//
//  Skiplist node-height sampler
//===----------------------------------------------------------------------===//

/// Returns a fresh tower height drawn from a geometric distribution
/// on `[1, kMaxHeight]` (Kim, Kwon, Kang SOSP 2025 §3; `p = 1/2`).
///
/// Per-thread TLS PRNG seeded lazily from
/// `tid XOR pid XOR partition_secret`, so each thread draws an
/// independent height sequence. Intended for upper-layer visitors
/// (insert / protect / split / coalesce); the skiplist's own
/// internal helpers (`Alloc`, `append_interval_node`) call this
/// directly.
[[nodiscard]] uint8_t sample_node_height();

//===----------------------------------------------------------------------===//
//  Diagnostics
//===----------------------------------------------------------------------===//

/// Snapshot of skiplist allocator state, used by tests.
struct SkiplistStats {
    /// Per-bucket allocated-node count.
    uint32_t nodes_allocated_per_bucket[kBucketCount];
    /// Per-bucket live-chunk count.
    uint32_t live_chunks_per_bucket[kBucketCount];
    /// Total live Arenas across all bindings.
    uint32_t arena_count;
    /// CAS / search retries on level i >= 1 upper publish.
    uint32_t upper_publish_retry_count;
};

/// Returns a `SkiplistStats` snapshot. Reader-only; no locks taken.
[[nodiscard]] SkiplistStats stats_snapshot();

//===----------------------------------------------------------------------===//
//  Template definitions — out-of-line bodies for Map / is_walk_range
//===----------------------------------------------------------------------===//

template <class Visitor>
int Map(Arena *arena, uintptr_t lo, uintptr_t hi, Visitor &&visitor) {
    if (LIBC_UNLIKELY(arena == nullptr || lo >= hi))
        return -EINVAL;

    for (;;) {
        LockedSet locked;
        (void)locked.acquire(arena, lo, hi);
        if (!locked.valid()) {
            // Lock failed to grow its locked-run storage or saw bad
            // input. Surface the errno_ captured by acquire().
            return locked.errno_;
        }

        VisitorOutcome out = visitor(locked, lo, hi);
        if (out.err != 0) {
            // Visitor signalled allocation failure; every partial it
            // owned has already been released by the visitor itself.
            // Release the locks and surface the error.
            Unlock(locked, /*include_pred=*/true);
            return out.err;
        }

        if (Swap(locked, out.nodes)) {
            if (out.update_hint)
                arena->hint.store(out.hint_after_success,
                                  cpp::MemoryOrder::RELEASE);
            // Banakar 2025's `Map` algorithm has no post-Swap survivor
            // walk, so clear the locked set immediately to preserve the
            // destructor's empty-store invariant. The transaction
            // engine in `run_envelope` keeps the set populated past
            // Swap for `reap_old_backings` to walk, then clears via
            // the post-reap `Unlock`.
            locked.clear();
            return 0;
        }

        // Swap CAS failed. Retire every node we built — each was
        // init_node'd by bucket_alloc_node, so the FreeFn handles
        // bitmap clear, live_count decrement, and node_canary check.
        retire_unpublished_nodes(out.nodes);
        Unlock(locked, /*include_pred=*/true);
    }
}

// Pin slot indices used by the templated walk; mirror the cpp.
namespace walk_pin_slots_ {
inline constexpr uint32_t kPrev = 0;
inline constexpr uint32_t kCur  = 1;
} // namespace walk_pin_slots_

/// Pinned chain advance — paper §4.2 Fig. 10 generalised form.
///
/// One `protect()` call covers the era-stability fast path plus the
/// bounded slow path (paper §5 Lemmas 5.2 / 5.3). The body lives in
/// interval_skiplist.cpp so the chunk-domain plumbing in
/// `resolve_link_target_impl` stays TU-local; only this declaration
/// is header-visible so the `is_walk_range` template below can call
/// it directly.
///
/// \param link_field The substrate Link word to read through.
/// \param cur_slot The pin slot index to install the resolved
///                 successor into.
/// \param parent_for_helper The parent node used by the substrate's
///                          mark-then-help slow path.
/// \returns The resolved successor (pinned in `cur_slot`), or
///          `nullptr` on null-sentinel or unresolvable chunk-table
///          state.
///
/// \warning The pin's lifetime is the slot reservation: the caller
///          MUST move on (re-pin or `clear_all`) before the returned
///          pointer's era can advance.
[[nodiscard]] SkiplistNodeBase *
pinned_read_link_target(
    cpp::Atomic<::LIBC_NAMESPACE::linkage::Link> &link_field,
    uint32_t cur_slot, SkiplistNodeBase *parent_for_helper);

template <class Visitor>
void is_walk_range(Arena *arena, uintptr_t lo, uintptr_t hi,
                   Visitor &&visitor) {
    if (LIBC_UNLIKELY(arena == nullptr || lo >= hi))
        return;

    using Link = ::LIBC_NAMESPACE::linkage::Link;
    auto &dom = g_va_tracker_skiplist_domain;

    // Anchor era only — walk_prev is already known-alive (head
    // sentinel here, or pinned from the prior iteration's kCur).
    SkiplistNodeBase *walk_prev = &arena->head;
    dom.anchor(walk_pin_slots_::kPrev);

    // Capacity-derived corruption guard, not a retry budget. Closes
    // the wait-free invariant on this reader path.
    uint32_t steps = 0;
    for (;;) {
        if (++steps > kTraversalStepLimit)
            __builtin_trap();
        cpp::Atomic<Link> &prev_link =
            (walk_prev == &arena->head ? arena->head_next(0)
                                       : walk_prev->next[0]);
        SkiplistNodeBase *node = pinned_read_link_target(
            prev_link, walk_pin_slots_::kCur, walk_prev);
        if (node == nullptr)
            return;
        if (node->lo >= hi)
            return;

        Link snap = node->next[0].load(cpp::MemoryOrder::ACQUIRE);
        const uint8_t st = snap.state();
        LIBC_ASSERT(st == static_cast<uint8_t>(SkiplistNodeState::LIVE) ||
                    st == static_cast<uint8_t>(SkiplistNodeState::LOCKED) ||
                    st == static_cast<uint8_t>(SkiplistNodeState::INVALIDATED));

        // Visit only LIVE / LOCKED nodes within range. INVALIDATED is
        // skipped: paper invariant I5 keeps the payload dereferenceable
        // for point queries, but ordered iteration follows the
        // post-Swap chain only.
        if (node->hi > lo && node->lo < hi &&
            st != static_cast<uint8_t>(SkiplistNodeState::INVALIDATED)) {
            visitor(node);
        }

        // Advance: rotate pins (cur -> prev). Anchor era only — node
        // was just pinned on kCur and we know it is alive.
        walk_prev = node;
        dom.anchor(walk_pin_slots_::kPrev);
    }
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_INTERVAL_SKIPLIST_H
