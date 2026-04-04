//===-- Atomic occupancy bitmap ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Post-hoc occupancy bitmap used by SlabPool and IndexedPool. One bit per
// slot. Set on alloc (mark_live), cleared on free (mark_dead).
//
// IMPORTANT — what this is NOT:
//   Neither SlabPool nor IndexedPool claims slots via this bitmap.
//   - SlabPool claims via XOR-encoded freelist pop or bump counter.
//   - IndexedPool claims via CAS on the slot pointer itself.
//   The bitmap is observed, not scanned for free slots.
//
// Scope — this primitive centralises the atomic bit RMW + trap discipline
// shared by the two callers:
//   mark_live / mark_dead — fetch_or / fetch_and(RELAXED) with optional
//                           double-alloc / double-free trap derived from
//                           the RMW's prev-value return at zero extra cost.
//   is_live / popcount    — diagnostic reads.
//   clear_all             — fork-reinit / drain resets.
//
// Explicitly NOT in scope — `for_each_live`. Both callers prefetch a
// caller-specific address (slab_slot(slab, idx) for SlabPool; &slots[local]
// for IndexedPool) and funnel to caller-specific callbacks with different
// signatures. A generic walk would either (a) template the prefetch and
// bloat .text with a fresh walk body per unique lambda pair, or (b) go
// through a function-pointer prefetch adding ~3 cycles per live bit on
// dense walks. Neither trade-off is worth centralising ~25 lines of
// tzcnt/blsr mechanics. Each caller keeps its own inline walk and reads
// the bitmap via the words_[] accessor exposed here (`word_at(w, order)`).
//
// Trap discipline — template parameter so there is no runtime branch and
// SlabPool's existing codegen is preserved exactly:
//   trap_on_collision=true   IndexedPool. fetch_or/fetch_and inspect prev
//                            at zero extra cost (the RMW already returns
//                            prev). Double-alloc / double-free trap via
//                            __builtin_trap. Matches indexed_pool.h:677-680
//                            (mark_live) and :715-718 (mark_dead).
//   trap_on_collision=false  SlabPool. Raw RMW. Owner-tid invariant is
//                            asserted by the caller (slab_pool.h:731/742
//                            LIBC_ASSERT). No prev inspection — the
//                            compiler DCEs the AND+test+branch.
//
// Memory ordering — RELAXED for every RMW (matches both call sites).
// is_live also RELAXED. `word_at` takes the order as a template non-type
// parameter so the emitted load instruction is resolved at compile time
// (cpp::Atomic::load takes a runtime cpp::MemoryOrder — if it isn't a
// constant, clang branches on it or falls back to libatomic). SlabPool's
// walks instantiate <RELAXED>; IndexedPool's <ACQUIRE>.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PRIMITIVES_OCCUPANCY_BITMAP_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PRIMITIVES_OCCUPANCY_BITMAP_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/common.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace alloc_primitives {

inline constexpr size_t OCCUPANCY_BITS_PER_WORD = 64;

LIBC_INLINE constexpr size_t occupancy_words(size_t n_bits) {
  return (n_bits + OCCUPANCY_BITS_PER_WORD - 1) / OCCUPANCY_BITS_PER_WORD;
}

// Lock-free post-hoc occupancy bitmap. N = tracked slots.
template <size_t N, bool trap_on_collision = true> class AtomicBitmap {
  static_assert(N > 0, "empty bitmap is meaningless");
  static constexpr size_t WORDS = occupancy_words(N);

  // `mutable` so const observer methods (is_live, popcount, word_at) can
  // invoke cpp::Atomic::load, which is not const-qualified in libc's
  // atomic wrapper. Mirrors std::atomic's load-on-const convention and
  // matches the pattern InitLatch uses for state_. Writers (mark_live /
  // mark_dead / clear_all) are non-const and are protected by the usual
  // member-function constness of the enclosing primitive.
  mutable cpp::Atomic<uint64_t> words_[WORDS]{};

public:
  static constexpr size_t bit_count = N;
  static constexpr size_t word_count = WORDS;

  LIBC_INLINE AtomicBitmap() = default;
  AtomicBitmap(const AtomicBitmap &) = delete;
  AtomicBitmap &operator=(const AtomicBitmap &) = delete;

  // Transition bit idx from 0 -> 1 via fetch_or(RELAXED). With
  // trap_on_collision=true, traps on double-alloc using the prev value
  // returned by the RMW (zero extra load).
  LIBC_INLINE void mark_live(size_t idx) {
    LIBC_ASSERT(idx < N && "occupancy bitmap mark_live: idx out of range");
    const size_t w = idx / OCCUPANCY_BITS_PER_WORD;
    const unsigned b = static_cast<unsigned>(idx % OCCUPANCY_BITS_PER_WORD);
    const uint64_t mask = 1ULL << b;
    const uint64_t prev = words_[w].fetch_or(mask, cpp::MemoryOrder::RELAXED);
    if constexpr (trap_on_collision) {
      if (LIBC_UNLIKELY((prev & mask) != 0))
        __builtin_trap(); // double-alloc: bit already set
    } else {
      (void)prev;
    }
  }

  // Transition bit idx from 1 -> 0 via fetch_and(RELAXED). With
  // trap_on_collision=true, traps on double-free.
  LIBC_INLINE void mark_dead(size_t idx) {
    LIBC_ASSERT(idx < N && "occupancy bitmap mark_dead: idx out of range");
    const size_t w = idx / OCCUPANCY_BITS_PER_WORD;
    const unsigned b = static_cast<unsigned>(idx % OCCUPANCY_BITS_PER_WORD);
    const uint64_t mask = 1ULL << b;
    const uint64_t prev = words_[w].fetch_and(~mask, cpp::MemoryOrder::RELAXED);
    if constexpr (trap_on_collision) {
      if (LIBC_UNLIKELY((prev & mask) == 0))
        __builtin_trap(); // double-free: bit already clear
    } else {
      (void)prev;
    }
  }

  // Atomic try-acquire: boolean form of fetch_or. Returns true if *this
  // call* caused the 0→1 transition (we own the claim); false if the bit
  // was already 1 (someone else holds it).
  //
  // Intended use: per-slot claim lock when the occupancy bitmap doubles
  // as a lock-free mutual-exclusion primitive. Caller that wins
  // try_acquire has an exclusive window to CAS the associated payload
  // word (e.g. FdTable slot) and publish the transition; a concurrent
  // release's unconditional fetch_and remains safe because contending
  // installers block at try_acquire until the prev bit returns to 0.
  //
  // Non-trapping regardless of trap_on_collision — the "bit already set"
  // outcome is the negative branch, not an error.
  [[nodiscard]] LIBC_INLINE bool try_acquire(size_t idx) {
    LIBC_ASSERT(idx < N && "occupancy bitmap try_acquire: idx out of range");
    const size_t w = idx / OCCUPANCY_BITS_PER_WORD;
    const unsigned b = static_cast<unsigned>(idx % OCCUPANCY_BITS_PER_WORD);
    const uint64_t mask = 1ULL << b;
    const uint64_t prev = words_[w].fetch_or(mask, cpp::MemoryOrder::RELAXED);
    return (prev & mask) == 0;
  }

  // Non-trapping fetch_or / fetch_and. Use from callsites where bit-already-
  // set or bit-already-clear is a legitimate race outcome rather than a bug.
  //
  // IndexedPool::mark_live_bitmap_only is the motivating caller:
  // mark_dead's ordering is `memset(slot, 0)` → `fetch_and` on the bit, so a
  // concurrent acquire_for_scan → slot-CAS → mark_live_bitmap_only path can
  // observe slot==NULL with the prior occupant's bit still set. The set-bit
  // is about to be cleared by mark_dead; we just want the final bit = 1, and
  // the race-window prev-set must not trap.
  //
  // `set` / `clear` bypass trap_on_collision regardless of template setting
  // so a bitmap can host both trap-guarded owner paths (mark_live / mark_dead)
  // and race-tolerant scan paths (set / clear) on the same storage.
  LIBC_INLINE void set(size_t idx) {
    LIBC_ASSERT(idx < N && "occupancy bitmap set: idx out of range");
    const size_t w = idx / OCCUPANCY_BITS_PER_WORD;
    const unsigned b = static_cast<unsigned>(idx % OCCUPANCY_BITS_PER_WORD);
    words_[w].fetch_or(1ULL << b, cpp::MemoryOrder::RELAXED);
  }

  LIBC_INLINE void clear(size_t idx) {
    LIBC_ASSERT(idx < N && "occupancy bitmap clear: idx out of range");
    const size_t w = idx / OCCUPANCY_BITS_PER_WORD;
    const unsigned b = static_cast<unsigned>(idx % OCCUPANCY_BITS_PER_WORD);
    words_[w].fetch_and(~(1ULL << b), cpp::MemoryOrder::RELAXED);
  }

  // Snapshot read. RELAXED load.
  [[nodiscard]] LIBC_INLINE bool is_live(size_t idx) const {
    LIBC_ASSERT(idx < N && "occupancy bitmap is_live: idx out of range");
    const size_t w = idx / OCCUPANCY_BITS_PER_WORD;
    const unsigned b = static_cast<unsigned>(idx % OCCUPANCY_BITS_PER_WORD);
    return ((words_[w].load(cpp::MemoryOrder::RELAXED) >> b) & 1ULL) != 0ULL;
  }

  // Snapshot live count. Diagnostic / assertion only. RELAXED loads across
  // all WORDS; trailing words are zero for callers that use only the first
  // `bitmap_word_count` (SlabPool) / BITMAP_WORDS (IndexedPool).
  [[nodiscard]] LIBC_INLINE size_t popcount() const {
    size_t count = 0;
    for (size_t w = 0; w < WORDS; ++w)
      count += static_cast<size_t>(__builtin_popcountll(
          words_[w].load(cpp::MemoryOrder::RELAXED)));
    return count;
  }

  // Read one backing word with a compile-time memory order. For caller-
  // written walks: SlabPool uses <RELAXED>, IndexedPool uses <ACQUIRE>.
  // The Order template parameter guarantees the load instruction is
  // selected at compile time instead of routed through a runtime switch.
  template <cpp::MemoryOrder Order = cpp::MemoryOrder::RELAXED>
  [[nodiscard]] LIBC_INLINE uint64_t word_at(size_t w) const {
    LIBC_ASSERT(w < WORDS && "occupancy bitmap word_at: idx out of range");
    return words_[w].load(Order);
  }

  // Reset all bits to 0 via RELAXED stores — matches the pre-migration
  // hand-rolled loops exactly. Callers that need the cleared state
  // published to other threads must publish via a control word (e.g. a
  // generation bump with RELEASE); this primitive does not establish
  // happens-before on its own. Used by fork-reinit and drain paths.
  LIBC_INLINE void clear_all() {
    for (size_t w = 0; w < WORDS; ++w)
      words_[w].store(0, cpp::MemoryOrder::RELAXED);
  }

  // Reset the first `n` backing words to 0. For callers whose live slot
  // count is bounded by a per-instance word count smaller than WORDS
  // and who know trailing words are already zero by invariant
  // (SlabPool: prior full_release is gated on returned == bump, so
  // every bit is clear by the time a slab enters the recycle list;
  // fresh slabs get zero-filled by placeholder_commit on the header
  // page). Shares clear_all's RELAXED-store + caller-publication
  // contract.
  LIBC_INLINE void clear_first_words(size_t n) {
    LIBC_ASSERT(n <= WORDS && "clear_first_words: n exceeds bitmap capacity");
    for (size_t w = 0; w < n; ++w)
      words_[w].store(0, cpp::MemoryOrder::RELAXED);
  }
};

// Non-atomic variant for callers that hold an external lock or own the
// bitmap thread-locally (LockTable, owned_pty tables). API parity with
// AtomicBitmap modulo memory-order parameters.
template <size_t N, bool trap_on_collision = true> class RelaxedBitmap {
  static_assert(N > 0, "empty bitmap is meaningless");
  static constexpr size_t WORDS = occupancy_words(N);

  uint64_t words_[WORDS]{};

public:
  static constexpr size_t bit_count = N;
  static constexpr size_t word_count = WORDS;

  LIBC_INLINE RelaxedBitmap() = default;
  RelaxedBitmap(const RelaxedBitmap &) = delete;
  RelaxedBitmap &operator=(const RelaxedBitmap &) = delete;

  LIBC_INLINE void mark_live(size_t idx) {
    LIBC_ASSERT(idx < N && "RelaxedBitmap mark_live: idx out of range");
    const size_t w = idx / OCCUPANCY_BITS_PER_WORD;
    const unsigned b = static_cast<unsigned>(idx % OCCUPANCY_BITS_PER_WORD);
    const uint64_t mask = 1ULL << b;
    if constexpr (trap_on_collision) {
      if (LIBC_UNLIKELY((words_[w] & mask) != 0))
        __builtin_trap();
    }
    words_[w] |= mask;
  }

  LIBC_INLINE void mark_dead(size_t idx) {
    LIBC_ASSERT(idx < N && "RelaxedBitmap mark_dead: idx out of range");
    const size_t w = idx / OCCUPANCY_BITS_PER_WORD;
    const unsigned b = static_cast<unsigned>(idx % OCCUPANCY_BITS_PER_WORD);
    const uint64_t mask = 1ULL << b;
    if constexpr (trap_on_collision) {
      if (LIBC_UNLIKELY((words_[w] & mask) == 0))
        __builtin_trap();
    }
    words_[w] &= ~mask;
  }

  [[nodiscard]] LIBC_INLINE bool is_live(size_t idx) const {
    LIBC_ASSERT(idx < N && "RelaxedBitmap is_live: idx out of range");
    const size_t w = idx / OCCUPANCY_BITS_PER_WORD;
    const unsigned b = static_cast<unsigned>(idx % OCCUPANCY_BITS_PER_WORD);
    return ((words_[w] >> b) & 1ULL) != 0ULL;
  }

  [[nodiscard]] LIBC_INLINE size_t popcount() const {
    size_t count = 0;
    for (size_t w = 0; w < WORDS; ++w)
      count += static_cast<size_t>(__builtin_popcountll(words_[w]));
    return count;
  }

  [[nodiscard]] LIBC_INLINE uint64_t word_at(size_t w) const {
    LIBC_ASSERT(w < WORDS && "RelaxedBitmap word_at: idx out of range");
    return words_[w];
  }

  LIBC_INLINE void clear_all() {
    __builtin_memset(&words_[0], 0, sizeof(words_));
  }

  LIBC_INLINE void clear_first_words(size_t n) {
    LIBC_ASSERT(n <= WORDS && "clear_first_words: n exceeds bitmap capacity");
    __builtin_memset(&words_[0], 0, n * sizeof(words_[0]));
  }
};

} // namespace alloc_primitives
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PRIMITIVES_OCCUPANCY_BITMAP_H
