//===- va_tracker_chunk_state.h - chunk lifecycle state machine -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Three-state lifecycle (Live -> Draining -> Dead) for va_tracker chunk
// descriptors, plus the composite (state | count | gen) live_state word
// that linearises drain transitions against the count change that
// triggers them.
//
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

enum class VaChunkState : uint8_t {
    Live = 0,
    // Live->Draining CAS winner owns the reclamation sequence; other
    // observers skip the chunk. Set by `release_va_chunk_slot`.
    Draining = 1,
    // Tombstone written by the FreeFn after Crystalline grace closes,
    // before the descriptor returns to the pool freelist. Overwritten to
    // Live=0 by the next `va_chunk_desc_pool_claim` memset, so it is only
    // observable to a straggler that races a stale `cd` pointer against
    // pool reclamation — the chunk_table entry was already cleared on
    // Live->Draining, so no reader reaches Dead through the table.
    Dead = 2,
};

// Composite live_state layout: (state:8 | count:24 | gen:32). One atomic
// so the Live->Draining transition CAS expecting `(Live, 1)` cannot
// succeed after a peer increment moves the snapshot to `(Live, 2)` —
// splitting state and count would let the releaser miss a concurrent
// reservation. Generation bumps on every successful CAS for ABA defence
// against within-incarnation churn; Crystalline-W pinning handles
// cross-incarnation ABA on the pool freelist.

inline constexpr uint32_t kLiveStateStateBits = 8;
inline constexpr uint32_t kLiveStateCountBits = 24;
inline constexpr uint32_t kLiveStateGenShift = 32;
inline constexpr uint64_t kLiveStateStateMask = 0xFFu;
inline constexpr uint64_t kLiveStateCountMax =
    (uint64_t{1} << kLiveStateCountBits) - 1;
inline constexpr uint64_t kLiveStateGenIncrement =
    uint64_t{1} << kLiveStateGenShift;

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

// Allocator pre-reserve. The reservation IS the count increment — no
// separate fetch_add after bitmap acquire. While held, the chunk cannot
// transition to Draining (`release_va_chunk_slot`'s drain CAS requires
// count == 0). ACQ_REL on success pairs the publisher's chunk-init
// writes with the caller's subsequent slot dereference.
[[nodiscard]] LIBC_INLINE bool
try_va_chunk_reserve(cpp::Atomic<uint64_t> &live_state, uint32_t cap) {
    uint64_t cur = live_state.load(cpp::MemoryOrder::ACQUIRE);
    for (;;) {
        if (state_of(cur) != static_cast<uint8_t>(VaChunkState::Live))
            return false;
        uint32_t cnt = count_of(cur);
        if (cnt >= cap)
            return false;
        uint64_t next = pack_live_state(
            static_cast<uint8_t>(VaChunkState::Live), cnt + 1,
            gen_of(cur) + 1);
        if (live_state.compare_exchange_weak(cur, next,
                                             cpp::MemoryOrder::ACQ_REL,
                                             cpp::MemoryOrder::ACQUIRE))
            return true;
    }
}

// Releaser plus reservation rollback. Folds the decrement and the
// Live->Draining transition into one CAS so a peer reservation cannot
// race into the gap a fetch_sub-then-CAS pair would open. Traps on
// count == 0 entry (double release or release without reservation).
// Returns true iff this caller is the unique drain winner — must then
// clear chunk_table, decommit, retire the descriptor.
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

// Single-writer (Crystalline-W FreeFn invariant). Preserves and bumps
// generation so a straggler observer with a stale snapshot still sees a
// distinct value. RELEASE so any straggler that pinned `desc` before the
// FreeFn ran sees the Dead marker once it reloads `live_state` ACQUIRE.
LIBC_INLINE void mark_dead(cpp::Atomic<uint64_t> &live_state) {
    uint64_t cur = live_state.load(cpp::MemoryOrder::RELAXED);
    live_state.store(pack_live_state(
                         static_cast<uint8_t>(VaChunkState::Dead), 0,
                         gen_of(cur) + 1),
                     cpp::MemoryOrder::RELEASE);
}

// Resets generation to 0; safe because Crystalline-W grace has already
// drained every previous-incarnation observer before this slot becomes
// claimable again.
LIBC_INLINE void init_live(cpp::Atomic<uint64_t> &live_state) {
    live_state.store(
        pack_live_state(static_cast<uint8_t>(VaChunkState::Live), 0, 0),
        cpp::MemoryOrder::RELAXED);
}

// ACQUIRE pairs with the publisher's chunk-init writes so the caller's
// subsequent `chunk_base` dereferences see committed pages.
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

// Per-slot canary `partition_secret ^ pack(class_id, chunk_id, slot_idx)`,
// packed into disjoint 16/8/8-bit lanes so distinct tuples cannot collide.
// `partition_secret` is ProcessPrng-derived in PCB Zone 0b and unreachable
// to user code, so a heap-spray attacker cannot craft the canary.
// Validated in every `*_free` BEFORE chunk-descriptor dereference.
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
