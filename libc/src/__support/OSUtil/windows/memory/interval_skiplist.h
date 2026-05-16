//===- interval_skiplist.h - Concurrent interval skiplist ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Inner concurrent interval skiplist of the va_tracker. One instance per
// (ART-leaf, CPU) pair owns the `[start, end) -> RegionDesc *` map.
// Clean-room reproduction of Kim, Kwon, and Kang, "Scalable Address
// Spaces using Concurrent Interval Skiplist," SOSP 2025 — no source-level
// derivation from the authors' GPL-licensed Linux artifact.
//
// Lock state, ABA tag, chain pointer, and the parking address all share
// one 64-bit link word per node (see skiplist_link_traits.h). Reintroducing
// a separately-allocated rwlock breaks substrate T1 (tag monotonicity):
// the lock state must participate in the same atomic that linearises
// every other write to the link, or in-place reuse becomes observable.
//
// Init-order safety: contention parking via `futex_addr::wait` requires
// the wait_slot pool to be live. Tier A bring-up
// (`LIBC_REGISTER_MEMORY_PRIMITIVE` phase 6) runs single-threaded and
// emits no Lock operations; the pool comes online before the first
// auxiliary libc thread (the reactor IOCP drain) spawns.
//
// Lock-hold envelope spans every NT syscall inside the mutation
// (placeholder reservation, view replacement, placeholder free, view
// unmap): Lock + syscalls + Swap-publish + Unlock. The kernel VAD lock
// serialises the underlying VM syscalls anyway, so the hold imposes no
// contention beyond what the kernel already enforces; disjoint-range
// parallelism is preserved because disjoint mutators take disjoint
// LOCKED sets.
//
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

// Geometric distribution at p=1/2 puts Pr(h>=16) below 0.001 in a 65K-node
// arena; beyond 65K the per-class substrate cap is exhausted anyway.
inline constexpr uint8_t kMaxHeight = 16;

// Buckets {1..2, 3..4, 5..8, 9..16} cover [1, kMaxHeight].
inline constexpr uint32_t kBucketCount = 4;

// Above 128 logical CPUs, threads share via a TID-hash modulo 128.
inline constexpr uint32_t kArenaCount = 128;
inline constexpr uint32_t kArenaMask = kArenaCount - 1;

// Capped at 256 by the substrate's 8-bit slot_idx field in Link::next.
inline constexpr uint32_t kSlotsPerChunk = 256;

// Capped at 256 by the substrate's 8-bit chunk_id field in Link::next.
inline constexpr uint32_t kChunksPerBucket = 256;

// Link::next reserves 0xFFFF as the null sentinel, so node allocation
// never installs chunk_id == 0xFF. The remaining 255 ids form a single
// process-global pool shared by every height bucket — tall-node-heavy
// workloads draw from the same pool as short-node-heavy workloads
// instead of exhausting a per-height partition early.
inline constexpr uint32_t kSkiplistAddressableChunks = 0xFFu;

inline constexpr uint32_t kMaxSkiplistNodes =
    kSlotsPerChunk * kSkiplistAddressableChunks;

// Not a retry budget — exceeding it means the chain is structurally
// broken and __builtin_trap() is the right response.
inline constexpr uint32_t kTraversalStepLimit =
    kMaxSkiplistNodes + kMaxHeight;

// Crystalline-W retire frequency for skiplist nodes (high-churn).
inline constexpr uint32_t kSkiplistRetireFreq = 16;

// Sparse-retire; only fires on live_count transitioning to zero.
inline constexpr uint32_t kSkiplistChunkRetireFreq = 4;

//===----------------------------------------------------------------------===//
//  Bucket geometry
//===----------------------------------------------------------------------===//

// Slot bytes derive from the fixed 72 B node header plus `height * 8`
// bytes of flexible `next[]` tail:
//   header  = 24 (CrystallineNode) + 8 lo + 8 hi + 8 value + 1 height
//           + 1 bucket + 1 chunk_id + 1 slot_idx + 4 _pad
//           + 8 owning_arena + 8 node_canary
//           = 72 B
//   slot    = align_up(header + max_height_in_bucket * 8, 16)
struct BucketGeometry {
  alloc::partition::PartitionClass cls;
  uint32_t slot_size;
  uint32_t slots_per_chunk;
  uint32_t chunk_bytes;
  uint8_t  max_height_in_bucket;
};

// chunk_bytes is pinned at 64 KiB because the pagemap requires
// 64 KiB-aligned chunk reservations; per-chunk payload (slot_size * 256)
// is always under 64 KiB and the remainder is committed-but-unused.
// Worst case ~30 MiB across all buckets at saturation; pages stay
// zero-backed until touched, so no RSS hit.
//
// chunk_bytes is an allocator storage-domain constant — orthogonal to
// the VA-range granularity of the intervals the nodes describe. `[lo,
// hi)` is arbitrary uintptr_t; 4 KiB-granular intervals route through
// the same buckets as 64 KiB-granular ones with no change in node
// layout, pin discipline, or retire path.
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

[[nodiscard]] LIBC_INLINE constexpr uint8_t bucket_for_height(uint8_t h) {
    return h <= 2  ? 0
         : h <= 4  ? 1
         : h <= 8  ? 2
                   : 3;
}

[[nodiscard]] LIBC_INLINE constexpr uint16_t
skiplist_class_id_for_bucket(uint8_t bucket) {
    return static_cast<uint16_t>(bucket_geometry(bucket).cls);
}

//===----------------------------------------------------------------------===//
//  Substrate Link::next encoding
//===----------------------------------------------------------------------===//
//
// Link::next packs an 8-bit process-global chunk_id (high byte) and
// 8-bit slot_idx (low byte). chunk_id names a process-global direct-
// table entry, so resolving an encoded successor is O(1) — no probing
// across the four height buckets.

[[nodiscard]] LIBC_INLINE constexpr uint16_t
encode_link_next(uint8_t chunk_id, uint8_t slot_idx) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(chunk_id) << 8) | slot_idx);
}

[[nodiscard]] LIBC_INLINE constexpr uint8_t
decode_link_chunk_id(uint16_t enc) {
    return static_cast<uint8_t>((enc >> 8) & 0xFFu);
}

[[nodiscard]] LIBC_INLINE constexpr uint8_t
decode_link_slot_idx(uint16_t enc) {
    return static_cast<uint8_t>(enc & 0xFFu);
}

// chunk_id == 0xFF / slot_idx == 0xFF reserved as null; allocation
// never claims chunk_id == 0xFF.
inline constexpr uint16_t kLinkNullEncoding = 0xFFFFu;

//===----------------------------------------------------------------------===//
//  SkiplistNodeBase — per-interval skiplist node
//===----------------------------------------------------------------------===//

// Layout (header 72 B; flexible-tail next[height]):
//
//   offset  size  field
//        0    24  CrystallineNode header
//       24     8  lo                       interval low (inclusive)
//       32     8  hi                       interval high (exclusive)
//       40     8  value                    cpp::Atomic<RegionDesc *>
//       48     1  height                   [1..kMaxHeight]
//       49     1  bucket                   0..3
//       50     1  chunk_id                 self-id in chunk_table
//       51     1  slot_idx                 self-id within chunk
//       52     4  _pad                     align owning_arena to 8
//       56     8  owning_arena             Arena *
//       64     8  node_canary              partition_secret XOR
//       72   Hx8  next[0..height-1]        substrate Link words
//
// alignas(16) is required for the substrate's tag-monotonicity
// invariant on the embedded Link words.
struct Arena;

struct alignas(16) SkiplistNodeBase
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
    // [0..19] Crystalline-W fields emitted directly so SkiplistNodeBase
    // is standard-layout; the unused 4 B at [20..23] keeps lo 8-aligned
    // at offset 24 and matches the pre-refactor inherited-base layout
    // byte-for-byte.
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

// `__builtin_offsetof(SkiplistNodeBase, next)` warns under
// -Winvalid-offsetof because the inherited CrystallineNode carries
// unions; the next[1] tail has fixed sizeof = sizeof(Link), so
// `sizeof(SkiplistNodeBase) - sizeof(next)` is the portable identity.
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

// One concurrent interval skiplist instance, covering VA range
// [arena_lo, arena_hi) for one (ART-leaf, cpu_index) pair. Inherits
// CrystallineNode so the arena retires through its own domain; the
// FreeFn drains any remaining chain nodes before returning the body.
//
// `head` is INLINE (not bucket-allocated) so the chain-head address
// stays stable across the arena's lifetime. Level 0 lives in
// head.next[0]; levels 1..kMaxHeight-1 live in head_tail_links_[] —
// head_next(i) abstracts the split.
struct alignas(64) Arena
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
    // [0..19] Crystalline-W fields emitted directly so Arena is
    // standard-layout; head is alignas(16) and lands at offset 32 with
    // 12 B natural pad — matches the pre-refactor inherited base.
    LIBC_CRYSTALLINE_NODE_FIELDS(Arena);

    alignas(16) SkiplistNodeBase head;
    cpp::Atomic<::LIBC_NAMESPACE::linkage::Link>
        head_tail_links_[kMaxHeight - 1]{};

    // Most-recent successful Alloc target — paper walk-start hint.
    // CAS-cleared on every retire whose node was the current target,
    // so it is conservatively stale at most until the next Alloc.
    cpp::Atomic<SkiplistNodeBase *> hint{nullptr};

    // Set at create from the owning ART leaf's binding (4 GiB stride
    // per leaf in the current ART key encoding).
    uintptr_t arena_lo{};
    uintptr_t arena_hi{};

    uint32_t  cpu_index{};

    // Defence-in-depth on top of the chunk-level canary.
    uint64_t  arena_canary{};

    uint8_t   _pad_tail[16]{};

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

// One per-height-bucket allocator. chunk_table is fixed at
// kChunksPerBucket entries and indexed by the process-global
// chunk_id; entries install lazily. next_chunk_id_hint points at
// the next likely owned id for this bucket; bucket_alloc_node falls
// back to a full scan when the hint misses.
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

// Move-only typed result of a successful Lock (paper Algorithm 1). On
// success (valid()): pred captures the level-0 predecessor under LOCKED
// with snap pred_snap, and at(0..count()) carries the locked overlapping
// successors in ascending VA order. On failure: errno_ is -EINVAL (bad
// input) or -ENOMEM (overflow-block alloc fail).
//
// Successor records carry only the node pointer; Swap uses
// link_exchange_state (unconditional CAS-loop), so no per-record snap.
//
// 32 inline records cover the common short-span Lock; arbitrary spans
// spill into page-allocated 1024-record overflow blocks.
struct LockedSet : private ChunkedAppendStore<SkiplistNodeBase *, 32, 1024> {
private:
    using Store = ChunkedAppendStore<SkiplistNodeBase *, 32, 1024>;

public:
    Arena *arena{nullptr};
    uintptr_t lo{0};
    uintptr_t hi{0};

    SkiplistNodeBase *pred{nullptr};
    ::LIBC_NAMESPACE::linkage::Link pred_snap{};

    // 0 on success; -EINVAL / -ENOMEM on failure.
    int errno_{0};

    LockedSet() = default;
    LockedSet(const LockedSet &) = delete;
    LockedSet &operator=(const LockedSet &) = delete;
    LockedSet(LockedSet &&other) noexcept;
    // Deleted: a LockedSet is acquired in place; reassigning a live
    // locked set has no defined unwind for the prior locks.
    LockedSet &operator=(LockedSet &&) = delete;
    ~LockedSet() = default;

    using Store::at;
    using Store::clear;
    using Store::count;
    using Store::empty;
    using Store::is_unused;
    using Store::push;
    using Store::release_storage;

    // Populates *this in place rather than returning a prvalue, so
    // RangeLockGuard and other holders never need move-assign. Contention
    // parks on the per-link 64-bit word; release-class CAS wakes.
    // Precondition: *this is pristine (default-constructed); calling
    // acquire on a populated LockedSet traps. Returns true on success;
    // on failure populates errno_ and leaves pred == nullptr.
    [[nodiscard]] bool acquire(Arena *arena, uintptr_t lo, uintptr_t hi);

    [[nodiscard]] LIBC_INLINE bool valid() const { return pred != nullptr; }
};

//===----------------------------------------------------------------------===//
//  NewNodes — typed builder for the Swap splice
//===----------------------------------------------------------------------===//

// cleanup_value_on_abort controls whether the FreeFn detaches and
// releases the node's payload RegionDesc on retire — false when the
// caller has already transferred desc ownership out of the unpublished
// node before the Swap CAS failed.
struct NewNodeRecord {
    SkiplistNodeBase *node{nullptr};
    bool cleanup_value_on_abort{true};
};

// 4 inline records cover the common single-insert + dual-fragment
// shape; arbitrary-span Swaps spill into page-allocated 1024-record
// overflow blocks.
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

    // cleanup_value_if_unpublished: false when the caller has already
    // transferred desc ownership out of the node, so a later retire
    // must NOT release it via the FreeFn. Returns false on
    // overflow-block alloc fail.
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

// Visitor responsibilities:
//   * On success / erase: err == 0; nodes is the replacement set (empty
//     for pure erase).
//   * On allocation failure: visitor releases every partial it owns
//     (descs via region_desc_release, nodes via the skiplist domain's
//     retire) and returns err == -ENOMEM with empty nodes.
//
// Map distinguishes intentional erase from alloc failure by err, NOT
// by the emptiness of nodes.
struct VisitorOutcome {
    NewNodes nodes;
    SkiplistNodeBase *hint_after_success{nullptr};
    bool update_hint{false};
    int err = 0;
};

//===----------------------------------------------------------------------===//
//  Crystalline-W FreeFn signatures and skiplist domain
//===----------------------------------------------------------------------===//

// Metadata-only. Validates node canary, releases the payload via
// region_desc_release when cleanup_value_on_abort was true at retire,
// clears the chunk bitmap, decrements live_count, returns the body to
// its slot pool. Never calls nt_pal — Crystalline-W offers no
// synchronous grace primitive (Nikolaev & Ravindran PLDI 2024 §1), so
// kernel-state teardown must be driven by the synchronous mutator,
// not by this FreeFn.
void skiplist_node_free(SkiplistNodeBase *node);

// Drains any remaining nodes from the chain (expected empty when the
// leaf's binding-retire path is the caller; defence-in-depth scrubs
// LOCKED to LIVE first) and returns the arena body to its slot pool.
void arena_free(Arena *arena);

// Pin slots: 0/1 walker prev/cur, 2/3 gather_level_bookmarks prev/cur.
// Audited max = 3, so MaxIdx = 4.
inline constexpr uint32_t kSkiplistMaxIdx = 4;

extern ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    SkiplistNodeBase, &skiplist_node_free, kSkiplistRetireFreq,
    kSkiplistMaxIdx>
    g_va_tracker_skiplist_domain;

} // namespace va_tracker
} // namespace windows

namespace concurrent {
// Bodies are out-of-line in interval_skiplist.cpp / arena_alloc.cpp
// because they need TU-local g_bucket_state / g_arena_state /
// kArenaSlotSize; the declarations here let every TU that instantiates
// CrystallineDomain<SkiplistNodeBase|Arena> see the codec interface.
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
// Every public op takes Arena * explicitly; the va_tracker composer
// dispatches via ART -> tagged-pointer Arena * (one per leaf). The
// `is_*` prefix avoids collision with the Linux kernel's `mas_*`
// maple-tree API surface and keeps blame logs unambiguous when comparing
// to upstream POSIX-on-Linux audit traces.

// include_pred=true on Map abandon (Swap-CAS failure, visitor error);
// false on normal Lock/Swap pairing because the linearisation CAS
// consumes pred. Each release CAS fires LINK_ALERT_FIRED_BIT via the
// traits hook and the paired wake drains parkers.
void Unlock(LockedSet &set, bool include_pred = false);

// Linearisation point: single link_cas_snap_relink<LIVE> on
// set.pred->next[0] publishes the new chain head AND clears LOCKED in
// one tag bump. On success the old locked-set members are proven
// unreachable from upper levels, OR-set to INVALIDATED, then retired.
// Upper-level publish (paper Fig. 8 collective CAS) splices new nodes
// into levels >= 1 under the LOCKED -> LOCKED re-entrant edge.
// Returns false on linearisation-CAS failure; caller's Map retries
// Lock from scratch.
[[nodiscard]] bool Swap(LockedSet &set, NewNodes &new_nodes);

// Wait-free, AS-safe — VEH-callable provided the caller holds the
// skiplist-domain pin externally. ACQUIRE-loads next[i] from
// kMaxHeight-1 down, skipping LOCKED and INVALIDATED (paper invariant
// I5 keeps INVALIDATED payloads dereferenceable until the FreeFn
// fires). Helps on a Harris MARK via link_cas_set_mark +
// link_finalize_after_splice. IDLE never reachable on a live chain.
//
// out_node: when non-null, receives the covering node so VEH callers
// can inspect link-word state for in-flight remap stalling. The node
// pointer is held alive by the same kPinSlotCur reservation as the
// returned desc.
[[nodiscard]] RegionDesc *Query(Arena *arena, uintptr_t key,
                                SkiplistNodeBase **out_node = nullptr);

// Walk-start uses arena->hint; insert-CAS at a gap of at least len
// bytes within the arena's range. Returns 0 on failure (no gap or
// substrate exhaustion).
[[nodiscard]] uintptr_t Alloc(Arena *arena, uintptr_t lo, uintptr_t hi,
                              size_t len, RegionDesc *value);

// Entries with cleanup_value_on_abort==false have their payload
// detached first so a failed CAS retry does not release caller-owned
// RegionDesc state.
void retire_unpublished_nodes(NewNodes &nodes);

// Point lookup — thin wrapper over Query (paper §8.2). See Query for
// the out_node contract.
[[nodiscard]] LIBC_INLINE RegionDesc *
is_walk(Arena *arena, uintptr_t key,
        SkiplistNodeBase **out_node = nullptr) {
    return Query(arena, key, out_node);
}

// LOCKED is held on the overlap set for the full insertion envelope,
// including any NT syscalls the visitor issues (Lock + syscalls +
// Swap-publish + Unlock).
// Returns -EINVAL on bad input, -ENOMEM on alloc failure inside Lock
// or the visitor.
[[nodiscard]] int is_insert(Arena *arena, uintptr_t lo, uintptr_t hi,
                            RegionDesc *value);

// Supports partial removal — split into surviving prefix/suffix
// fragments. -ENOMEM on alloc failure for the fragment clone path.
[[nodiscard]] int is_erase(Arena *arena, uintptr_t lo, uintptr_t hi);

// Paper Algorithm 2 driver. Visitor signature:
//   VisitorOutcome(LockedSet &, uintptr_t lo, uintptr_t hi)
// Retries the full Lock/Visitor/Swap cycle on linearisation-CAS
// failure; surfaces visitor allocation failure verbatim.
template <class Visitor>
[[nodiscard]] int Map(Arena *arena, uintptr_t lo, uintptr_t hi,
                      Visitor &&visitor);

// Ordered iteration. Visitor signature: void(SkiplistNodeBase *).
// Visits LIVE and LOCKED nodes overlapping [lo, hi) in ascending VA
// order; INVALIDATED is skipped because ordered iteration follows the
// post-Swap chain only (paper I5 still keeps `value` dereferenceable
// for point Query, but not for ordered walks).
template <class Visitor>
void is_walk_range(Arena *arena, uintptr_t lo, uintptr_t hi,
                   Visitor &&visitor);

// Map wrapper named to keep the public surface uniform with the other
// is_* operations.
template <class Visitor>
[[nodiscard]] LIBC_INLINE int is_modify_range(Arena *arena, uintptr_t lo,
                                              uintptr_t hi, Visitor &&visitor) {
    return Map(arena, lo, hi, static_cast<Visitor &&>(visitor));
}

//===----------------------------------------------------------------------===//
//  Allocator surface
//===----------------------------------------------------------------------===//

// Returned node is init_node'd (birth era stamped, canary derived) but
// NOT YET PUBLISHED — caller's Swap commits visibility. owning_arena
// is stored in the header so the FreeFn can reach back at retire time.
// Returns nullptr on link-encoding exhaustion or substrate alloc fail.
[[nodiscard]] SkiplistNodeBase *bucket_alloc_node(uint8_t height,
                                                  Arena *owning_arena);

// Slot init: handles zeroed, shape == NONE, flags == 0, canary
// derived. Caller populates then publishes via value.store(RELEASE).
[[nodiscard]] RegionDesc *region_desc_alloc();

// Metadata-only — validates canaries, zero-fills, drains the chunk
// state machine. Carries no kernel handles; backing kernel-state
// lifetime is owned by the Transaction whose commit retired the desc.
// Precondition: no concurrent reader holds a stale pointer past their
// pin — skiplist-domain grace covers this via the parent node retire
// that triggered the release.
void region_desc_release(RegionDesc *desc);

// Clones src for a fragment covering [frag_lo, ...) where src
// originally covered [src_lo, src_hi). The clone shares src's
// BackingRef verbatim — kernel handles are never duplicated, the
// placeholder is never split, the backing object is not mutated.
// section_offset is shifted by (frag_lo - src_lo); shape, flags,
// view_prot, numa_interleave_mask are copied verbatim. Callers
// patching flags or view_prot overwrite after the clone returns.
// Returns nullptr on region_desc_alloc failure; discarding a failed
// clone leaks no state because the FreeFn is metadata-only.
[[nodiscard]] RegionDesc *clone_region_desc_for_fragment(RegionDesc *src,
                                                          uintptr_t src_lo,
                                                          uintptr_t frag_lo);

// Initialises the sentinel head (lo==arena_lo, hi==arena_hi,
// height==kMaxHeight, all next[] null) and stamps the birth era.
[[nodiscard]] Arena *arena_alloc(uintptr_t arena_lo, uint32_t cpu_index);

// arena_free drains any remaining nodes — expected empty when the
// caller is the leaf's binding-retire path; defence-in-depth scrubs
// LOCKED Links to LIVE and retires every reachable node.
void arena_retire(Arena *arena);

// Returns nullptr on the null-sentinel encoding (0xFFFF) or an
// unresolvable direct chunk-table entry (e.g. chunk in Draining state).
[[nodiscard]] SkiplistNodeBase *resolve_link_target(uint16_t enc);

//===----------------------------------------------------------------------===//
//  Init and fork hooks
//===----------------------------------------------------------------------===//

// Registered via LIBC_REGISTER_MEMORY_PRIMITIVE phase 6. Runs
// single-threaded before any auxiliary libc thread spawns, satisfying
// the init-order safety invariant for futex_addr::wait on the link
// word. Lazy-reserves the four skiplist partitions plus the Arena and
// RegionDesc partitions; initialises bucket states and chunk-desc pool.
void interval_skiplist_init();

// .libcfork$M hook, priority kForkPrioVaTracker. Drains pending
// retires, re-derives every chunk canary from the rotated
// partition_secret, reseeds the per-thread height PRNG. Defence-in-
// depth scrubbing of LOCKED Links is owned by the va_tracker layer
// above this one.
void interval_skiplist_fork_reinit();

//===----------------------------------------------------------------------===//
//  Skiplist node-height sampler
//===----------------------------------------------------------------------===//

// Geometric on [1, kMaxHeight] (paper §3; p=1/2). Per-thread TLS PRNG
// seeded lazily from `tid XOR pid XOR partition_secret` so each thread
// draws an independent sequence.
[[nodiscard]] uint8_t sample_node_height();

//===----------------------------------------------------------------------===//
//  Diagnostics
//===----------------------------------------------------------------------===//

// Snapshot of skiplist allocator state; used by tests.
struct SkiplistStats {
    uint32_t nodes_allocated_per_bucket[kBucketCount];
    uint32_t live_chunks_per_bucket[kBucketCount];
    uint32_t arena_count;
    // CAS / search retries on level i >= 1 upper publish.
    uint32_t upper_publish_retry_count;
};

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

// Pinned chain advance — paper §4.2 Fig. 10 generalised form. One
// protect() call covers the era-stability fast path plus the bounded
// slow path (paper §5 Lemmas 5.2 / 5.3). The body lives in the cpp so
// the chunk-domain plumbing in resolve_link_target_impl stays
// TU-local; only this declaration is header-visible so is_walk_range
// below can call it. Returns nullptr on null-sentinel or unresolvable
// chunk-table state.
//
// Pin lifetime is the slot reservation: caller MUST move on (re-pin
// or clear_all) before the returned pointer's era can advance.
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
