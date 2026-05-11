//===- va_tracker_chunk_state.h - chunk lifecycle state machine -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Three-state lifecycle for the va_tracker chunk descriptors used by the
/// outer ART and the inner interval skiplist. A chunk is a 2 GiB ART-leaf-
/// mapped arena that holds a population of slab-allocated skiplist nodes.
///
/// A bare "live_count -> 0 -> decommit" sequence would carry a use-after-
/// free race: a peer allocator could ACQUIRE-load `chunk_table[chunk_id]`
/// after the owner cleared the bitmap bit but before the owner cleared
/// the table entry, then `try_acquire` a different slot, then
/// `__builtin_memset` a new node into pages the owner had just
/// decommitted. The same race shape applies in ART. Chunk reuse therefore
/// goes through a CAS-driven state transition:
///
/// \code
///   Live ----[last release wins CAS]----> Draining
///     ^                                       |
///     |                                       v
///     +---[reserve_or_grow returns new desc]- Dead --[Crystalline FreeFn]
/// \endcode
///
///   * Live     - `chunk_table[chunk_id]` points at a descriptor whose
///                backing pages are committed; allocators may claim slots.
///   * Draining - last release won the Live->Draining CAS; the table is
///                cleared, pages decommitted, descriptor handed to the
///                chunk Crystalline-W domain. Allocators that observe
///                Draining skip the chunk.
///   * Dead     - the descriptor's Crystalline grace window has passed
///                and the FreeFn returned the descriptor slot to its BSS
///                pool. `chunk_table[chunk_id]` is null at this point.
///
/// Allocators ACQUIRE-load `state` on every chunk before claiming a slot.
/// State transitions go through CAS, never naked stores, so only one
/// thread observes the Live->Draining transition (the "last release
/// winner") and allocators that arrive in the window between
/// `count == 0` and the CAS lose their bitmap-claim race against the
/// winner's `mark_dead` plus decommit step.
///
/// Re-commit on a freshly-installed `chunk_id`: the partition layer's
/// `commit_chunk_register` returns a fresh `PartitionDescriptor`; the
/// caller allocates a new `VaChunkDesc` from the BSS pool, initialises
/// state to Live, and CAS-installs into `chunk_table[id]`. There is no
/// path that resurrects a Dead descriptor — once the Crystalline FreeFn
/// releases a `VaChunkDesc` slot the descriptor's `state` byte is
/// irrelevant, and a subsequent commit gets a fresh descriptor.
///
/// Observer invariant:
///   * Live     - `chunk_base` may be dereferenced; allocators may
///                `try_acquire` slots.
///   * Draining - `chunk_base` MUST NOT be dereferenced; allocators skip.
///   * Dead     - the descriptor itself is being reaped; no observer
///                should still hold a pointer past the chunk-domain
///                grace window.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_TRACKER_CHUNK_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_TRACKER_CHUNK_STATE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

/// Three-state lifecycle for a va_tracker chunk descriptor.
///
/// Encoded as a single `uint8_t` for cheap atomic CAS. Numeric values
/// are stable for the lifetime of the structure; no ABI concern since
/// descriptors are process-internal.
enum class VaChunkState : uint8_t {
    /// Chunk is committed and accepting allocations.
    Live = 0,

    /// Last allocation drained. The Live->Draining CAS winner owns the
    /// reclamation sequence: clear `chunk_table`, decommit, retire the
    /// descriptor through the chunk Crystalline-W domain. Other
    /// allocator observers skip the chunk; readers wait for grace.
    Draining = 1,

    /// Grace window passed; FreeFn ran. Slot has been returned to the
    /// BSS pool. The state byte is set as a tombstone marker but the
    /// descriptor memory may already be reused by a fresh descriptor —
    /// readers must not ever observe Dead through `chunk_table`, which
    /// is cleared as part of the Live->Draining transition before
    /// decommit.
    Dead = 2,
};

//===----------------------------------------------------------------------===//
// Composite live_state encoding.
//
// Splitting `state` and `count` across two atomics would leave an
// unsynchronised window between an allocator's count increment and a
// releaser's state CAS: a releaser that only checked `state == Live`
// without re-verifying `count == 0` could miss an allocator that
// incremented count between its `fetch_sub` and the state CAS, ending
// up draining a chunk that still holds a live slot.
//
// Packing all three fields (state, count, generation) into one 64-bit
// atomic linearises every state transition with the count change that
// triggers it. A single CAS expecting `(Live, 1)` and writing
// `(Draining, 0)` cannot succeed after a peer increments count, because
// the count-bump CAS would have observed the same `(Live, 1)` snapshot
// and either won (count becomes 2; releaser's CAS fails on next retry)
// or lost (releaser won; allocator sees `(Draining, ...)` on retry and
// bails).
//
// Bit layout:
//   bits  0-7    state       (VaChunkState)
//   bits  8-31   count       (24 bits — comfortably > kMaxSlotsPerChunk
//                             = 256)
//   bits 32-63   generation  (32 bits; bumped every successful CAS for
//                             ABA defence on count A->B->A patterns)
//
// 32-bit generation overflow is benign in practice (4 G CAS cycles per
// chunk between any single observer's load and CAS). Crystalline-W pin
// discipline already prevents cross-incarnation ABA via the pool
// freelist's hand-off — the generation field is defence in depth for
// within-incarnation churn (heavy alloc/free traffic on the same chunk
// while one observer is preempted between load and CAS).
//===----------------------------------------------------------------------===//

inline constexpr uint32_t kLiveStateStateBits = 8;
inline constexpr uint32_t kLiveStateCountBits = 24;
inline constexpr uint32_t kLiveStateGenShift = 32;
inline constexpr uint64_t kLiveStateStateMask = 0xFFu;
inline constexpr uint64_t kLiveStateCountMax =
    (uint64_t{1} << kLiveStateCountBits) - 1;
inline constexpr uint64_t kLiveStateGenIncrement =
    uint64_t{1} << kLiveStateGenShift;

// The count field must accommodate every slot a chunk can hold.
// `kMaxSlotsPerChunk` is defined in va_tracker_chunk.h (256 = substrate
// cap, 8-bit slot_idx). If va_tracker_chunk.h ever raises that past
// `kLiveStateCountMax`, this assertion trips.
static_assert(kLiveStateCountMax >= 256,
              "live_state count field must fit kMaxSlotsPerChunk");

[[nodiscard]] LIBC_INLINE constexpr uint64_t
pack_live_state(uint8_t state, uint32_t count, uint32_t generation) {
    return uint64_t{state} | (uint64_t{count} << kLiveStateStateBits) |
           (uint64_t{generation} << kLiveStateGenShift);
}

[[nodiscard]] LIBC_INLINE constexpr uint8_t state_of(uint64_t v) {
    return static_cast<uint8_t>(v & kLiveStateStateMask);
}

[[nodiscard]] LIBC_INLINE constexpr uint32_t count_of(uint64_t v) {
    return static_cast<uint32_t>((v >> kLiveStateStateBits) &
                                 kLiveStateCountMax);
}

[[nodiscard]] LIBC_INLINE constexpr uint32_t gen_of(uint64_t v) {
    return static_cast<uint32_t>(v >> kLiveStateGenShift);
}

//===----------------------------------------------------------------------===//
// CAS helpers operating on the composite `live_state` atomic.
//===----------------------------------------------------------------------===//

/// Allocator pre-reserve: atomically establish `(state == Live AND
/// count < cap)` and increment count.
///
/// The reservation is the count increment — there is no separate
/// `fetch_add` after bitmap acquire. Once a reservation is held the
/// chunk cannot transition to Draining (the drain CAS in
/// `release_va_chunk_slot` requires `count == 0`).
///
/// Success ordering is ACQ_REL: pairs the publisher's chunk-init writes
/// with the caller's subsequent bitmap acquire / slot dereference.
/// Failure ordering is ACQUIRE: refresh `state` for the next chunk in
/// the scan.
///
/// \returns true if the caller now holds a reservation and may proceed
///          to acquire a bitmap bit; false if the chunk is Draining,
///          Dead, or full.
[[nodiscard]] LIBC_INLINE bool
try_va_chunk_reserve(cpp::Atomic<uint64_t> &live_state, uint32_t cap) {
    uint64_t cur = live_state.load(cpp::MemoryOrder::ACQUIRE);
    for (;;) {
        if (state_of(cur) != static_cast<uint8_t>(VaChunkState::Live))
            return false;
        uint32_t cnt = count_of(cur);
        if (cnt >= cap)
            return false;
        // Bump generation on every successful CAS for ABA defence.
        uint64_t next = pack_live_state(
            static_cast<uint8_t>(VaChunkState::Live), cnt + 1,
            gen_of(cur) + 1);
        if (live_state.compare_exchange_weak(cur, next,
                                             cpp::MemoryOrder::ACQ_REL,
                                             cpp::MemoryOrder::ACQUIRE))
            return true;
        // `cur` has been reloaded with the observed value; loop.
    }
}

/// Releaser plus reservation rollback: atomically decrement count, and
/// if the post-decrement count is 0 and state is Live, also transition
/// state to Draining in the same CAS.
///
/// Folding the decrement and the Live->Draining transition into one CAS
/// closes the gap that a peer reservation could race into: any peer
/// reservation that observes `(Live, 0)` between our fetch_sub and a
/// would-be follow-on state CAS is denied because the single combined
/// CAS expects `(Live, 1)` and the peer's increment moves it to
/// `(Live, 2)`.
///
/// \warning Traps on `count == 0` entry — the caller violated the
///          protocol (releasing without holding a reservation, or
///          rolling back a reservation twice).
/// \returns true if this caller is the unique drain winner (must clear
///          `chunk_table`, decommit, retire the descriptor); false in
///          every other case (count > 0 after decrement, or state was
///          already Draining / Dead).
[[nodiscard]] LIBC_INLINE bool
release_va_chunk_slot(cpp::Atomic<uint64_t> &live_state) {
    uint64_t cur = live_state.load(cpp::MemoryOrder::ACQUIRE);
    for (;;) {
        uint32_t cnt = count_of(cur);
        if (LIBC_UNLIKELY(cnt == 0))
            __builtin_trap();
        uint32_t next_cnt = cnt - 1;
        uint8_t st = state_of(cur);
        bool drain_now =
            (next_cnt == 0 &&
             st == static_cast<uint8_t>(VaChunkState::Live));
        uint8_t next_st = drain_now
                              ? static_cast<uint8_t>(VaChunkState::Draining)
                              : st;
        uint64_t next =
            pack_live_state(next_st, next_cnt, gen_of(cur) + 1);
        if (live_state.compare_exchange_weak(cur, next,
                                             cpp::MemoryOrder::ACQ_REL,
                                             cpp::MemoryOrder::ACQUIRE))
            return drain_now;
    }
}

/// Mark a Draining chunk Dead.
///
/// Called once the FreeFn has finished its teardown and is about to
/// return the descriptor slot to the pool. Single-writer by the
/// Crystalline-W FreeFn invariant; preserves the generation for defence
/// in depth against any straggler observer holding a stale snapshot.
LIBC_INLINE void mark_dead(cpp::Atomic<uint64_t> &live_state) {
    uint64_t cur = live_state.load(cpp::MemoryOrder::RELAXED);
    live_state.store(pack_live_state(
                         static_cast<uint8_t>(VaChunkState::Dead), 0,
                         gen_of(cur) + 1),
                     cpp::MemoryOrder::RELEASE);
}

/// Initialise a freshly-allocated descriptor's `live_state` to
/// `(Live, 0, 0)`.
///
/// Called on fresh pool allocations; never on a Dead descriptor in
/// place. Dead descriptors remain in the pool and are returned to fresh
/// allocators only via the pool's freelist, which already enforces a
/// hand-off discipline. Resetting the generation to 0 is safe:
/// Crystalline-W grace ensures the previous incarnation's observers
/// have all drained before the pool slot becomes claimable again, and
/// within-incarnation observers will see monotonically-increasing
/// generations from any subsequent CAS.
LIBC_INLINE void init_live(cpp::Atomic<uint64_t> &live_state) {
    live_state.store(
        pack_live_state(static_cast<uint8_t>(VaChunkState::Live), 0, 0),
        cpp::MemoryOrder::RELAXED);
}

/// Returns the current chunk state.
///
/// ACQUIRE so the observer's subsequent dereferences of `chunk_base`
/// see the publisher's commit operations. The atomic argument is
/// non-`const` because `cpp::Atomic::load` is non-const in this
/// codebase.
[[nodiscard]] LIBC_INLINE VaChunkState
load_state(cpp::Atomic<uint64_t> &live_state) {
    return static_cast<VaChunkState>(
        state_of(live_state.load(cpp::MemoryOrder::ACQUIRE)));
}

[[nodiscard]] LIBC_INLINE bool is_live(cpp::Atomic<uint64_t> &live_state) {
    return load_state(live_state) == VaChunkState::Live;
}

[[nodiscard]] LIBC_INLINE uint32_t
load_count(cpp::Atomic<uint64_t> &live_state) {
    return count_of(live_state.load(cpp::MemoryOrder::ACQUIRE));
}

//===----------------------------------------------------------------------===//
// Per-slot canary derivation.
//
// Every va_tracker slot type (SkiplistNodeBase, ArtNodeBase, RegionDesc,
// Arena) carries a `node_canary` 64-bit field derived at init time and
// validated on every `*_free`. The derivation must be:
//
//   * Deterministic: the FreeFn computes the expected value identically.
//   * Cross-fork: re-derived in fork-reinit when partition_secret
//     rotates.
//   * Resistant to heap-spray crafting: a peer with arbitrary write
//     into a freed-but-unreused slot can craft `bucket` / `chunk_id` /
//     `slot_idx` but cannot guess `partition_secret` (ProcessPrng-
//     derived, Zone 0b PCB, never observable to user code).
//
// Formula: `partition_secret ^ pack(class_id, chunk_id, slot_idx)`.
// The pack discipline keeps each component in its own 8-/16-bit lane —
// no overlap, no aliasing — so distinct tuples cannot collide.
//
// Validated in `*_free` BEFORE any chunk-descriptor dereference, so a
// crafted slot cannot redirect a FreeFn into a victim chunk.
//===----------------------------------------------------------------------===//

[[nodiscard]] LIBC_INLINE uint64_t
compute_va_node_canary(uint64_t partition_secret, uint16_t class_id,
                       uint8_t chunk_id, uint8_t slot_idx) {
    uint64_t packed = (static_cast<uint64_t>(class_id) << 16) |
                      (static_cast<uint64_t>(chunk_id) << 8) |
                      static_cast<uint64_t>(slot_idx);
    return partition_secret ^ packed;
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_TRACKER_CHUNK_STATE_H
