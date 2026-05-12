//===-- Slab pool allocator for Windows ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Runtime-configurable slab pool. Per-slab freelists with thread ownership
// eliminate atomics on the alloc/free hot path (SLUB/mimalloc model).
// Placeholder-backed VA lifecycle prevents use-after-release races.
//
// Slab layout (64KB, one allocation-granularity unit):
//   [header 4KB] [guard 4KB] [slots 52KB] [guard 4KB]
// Header and body are split placeholders with independent lifecycles.
// Guard pages are never-committed placeholder regions (zero cost).
//
// Hardening:
//   - Freelist pointers XOR'd with per-slab cookie + storage address on
//     BOTH local_free and xthread chains (uniform encoding)
//   - Zero-on-free prevents info leaks from reused slots
//   - Double-free canary from independent per-slab key + slot address
//   - Canary cleared on alloc to prevent false positives
//   - Guard pages isolate header from slots (underflow) and catch
//     overflow past the last slot — both MMU-enforced, zero runtime cost
//   - Released slabs become placeholders (VA stays reserved) —
//     concurrent access faults cleanly, foreign allocations blocked
//
// Lifecycle (epoch-based):
//   free() never triggers slab release. Empty abandoned slabs are
//   detected and released during alloc_slow() adoption or abandon().
//   This eliminates the class of release races between concurrent
//   freeing threads — the release decision is always single-threaded.
//
// Slab bodies are backed by VaSubstrate sub-slots (Medium for the
// default 64 KB pool, Huge for the 1 MB L3 pool). SlabRegistry's
// L1/L2/L3 directory pages also come from the substrate (Small or
// XLarge for L1 depending on VA width; Small for L2 and L3). Each
// substrate slot stamps its VA into the libc mapping table as
// LIBC_INTERNAL via the substrate's per-arena registration, so slab
// and directory churn is invisible to the table and the population
// stays O(arenas), not O(slabs). The remaining direct page_*
// primitives (page_protect / page_commit / page_decommit) operate
// only on per-page state INSIDE already-substrate-owned slots — see
// the "Substrate does not track per-page state inside a live slot"
// preservation guarantee in `lets-migrate-slabpool-first-lazy-meteor.md`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SLAB_POOL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SLAB_POOL_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/primitives/canary_seed.h"
#include "src/__support/OSUtil/windows/alloc/primitives/init_latch.h"
#include "src/__support/OSUtil/windows/alloc/primitives/occupancy_bitmap.h"
#include "src/__support/OSUtil/windows/alloc/legacy/va_substrate.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_serial_table.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/libc_assert.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Overlaid on a freed slot for freelist linkage.
struct SlabFreeNode {
  SlabFreeNode *next;
};

// xthread_free encoding (intra-slab offset only).
//
// xthread_free nodes always reside within the same slab that owns the
// atomic. Since slabs are aligned to SlabBytesV (a power-of-two multiple
// of the 64 KB NT granule), the low log2(SlabBytesV) bits of any node
// address are the intra-slab offset, and `slab_base | offset` reconstructs
// the full pointer.
//
// Offset extraction uses a bitmask (& (kSlabBytes - 1)) rather than
// subtraction. A corrupt pointer produces a wrong intra-slab offset but
// cannot bleed into other fields — the mask makes overflow impossible by
// construction, unlike subtraction which silently truncates.
//
// Zero offset encodes nullptr (offset 0 is the header page, never a slot).
//
// There is no ABA generation counter because there is no CAS-pop anywhere:
// producers use XCHG-push (Vyukov MPSC), consumers use XCHG-drain. Both
// unconditionally replace the atomic word — no compare, no stale-read
// window, no tag needed. See xthread_push / drain_xthread below.
//
// The helpers are templates rather than free functions keyed on a runtime
// slab base because the offset mask depends on the arena size — different
// SlabPoolT<N> instantiations need different masks.
static_assert(sizeof(void *) == 8, "tagged-pointer requires 64-bit");

// ===----------------------------------------------------------------------===//
// SlabLayout<SlabBytesV>
//
// Compile-time layout constants derived from the per-pool arena size.
// Every SlabPoolT<SlabBytesV> and SlabHeaderT<SlabBytesV> instantiation
// pulls these in as `Layout = SlabLayout<SlabBytesV>`.
//
// Default SlabBytesV = 65536 (one NT allocation granule) matches the
// original slab layout byte-for-byte — every existing caller that uses
// the SlabPool / SlabHeader aliases compiles and behaves unchanged.
// Larger arenas (e.g. 1 MB for multi-granule slabs) are expressed by
// instantiating SlabPoolT<N> directly.
// ===----------------------------------------------------------------------===//
template <size_t SlabBytesV = 65536,
          size_t MinSlotBytesV = sizeof(SlabFreeNode)>
struct SlabLayout {
  static_assert(SlabBytesV >= 65536,
                "slab arena must be at least one NT allocation granule");
  static_assert((SlabBytesV & (SlabBytesV - 1)) == 0,
                "slab arena must be a power of two so ptr_to_slab's "
                "~(kSlabBytes - 1) mask and the xthread offset mask are "
                "both single-instruction bitops");
  static_assert((SlabBytesV % 65536) == 0,
                "slab arena must be a multiple of the 64 KB NT granule");
  static_assert(MinSlotBytesV >= sizeof(SlabFreeNode),
                "MinSlotBytesV must be at least the freelist node size");

  static constexpr size_t kSlabBytes = SlabBytesV;
  static constexpr size_t kMinSlotBytes = MinSlotBytesV;
  static constexpr size_t kPageSize = 4096;
  // [header 4KB] [guard 4KB] [slots ...] [guard 4KB]
  static constexpr size_t kLeadingGuardOffset = kPageSize;
  static constexpr size_t kSlotStartOffset = 2 * kPageSize;
  static constexpr size_t kTrailingGuardOffset = kSlabBytes - kPageSize;
  static constexpr size_t kUsableBytes =
      kTrailingGuardOffset - kSlotStartOffset;
  static constexpr size_t kBodyOffset = kPageSize;
  static constexpr size_t kBodySize = kSlabBytes - kPageSize;

  // Canary lives at offset 8 (after the freelist pointer). Arena-independent
  // but kept here so a single Layout:: prefix fetches every layout constant.
  static constexpr size_t kCanaryOffset = sizeof(SlabFreeNode);

  // Number of slot-region pages. For default (65536): 52 KB / 4 KB = 13.
  static constexpr unsigned kSlotPages =
      (kTrailingGuardOffset - kSlotStartOffset) / kPageSize;

  // page_state packs 2 bits per slot-region page into uint64_t words.
  // 32 pages per word keeps the default (kSlotPages = 13) in a single
  // word — compiled as the scalar load/store it was pre-templating —
  // while a 1 MB arena (kSlotPages = 253) spreads over 8 words without
  // any layout-level constraint. Accessors compute (word, shift) at O(1).
  static constexpr unsigned kStateBitsPerPage = 2;
  static constexpr unsigned kStateWords = (kSlotPages + 31) / 32;

  // Upper bound on slot count for any instantiation of this pool. Drives
  // the compile-time size of the occupancy bitmap in SlabHeaderT. The
  // default MinSlotBytesV = sizeof(SlabFreeNode) matches the historical
  // worst-case sizing (6656 slots for a 64 KB arena). A pool that holds
  // only larger slots (e.g. the L3 pool at 65664 B per slot in a 1 MB
  // arena) can pass a matching MinSlotBytesV and collapse its bitmap
  // from 16 KB to a single 64-bit word.
  static constexpr unsigned kMaxSlots = kUsableBytes / kMinSlotBytes;
  static_assert(kMaxSlots > 0, "arena cannot hold any slot of MinSlotBytesV");

  // xthread tagged-pointer geometry derives from the arena size.
  //   [gen : 64 - log2(kSlabBytes)] [offset : log2(kSlabBytes)]
  // For default 65536: 16 offset bits, 48 gen bits.
  // For 1 MB arena:    20 offset bits, 44 gen bits.
  static constexpr int kXthreadTagShift = __builtin_ctzll(kSlabBytes);
  static constexpr uintptr_t kXthreadOffsetMask =
      static_cast<uintptr_t>(kSlabBytes) - 1;
};

// Default-arena aliases. Every call site outside SlabPool/SlabHeader that
// references these names continues to resolve to the 64 KB values it
// always did. Inside the class templates the same names are re-declared
// at class scope as `static constexpr size_t kSlabBytes = Layout::kSlabBytes;`
// (etc.), shadowing these for per-instantiation values without requiring
// the 100+ internal references to change.
inline constexpr size_t kSlabBytes = SlabLayout<>::kSlabBytes;   // 65536
inline constexpr size_t kPageSize = SlabLayout<>::kPageSize;     // 4096
inline constexpr size_t kLeadingGuardOffset =
    SlabLayout<>::kLeadingGuardOffset;
inline constexpr size_t kSlotStartOffset = SlabLayout<>::kSlotStartOffset;
inline constexpr size_t kTrailingGuardOffset =
    SlabLayout<>::kTrailingGuardOffset;
inline constexpr size_t kUsableBytes = SlabLayout<>::kUsableBytes; // 53248
inline constexpr size_t kBodyOffset = SlabLayout<>::kBodyOffset;
inline constexpr size_t kBodySize = SlabLayout<>::kBodySize;      // 60 KB
inline constexpr size_t kCanaryOffset = SlabLayout<>::kCanaryOffset;
inline constexpr unsigned kSlotPages = SlabLayout<>::kSlotPages;  // 13

// Validates xthread_free nullptr sentinel: offset 0 is the header page,
// which is outside the slot region and can never be a valid slot pointer.
static_assert(SlabLayout<>::kSlotStartOffset > 0,
              "offset 0 must be outside the slot region (nullptr sentinel)");

// ===----------------------------------------------------------------------===//
// PageBitset<N>
//
// Small bitset sized to the slot-region page count of the enclosing slab.
// Used for the drain/compact-local sealed/touched/unsealed masks so their
// width tracks kSlotPages: one uint64_t for any arena ≤ 256 KB (kSlotPages
// ≤ 64), an array of uint64_t words for larger arenas.
//
// The default SlabPool (kSlotPages = 13) picks the single-word specialisation,
// which compiles to the same tzcnt/blsr instructions the pre-template code
// used — except the variable is uint64_t instead of uint32_t, which is free
// on x86_64 / AArch64 and bumps the supported arena ceiling from 128 KB to
// 256 KB in the common-case specialisation.
//
// `first_set` returns kSlotPages as a "no bits set" sentinel; `clear_lowest`
// is blsr. Keeping the contract tight lets callers write the same loop
// shape as the pre-template uint32_t version:
//
//     for (PageBitset<...> m = ...; m.any(); m.clear_lowest()) {
//       unsigned pg = m.first_set();
//       ...
//     }
// ===----------------------------------------------------------------------===//
template <unsigned N, bool = (N <= 64)>
struct PageBitset;

template <unsigned N>
struct PageBitset<N, /*SingleWord=*/true> {
  uint64_t bits = 0;

  LIBC_INLINE void set(unsigned pg) {
    LIBC_ASSERT(pg < N);
    bits |= uint64_t{1} << pg;
  }
  LIBC_INLINE bool test(unsigned pg) const {
    LIBC_ASSERT(pg < N);
    return (bits >> pg) & 1u;
  }
  LIBC_INLINE bool any() const { return bits != 0; }
  LIBC_INLINE unsigned first_set() const {
    LIBC_ASSERT(bits != 0);
    return static_cast<unsigned>(__builtin_ctzll(bits));
  }
  LIBC_INLINE void clear_lowest() { bits &= bits - 1; } // blsr
  LIBC_INLINE static PageBitset all() {
    // All-ones below bit N; `N == 64` takes the saturate branch to avoid
    // the UB of a 64-bit shift-by-64 on the (uint64_t{1} << N) form.
    PageBitset m;
    m.bits = (N == 64) ? ~uint64_t{0} : ((uint64_t{1} << N) - 1);
    return m;
  }
};

template <unsigned N>
struct PageBitset<N, /*SingleWord=*/false> {
  static constexpr unsigned kWords = (N + 63) / 64;
  uint64_t bits[kWords] = {};

  LIBC_INLINE void set(unsigned pg) {
    LIBC_ASSERT(pg < N);
    bits[pg >> 6] |= uint64_t{1} << (pg & 63);
  }
  LIBC_INLINE bool test(unsigned pg) const {
    LIBC_ASSERT(pg < N);
    return (bits[pg >> 6] >> (pg & 63)) & 1u;
  }
  LIBC_INLINE bool any() const {
    for (unsigned w = 0; w < kWords; w++)
      if (bits[w] != 0)
        return true;
    return false;
  }
  LIBC_INLINE unsigned first_set() const {
    for (unsigned w = 0; w < kWords; w++)
      if (bits[w] != 0)
        return w * 64 + static_cast<unsigned>(__builtin_ctzll(bits[w]));
    LIBC_ASSERT(false && "first_set() on empty PageBitset");
    return N;
  }
  LIBC_INLINE void clear_lowest() {
    for (unsigned w = 0; w < kWords; w++) {
      if (bits[w] != 0) {
        bits[w] &= bits[w] - 1;
        return;
      }
    }
  }
  LIBC_INLINE static PageBitset all() {
    PageBitset m;
    unsigned full = N / 64;
    unsigned rem = N & 63;
    for (unsigned w = 0; w < full; w++)
      m.bits[w] = ~uint64_t{0};
    if (rem != 0)
      m.bits[full] = (uint64_t{1} << rem) - 1;
    return m;
  }
};

template <size_t SlabBytesV = 65536,
          size_t MinSlotBytesV = sizeof(SlabFreeNode)>
class SlabPoolT;
using SlabPool = SlabPoolT<>; // Default 64 KB arena for existing callers.

// Per-slab header. Cache-line split: owner fields (line 0) vs
// cross-thread atomic (line 1) to prevent false sharing.
// Page occupancy counters on line 2 (owner-only, separate from xthread).
//
// Templated on SlabBytesV *and* MinSlotBytesV so page_occupancy[kSlotPages],
// page_state[kStateWords], and the occupancy bitmap (AtomicBitmap<kMaxSlots>)
// all size to the per-pool layout at compile time. Default-arena uses keep
// working via `using SlabHeader = SlabHeaderT<>` below.
template <size_t SlabBytesV = 65536,
          size_t MinSlotBytesV = sizeof(SlabFreeNode)>
struct alignas(64) SlabHeaderT
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
  // [0..19] Intrusive Crystalline-W runtime fields emitted directly so
  // SlabHeaderT is standard-layout. `crystalline_serial` packs at the
  // natural 4-byte slot at offset 20 immediately after batch_link.
  LIBC_CRYSTALLINE_NODE_FIELDS(SlabHeaderT);

  using Layout = SlabLayout<SlabBytesV, MinSlotBytesV>;

  // Class-scope layout constants shadow the file-scope names so the ~40
  // internal references (page_occupancy[kSlotPages], kCanaryOffset, etc.)
  // resolve to per-instantiation values without needing Layout:: prefixes.
  static constexpr size_t kSlabBytes = Layout::kSlabBytes;
  static constexpr size_t kPageSize = Layout::kPageSize;
  static constexpr size_t kLeadingGuardOffset = Layout::kLeadingGuardOffset;
  static constexpr size_t kSlotStartOffset = Layout::kSlotStartOffset;
  static constexpr size_t kTrailingGuardOffset = Layout::kTrailingGuardOffset;
  static constexpr size_t kUsableBytes = Layout::kUsableBytes;
  static constexpr size_t kBodyOffset = Layout::kBodyOffset;
  static constexpr size_t kBodySize = Layout::kBodySize;
  static constexpr size_t kCanaryOffset = Layout::kCanaryOffset;
  static constexpr unsigned kSlotPages = Layout::kSlotPages;
  static constexpr unsigned kStateWords = Layout::kStateWords;

  // -- Cache line 0: inherited CrystallineNode + crystalline_serial + pad --
  //
  // The 24 bytes inherited from CrystallineNode (next/slot/birth_era
  // union, refs/batch_next union, batch_link) live at offset 0..23 and
  // are owned exclusively by the Crystalline runtime once the slab has
  // been retired via slab_retire_domain_. Pre-retire those bytes are
  // zero (CrystallineDomain::init_node stamps them at retire-publish
  // time, not allocation time).
  //
  // `crystalline_serial` is the BatchLinkCodec<SlabHeader>'s decode
  // key. Stamped once at `init_slab` from the per-pool serial counter
  // (`crystalline_serial_counter_`); registered into the per-pool
  // `crystalline_serial_table_`. It packs into the 4-byte tail pad of
  // the inherited CrystallineNode (offsets 20..23) under the Itanium-
  // ABI tail-padding reuse rule, so adding it costs zero extra bytes.
  //
  // The 36-byte pad below pushes owner-mutable fields onto cache line
  // 1 — Crystalline's post-retire writes (chain links, refcount
  // adjustments) cannot collide with owner-side writes still in
  // flight on a slab observed by a concurrent reader. Mirrors the
  // layout choice made by ArenaHeader (va_substrate.h:294).
  uint32_t crystalline_serial{0};
  uint8_t _pad_post_crystalline[36];

  // -- Cache line 1: owner-only (no atomics) --
  SlabFreeNode *local_free;   // Owner-only freelist (raw head pointer).
  uint16_t bump;              // Next virgin slot index.
  uint16_t slots_per_slab;    // Computed from slot_size at init.
  // The (tid, seal_seq) pair previously kept here as `owner_tid`
  // moved to cache line 2 as the packed `state_` atomic — see the
  // declaration there for rationale. The 4 bytes are reclaimed as
  // explicit padding to keep cache line 1 byte-for-byte stable.
  uint32_t _pad_owner_tid_moved{0};
  SlabHeaderT *next_abandoned; // Abandoned-stack Treiber link.
  SlabHeaderT *all_next;       // Doubly-linked all-slabs list.
  SlabHeaderT *all_prev;       // Enables O(1) unlink on release.
  uintptr_t freelist_cookie;  // Per-slab XOR cookie for hardening.
  uint16_t returned;          // Slots returned (owner-only, plain).
  uint16_t bump_offset;       // Random start offset for bump allocation.
  uint8_t class_index;        // Pool identifier for multi-class dispatch.
  uint8_t _pad_class{0};      // Explicit padding before the 2-aligned word count.
  // Precomputed (slots_per_slab + 63) / 64. uint16_t so future SlabPoolT
  // instantiations with small MinSlotBytesV in a large arena (slots up
  // to 65535 → up to 1024 words) cannot silently truncate.
  uint16_t bitmap_word_count;
  SlabPoolT<SlabBytesV, MinSlotBytesV> *pool; // Backpointer for TLS cleanup.

  // -- Cache line 2: Vyukov MPSC queue state + read-only metadata --
  //
  // Cross-thread MPSC queue for slots freed by non-owner threads,
  // mirroring the design in signal/pending/rt_queue.h:
  //
  //   xthread_stub   — permanent sentinel node. Initial head AND tail,
  //                    re-inserted as new tail during drain when all
  //                    real entries have been dequeued. Its `next` is
  //                    atomically accessed by producers and consumers.
  //   xthread_head   — consumer-private head. Walks forward through
  //                    the queue; advances one link per try_pop.
  //   xthread_tail   — cross-thread atomic tail. Producers XCHG here
  //                    in Phase 2; consumers read to detect the
  //                    "h is current tail → re-insert stub" branch.
  //                    Stored as an intra-slab offset via xthread_pack
  //                    so the same encoding/bounds-check machinery as
  //                    the rest of the freelist applies.
  //
  // Producer cost: wait-free (one RELEASE store, one ACQ_REL XCHG, one
  // RELEASE store). Consumer cost: walks head→next one entry at a time,
  // taking ownership only after the back-link is visible — never waits
  // long, never leaks. See drain_xthread and xthread_push below for the
  // full algorithm and the rt_queue parallel.
  alignas(64) SlabFreeNode xthread_stub;
  SlabFreeNode *xthread_head;
  cpp::Atomic<uintptr_t> xthread_tail;
  uintptr_t canary_key;   // Independent of freelist_cookie. Read-only after init.
  // Slab authenticity tag: global_secret ^ slab_base. Read-only after init.
  // Replaces 4 field-consistency checks (pool, slot_size, slots_per_slab,
  // bump/returned) with one unforgeable validation. Catches non-slab
  // pointers, corrupted headers, use-after-release, and cross-pool
  // misrouting — strictly stronger than the individual field checks.
  // Cost: 1 XOR + 1 CMP, on a cache line already fetched for
  // slot_size_recip and canary_key.
  uintptr_t slab_tag;
  // Reciprocal multiplier for slot index computation. Set once at init.
  // slot_index = (byte_offset * slot_size_recip) >> 32. Avoids a
  // runtime division (20-90 cycles) on the alloc/free bitmap hot path.
  // Formula: slot_size_recip = ceil(2^32 / slot_size).
  uint32_t slot_size_recip;
  // Byte size of each slot. Widened from uint16_t so the L3 pool's
  // 65664 B slot size fits; parked on this cache line alongside the
  // other read-only-after-init metadata (slot_size_recip, canary_key,
  // slab_tag) so the alloc/free hot paths that touch slot_size_recip
  // already have the line warm.
  uint32_t slot_size;
  // Packed (seal_seq : 32 | tid : 32) ownership word.
  //
  // ALIVE: low 32 = owning thread's TID; SEALED: low 32 = 0. The high
  // 32 are a per-slab seal-cycle sequence number bumped on EVERY tid
  // transition (init claim, abandon release, adoption claim, re-abandon
  // release, fork-reinit claim/release). Single-writer per transition
  // (the slab's ownership protocol serialises owner / adopter), so
  // incrementing the seq via load+store on the writer is race-free; the
  // RELEASE store of the packed 64-bit value publishes the new seq
  // simultaneously with the new tid.
  //
  // Cross-thread freers MUST capture `state_.load(ACQUIRE)` BEFORE
  // `xthread_push` and AGAIN after, then enter the post-seal fetch_add
  // / last-freer path only when (i) `pre == post` (no transition during
  // my push) AND (ii) low-32 of post == 0 (sealed cycle). The single
  // packed atomic gives a consistent (seq, tid) snapshot per load —
  // there is no cross-atomic ordering puzzle. This closes the phantom-
  // freer race in which a pre-drain push has already been counted in
  // `returned` but the freer's tid recheck observes the post-abandon
  // tid=0 and would otherwise contribute a stale fetch_add to the new
  // cycle's sealed_target, retiring the slab before its real
  // outstanding slots are freed (UAF on a later release / app free).
  cpp::Atomic<uint64_t> state_;

  // -- Seal-state (set at abandon, transitions on adoption) --
  //
  // sealed_target: number of cross-thread frees the slab is waiting on
  //   before it can self-retire. Equal to (bump - returned) at the
  //   moment owner releases the slab. Read by post-seal cross-thread
  //   freers AFTER their fetch_add on `xthread_returned_count`. Atomic
  //   so the read/write across the owner/freer boundary is race-free
  //   under the C++17 memory model — adopter resets it concurrently
  //   with stale freers in the in-flight window between abandon and
  //   re-claim. RELAXED ordering is sufficient: the synchronisation
  //   point is the packed `state_` RELEASE store inside claim/release,
  //   which transitively publishes every prior write on this cache
  //   line — and the cross-thread freer's pre/post `state_` snapshot
  //   check additionally rejects any fetch_add whose push window
  //   straddled an ownership transition (closes the phantom-freer
  //   race).
  //
  // xthread_returned_count: each post-seal cross-thread freer's
  //   fetch_add returns the new count. The freer that observes
  //   new_count == sealed_target is the last freer.
  //
  // retire_initiated: three-state ladder enforcing exactly one retire
  //   path per slab while closing the adoption-vs-stale-freer race.
  //
  //     kRetireIdle (0)     — slab is sealed; freers' last-freer CAS
  //                            may transition 0 → kRetireDone.
  //     kRetireDone (1)     — terminal. Crystalline owns the slab.
  //     kRetireAdopting (2) — adopter holds a transition lock across
  //                            its claim/reset/drain sequence (and,
  //                            for an adopted slab, across its entire
  //                            alive lifetime). Freers' last-freer
  //                            CAS sees 2 and bails.
  //
  //   Without the kRetireAdopting state, an in-flight freer that
  //   observed tid==0 BEFORE the adopter's claim could match a stale
  //   sealed_target / xthread_returned_count and win the
  //   `retire_initiated 0→1` CAS, retiring a slab the adopter has
  //   just claimed. The lock-take CAS converts that race into a
  //   gated CAS-failure for the freer.
  //
  //   The lock is released back to kRetireIdle ONLY at the next
  //   abandon (re-seal path) — and the abandon's own
  //   `release() // RELEASE tid=0` publishes the new
  //   sealed_target / count to any subsequent freer that sees tid==0,
  //   so the freer always observes a consistent (count=0, target=K)
  //   pair for the new cycle.
  //
  // Total: 2 + 2 + 4 = 8 bytes — preserves cache-line-2 size, so
  // page_occupancy stays at offset 192 with no further shifts.
  //
  // retire_initiated is packed: low 8 bits = state byte
  // (kRetireIdle/kRetireDone/kRetireAdopting), high 24 bits = cycle
  // sequence number (mod 2^24, taken from state_.seq at the moment the
  // value was last published). Every writer of retire_initiated stamps
  // the cycle's seq alongside the state byte; cross-thread freers'
  // last-freer CAS expects (pre_seq, kRetireIdle) → (pre_seq,
  // kRetireDone), so a stale freer whose pre-snapshot seq belongs to
  // an earlier cycle observes a seq mismatch and the CAS cleanly
  // fails. This closes the residual sliver of the phantom-freer race
  // in which a transition (adoption + re-abandon) interleaves between
  // the freer's post-snapshot and the retire CAS — the new cycle's
  // retire_initiated has a fresh seq, so the stale CAS fails even
  // though both old and new cycles publish kRetireIdle. 24-bit seq
  // wrap is at 16M cycles per slab, infeasible inside a freer's
  // microsecond-scale window.
  cpp::Atomic<uint16_t> sealed_target;
  cpp::Atomic<uint16_t> xthread_returned_count;
  cpp::Atomic<uint32_t> retire_initiated;

  // Three-state ladder for `retire_initiated`'s low byte. Centralised
  // so the post-seal protocol and adoption flow share one definition.
  static constexpr uint8_t kRetireIdle = 0;
  static constexpr uint8_t kRetireDone = 1;
  static constexpr uint8_t kRetireAdopting = 2;

  // Pack a (cycle seq, state byte) pair into the retire_initiated word.
  // The seq is masked to 24 bits — wrap at 16M cycles per slab is
  // unreachable inside any freer's microsecond-scale window, and the
  // narrower seq leaves the low 8 bits for the kRetire* state byte.
  LIBC_INLINE static constexpr uint32_t pack_retire(uint32_t seq,
                                                     uint8_t state_byte) {
    return ((seq & 0xFFFFFFu) << 8) | static_cast<uint32_t>(state_byte);
  }
  LIBC_INLINE static constexpr uint8_t unpack_retire_state(uint32_t v) {
    return static_cast<uint8_t>(v & 0xFFu);
  }
  // Read the current cycle seq from `state_`. RELAXED — single-writer
  // contexts use this just before bumping seq via release()/claim() to
  // compute the post-transition seq for the matching retire_initiated
  // stamp. Cross-thread freers extract pre_seq from their pre-snapshot
  // directly, not via this accessor.
  LIBC_INLINE uint32_t state_seq() {
    return static_cast<uint32_t>(
        state_.load(cpp::MemoryOrder::RELAXED) >> 32);
  }

  // -- Cache line 3: per-page occupancy + state (owner-only) --
  //
  // page_occupancy: live (allocated) slots per slot-region page.
  // page_state: 2 bits per page packed in an array of uint64_t (32 pages
  //             per word).
  //   0 = COMMITTED  — normal, accessible
  //   1 = SEALED     — PAGE_NOACCESS, data preserved (freelist nodes intact)
  //   2 = DECOMMITTED — PAGE_NOACCESS, physical returned, data destroyed
  //
  // Lifecycle: COMMITTED → SEALED (on last free) → DECOMMITTED (on compact)
  //            DECOMMITTED → COMMITTED (on recommit for reuse)
  //            SEALED → COMMITTED (on unseal for alloc from freelist)
  //
  // For the default SlabBytesV (64 KB), kStateWords is 1 — the word is a
  // drop-in replacement for the old `uint32_t page_state` (compiler sees a
  // constant index into a 1-element array and emits a scalar load/store).
  alignas(64) uint16_t page_occupancy[kSlotPages];
  uint64_t page_state[kStateWords];
  // Substrate ownership receipt for this slab's VA. Relocated from
  // cache line 2 to make room for the packed `state_` atomic; sits
  // here in the slack between page_state[] and the alignas(64)
  // occupancy bitmap. Cold — read only at full_release / destroy.
  // Captured from the `SubSlotHandle` returned by
  // `substrate_acquire_uncommitted` at slab creation; passed back to
  // `substrate_release` (paired with the slab base pointer via
  // `SubSlotHandle::from_detached`) when the slab is released. Read-
  // only after init.
  uint64_t substrate_token;

  // -- Cache lines 4+: per-slot occupancy bitmap --
  //
  // One bit per slot. Set on alloc, cleared on free (owner path) or
  // drain_xthread (cross-thread path). Enables O(1)-per-64-slots
  // iteration via tzcnt/blsr without walking the freelist.
  //
  // Atomic<uint64_t> for formal C++17 race-freedom: validate_slot reads
  // the bitmap on the xthread free path while the owner may be writing.
  // All accesses use RELAXED ordering — compiles to plain MOV on x86_64
  // and AArch64, zero overhead vs non-atomic. The bitmap is not used
  // for cross-thread synchronization; it only needs tear-freedom.
  //
  // Max slots = kUsableBytes / sizeof(SlabFreeNode) (minimum slot size).
  // Derived at compile time: currently 53248 / 8 = 6656 slots →
  // ceil(6656/64) = 104 words = 832 bytes.
  // Total header footprint: 192 + 832 = 1024 bytes, within the 4KB
  // header page.
  //
  // trap_on_collision=false: SlabPool's owner-tid invariant
  // (LIBC_ASSERT(slab->tid() == current_tid()) at each write) is the
  // double-alloc / double-free guard. A prev-bit inspection here would
  // be redundant and is opted out so codegen matches the prior hand-
  // rolled fetch_or / fetch_and (pure RMW, no branch). The xthread
  // free path clears bits under the owner during drain_xthread, also
  // owner-asserted; cross-thread frees don't touch the bitmap.
  static constexpr unsigned kMaxSlots = Layout::kMaxSlots;
  static constexpr unsigned kMaxBitmapWords =
      alloc_primitives::occupancy_words(kMaxSlots);
  alignas(64)
      alloc_primitives::AtomicBitmap<kMaxSlots, /*trap_on_collision=*/false>
          occupancy;

  // -- Accessors --
  //
  // The packed `state_` word holds (seal_seq << 32) | tid. tid() and
  // state() ACQUIRE-load so a freer that observes 0 in the low half
  // also observes the owner's pre-transition RELEASE-store of
  // sealed_target / count reset. claim() and release() bump the high
  // half and write the new low half in a single RELEASE store: cross-
  // thread freers that read state_ after the RELEASE see (new_seq,
  // new_tid) atomically, so the pre/post snapshot check in the xthread
  // free path catches every transition — including alive→sealed during
  // a freer's push window — and rejects phantom fetch_adds.
  //
  // claim/release are single-writer per transition (the slab's
  // ownership protocol serialises owner / adopter / fork-reinit /
  // adopter-re-abandon), so the load+store on `state_` here is race-
  // free without a CAS.
  //
  // Non-const because cpp::Atomic<T>::load is non-const (matches
  // substrate's ArenaHeader::live_count.load pattern).
  LIBC_INLINE uint64_t state() {
    return state_.load(cpp::MemoryOrder::ACQUIRE);
  }
  LIBC_INLINE uint32_t tid() {
    return static_cast<uint32_t>(state_.load(cpp::MemoryOrder::ACQUIRE));
  }
  LIBC_INLINE void claim(uint32_t t) {
    uint64_t cur = state_.load(cpp::MemoryOrder::RELAXED);
    uint64_t new_state = (((cur >> 32) + 1ULL) << 32) | static_cast<uint64_t>(t);
    state_.store(new_state, cpp::MemoryOrder::RELEASE);
  }
  LIBC_INLINE void release() {
    uint64_t cur = state_.load(cpp::MemoryOrder::RELAXED);
    uint64_t new_state = ((cur >> 32) + 1ULL) << 32;
    state_.store(new_state, cpp::MemoryOrder::RELEASE);
  }
};

// Default-arena alias. Every reference to `SlabHeader` outside the
// templated definitions resolves here — the 64 KB / 8 B-min layout,
// byte-for-byte identical to the pre-template header. Per-instantiation
// layouts are expressed as `SlabHeaderT<N, M>` directly.
using SlabHeader = SlabHeaderT<>;

// Layout invariants applied to the default instantiation. Per-instantiation
// variants are validated inside the template body via class-scope constants
// (kSlotPages ≤ 16 assertion in SlabLayout; bitmap sizing from kUsableBytes).
//
// Cache line 0 is fully occupied by the inherited CrystallineNode (24 B)
// plus a 40 B pad. The pad's size is the ONLY thing that depends on
// CrystallineNode's exact width — if CrystallineNode changes size, the
// pad calculation must be revisited. The sizeof assert below is the
// load-bearing check for that invariant.
//
// Subsequent cache-line boundaries (offset 128 for xthread_stub, 192 for
// page_occupancy, 256 for occupancy) are enforced by the `alignas(64)`
// decorations on those members. alignas is an ABI guarantee from the
// language, not an advisory hint, so we don't need offsetof asserts to
// re-verify it. (Using offsetof on SlabHeader is also formally
// conditionally-supported in C++17 because the inheritance makes it
// non-standard-layout; the alignas-based contract sidesteps that issue
// entirely.)
// The post-refactor `CrystallineNode` is an empty tag base; the
// intrusive runtime fields it used to carry now live directly in
// SlabHeaderT via `LIBC_CRYSTALLINE_NODE_FIELDS`. The layout the
// 40-byte `_pad_post_crystalline` was sized against is pinned on the
// macro's stable 24-byte CrystallineNodeLayoutRef instead.
static_assert(sizeof(::LIBC_NAMESPACE::concurrent::CrystallineNodeLayoutRef) ==
                  24,
              "CrystallineNode field layout assumption — the 40-byte "
              "_pad_post_crystalline in SlabHeaderT depends on the macro "
              "emitting exactly 24 bytes of header. If this fires the macro "
              "has changed and the pad must be re-derived.");
static_assert(sizeof(SlabHeader) <= 4096,
              "SlabHeader must fit in the 4KB header page");
// SlabHeaders live in page_commit'd memory with no C++ destructor path.
// A non-trivial destructor would silently leak or corrupt on slab release.
static_assert(cpp::is_trivially_destructible_v<SlabHeader>,
              "SlabHeader must be trivially destructible");

// ===----------------------------------------------------------------------===//
// Slab Registry — three-level radix bitmap of active slab bases.
//
// Shared by all SlabPool instances. Enables O(1) slab identification
// for free-path dispatch (slab vs large) without trusting magic values.
//
// Keys are 64KB-aligned slab base addresses. The key is (addr >> 16),
// giving up to 41 significant bits for 57-bit VA or 32 bits for 48-bit.
// Split into three levels:
//
//   L1: runtime-sized root (bits [N:26])  — sized from PCB max_address,
//       eagerly committed. For 48-bit VA: 64 entries = 512 B (rounds
//       up into a 16 KB Small substrate slot — slack is fine, L1 is
//       a singleton). For 57-bit VA: 32 K entries = 256 KB exactly,
//       served from a 256 KB XLarge substrate slot. Covers the full
//       user VA range.
//   L2: 1024 pointers to L3 bitmaps (bits [25:16]) — 8 KB each,
//       demand-allocated per 4 TB region from a 16 KB Small slot
//       (8 KB slack holds the slot's substrate token).
//   L3: 65536-bit bitmap (bits [15:0] → word[15:6] + bit[5:0]) — 8 KB
//       each, demand-allocated per 4 GB region from a 16 KB Small slot.
//
// Physical memory is proportional to populated VA regions only:
//   - L2: one 16 KB substrate slot per 4 TB region with slab activity
//   - L3: one 16 KB substrate slot per 4 GB region with slab activity
//
// All directory pages (L1/L2/L3) are substrate-served; their VA is
// stamped LIBC_INTERNAL in the mapping table at arena grain via the
// substrate's own arena registration. No raw page_alloc / page_reserve
// from this struct.
//
// Future-proof: L1 sizing derives from PCB max_address (populated at
// startup from NtQuerySystemInformation). If Windows extends user VA
// to 57 bits, L1 picks XLarge automatically — no code changes needed.
//
// Lock-free: insert/remove use atomic bit ops. L2/L3 allocation uses
// CAS on the parent slot (one-time cost per region). contains() is
// three loads on the hot path.
//
// L2/L3 pages are retained for process lifetime once allocated.
// Reclaiming them mid-life would require serializing against concurrent
// insert() to prevent atomic ops on a decommitted page. Each L3 covers
// a 4 GB region and slab activity clusters in few regions, so the
// retained cost is negligible. All pages are released back to the
// substrate at fini via destroy() — see below.
// ===----------------------------------------------------------------------===//

struct SlabRegistry {
  // Every SlabPoolT<N> instantiation reads slab_secret_ to compute and
  // verify slab authenticity tags. Template-friend clause grants access
  // to all instantiations without enumerating them.
  template <size_t, size_t> friend class SlabPoolT;

  using Bitmap = cpp::Atomic<uint64_t>;

  // L3 bitmap: 65536 bits = 1024 uint64_t words = 8KB.
  // One L3 covers a 4GB VA region (65536 slabs × 64KB).
  // Bottom 16 key bits address into the bitmap: [15:6]=word, [5:0]=bit.
  static constexpr unsigned kL3Words = 1024;
  static constexpr size_t kL3Bytes = kL3Words * sizeof(Bitmap);
  static constexpr unsigned kL3Bits = 16; // 65536 bitmap positions.

  // L2 directory: 1024 atomic pointers to L3 bitmaps = 8KB.
  // One L2 covers a 4TB VA region (1024 L3 × 4GB).
  using L2Entry = cpp::Atomic<Bitmap *>;
  static constexpr unsigned kL2Slots = 1024;
  static constexpr size_t kL2Bytes = kL2Slots * sizeof(L2Entry);
  static constexpr unsigned kL2Bits = 10; // 1024 L2 slots.

  // L1 directory: runtime-sized array of atomic pointers to L2 pages.
  // Size determined at init from PCB max_address. For 48-bit VA:
  //   max key = (0x7FFFFFFEFFFF >> 16) = 0x7FFFFFFE (32 bits)
  //   L1 index = key >> 26 = 0x1FF = 511, so L1 = 512 entries = 4KB.
  // For 57-bit VA: L1 index = key >> 26 → max ~16K entries = 128KB.
  using L1Entry = cpp::Atomic<L2Entry *>;

  static_assert(sizeof(L1Entry) == sizeof(void *),
                "Atomic<L2Entry*> must be pointer-sized for L1 layout");
  static_assert(sizeof(L2Entry) == sizeof(void *),
                "Atomic<Bitmap*> must be pointer-sized for L2 layout");

  static constexpr unsigned kL2Mask = (1U << kL2Bits) - 1; // 0x3FF
  static constexpr unsigned kL3Mask = (1U << kL3Bits) - 1; // 0xFFFF

  mutable cpp::Atomic<L1Entry *> l1_{nullptr};
  // One-shot init latch. Infallible-by-construction: every failure inside
  // ensure_init() traps immediately (substrate_acquire returns an empty
  // handle → trap; init_seed_or_trap traps internally). There is no retry
  // edge.
  alloc_primitives::InitLatch init_latch_;
  // Power-of-2 mask for L1 index clamping. Set once at init.
  // Wild addresses produce clamped indices that land on nullptr L2 slots,
  // caught by the existing null checks — no branch needed for bounds safety.
  // Transitively published by init_latch_.publish_ready()'s RELEASE store.
  size_t l1_mask_{0};
  // Substrate ownership receipt for the L1 directory page. Captured from
  // the SubSlotHandle returned by substrate_acquire; passed back to
  // substrate_release in destroy(). L1 is a singleton, so we only need
  // one token. L2 and L3 page tokens live at offset kL2Bytes / kL3Bytes
  // within their backing Small slot (16 KB Small slot, 8 KB directory
  // payload; the remaining 8 KB has room for an 8 B token plus padding).
  uint64_t l1_handle_token_{0};
  // Number of valid entries actually addressable in `l1_`. Set at init.
  // Used by destroy() to walk the L2 fanout without re-deriving from the
  // VA mask.
  size_t l1_entries_{0};

private:
  // Process-wide secret for slab authenticity tags. Set once during
  // registry init (ProcessPrng), transitively published by
  // init_latch_.publish_ready(). SlabPool reads this via friend access
  // to compute slab_tag = secret ^ slab_base at slab creation and to
  // verify it on every free(). Not publicly accessible — leaking the
  // secret defeats the authenticity scheme.
  uintptr_t slab_secret_{0};

public:
  // Decompose a 64KB-aligned address into L1/L2/L3 indices.
  struct Key {
    size_t l1;     // L1 slot index (bits [N:26] of key).
    unsigned l2;   // L2 slot index (bits [25:16] of key).
    unsigned word; // L3 word index (bits [15:6] of key).
    unsigned bit;  // Bit within the L3 word (bits [5:0] of key).
  };

  // Decompose with L1 index clamped by mask. One AND instruction,
  // no branch — wild addresses land on nullptr L2 slots.
  LIBC_INLINE Key decompose(uintptr_t base) const {
    uintptr_t k = base >> 16; // Full key, no truncation.
    return {
        static_cast<size_t>(k >> (kL2Bits + kL3Bits)) & l1_mask_,
        static_cast<unsigned>((k >> kL3Bits) & kL2Mask),
        static_cast<unsigned>((k & kL3Mask) >> 6),
        static_cast<unsigned>(k & 63)};
  }

  // Compute required L1 size from the maximum user-mode address.
  // The PCB max_address is populated at startup from
  // NtQuerySystemInformation(SystemBasicInformation).
  LIBC_INLINE static size_t required_l1_size() {
    uintptr_t max_addr =
        reinterpret_cast<uintptr_t>(pcb_max_address());
    uintptr_t max_key = max_addr >> 16;
    size_t max_l1_idx = static_cast<size_t>(max_key >> (kL2Bits + kL3Bits));
    return max_l1_idx + 1;
  }

  // Pick the smallest substrate slot class that fits a directory page of
  // `bytes` bytes. The L1 directory ranges from 512 B (48-bit VA) to
  // 256 KB (57-bit VA); L2 and L3 directories are always 8 KB.
  LIBC_INLINE static ::LIBC_NAMESPACE::windows::alloc::SubSlotClass
  pick_class_for_directory(size_t bytes) {
    using ::LIBC_NAMESPACE::windows::alloc::SubSlotClass;
    return (bytes <= (size_t{16} << 10))    ? SubSlotClass::Small
           : (bytes <= (size_t{64} << 10))  ? SubSlotClass::Medium
           : (bytes <= (size_t{128} << 10)) ? SubSlotClass::Large
           : (bytes <= (size_t{256} << 10)) ? SubSlotClass::XLarge
                                            : SubSlotClass::Huge;
  }

  // Token-stash offset for L2/L3 directory pages. The L1 token lives
  // in `l1_handle_token_` (see field comment); for L2 and L3 we stash
  // the token at the very end of the directory payload inside the same
  // Small slot. Reads/writes are unsynchronized: the publishing CAS
  // ordering on the parent slot RELEASE-publishes every prior write
  // (token included), and only single-threaded fini reads the token
  // back. The 16 KB Small slot easily accommodates the 8 KB directory
  // payload plus the 8 B token plus padding.
  static constexpr size_t kL2TokenOffset = kL2Bytes;
  static constexpr size_t kL3TokenOffset = kL3Bytes;
  static_assert(kL2TokenOffset + sizeof(uint64_t) <= (size_t{16} << 10),
                "L2 token must fit inside the Small backing slot");
  static_assert(kL3TokenOffset + sizeof(uint64_t) <= (size_t{16} << 10),
                "L3 token must fit inside the Small backing slot");

  LIBC_INLINE static void stash_token(void *page, size_t offset,
                                       uint64_t token) {
    *reinterpret_cast<uint64_t *>(reinterpret_cast<char *>(page) + offset) =
        token;
  }
  LIBC_INLINE static uint64_t fetch_token(void *page, size_t offset) {
    return *reinterpret_cast<uint64_t *>(reinterpret_cast<char *>(page) +
                                          offset);
  }

  LIBC_INLINE void ensure_init() {
    // Fast path: already READY. One ACQUIRE load + branch.
    if (LIBC_LIKELY(init_latch_.is_ready()))
      return;
    if (!init_latch_.try_begin()) {
      // Another thread is initializing; wait for READY.
      init_latch_.wait_ready();
      return;
    }
    // Winner path. Every failure below traps — there is no retry edge.
    //
    // Runtime-sized L1: derive entry count from PCB max_address, rounded
    // up to power-of-2 for branchless mask clamping. For 48-bit VA
    // (current Windows): 64 entries = 512 B (rounds up to a Small slot,
    // which is 16 KB — wasted slack is fine; L1 is a singleton). For
    // 57-bit VA: 32 K entries = 256 KB exactly (XLarge fit). Substrate's
    // committed acquire zero-fills the slot, so every L2 entry starts
    // null. Extra entries from rounding land wild indices on them and
    // fail gracefully via the L2 null checks.
    size_t l1_count = required_l1_size();
    size_t l1_rounded = l1_count;
    if (l1_rounded & (l1_rounded - 1))
      l1_rounded = static_cast<size_t>(1) << (64 - __builtin_clzll(l1_rounded));
    size_t l1_bytes = l1_rounded * sizeof(L1Entry);
    auto klass = pick_class_for_directory(l1_bytes);
    auto handle = ::LIBC_NAMESPACE::windows::alloc::substrate_acquire(
        klass, ::LIBC_NAMESPACE::windows::alloc::ConsumerTag::SlabRegistry);
    if (!handle)
      __builtin_trap();
    l1_handle_token_ = handle.token();
    auto *mem = handle.detach_ptr();
    l1_.store(static_cast<L1Entry *>(mem), cpp::MemoryOrder::RELAXED);
    l1_mask_ = l1_rounded - 1;
    l1_entries_ = l1_rounded;
    // Slab authenticity secret — seeded from OS entropy once.
    // init_seed_or_trap is fail-closed (ProcessPrng failure or zero draw
    // traps). A zero secret would make every slab_tag trivially forgeable
    // (tag = 0 ^ base = base), defeating authenticity checks.
    alloc_primitives::SingleCanarySeed secret;
    alloc_primitives::init_seed_or_trap(secret);
    slab_secret_ = secret.seed;
    // l1_, l1_mask_, slab_secret_ are transitively published by
    // publish_ready()'s RELEASE store.
    init_latch_.publish_ready();
  }

  // Allocate an L2 page for a 4TB region. CAS into L1[idx].
  // Returns the winning pointer (ours or the racer's).
  [[nodiscard]] LIBC_INLINE L2Entry *ensure_l2(size_t l1_idx) {
    L1Entry *l1 = l1_.load(cpp::MemoryOrder::RELAXED);
    L2Entry *l2 = l1[l1_idx].load(cpp::MemoryOrder::ACQUIRE);
    if (l2)
      return l2;

    auto handle = ::LIBC_NAMESPACE::windows::alloc::substrate_acquire(
        ::LIBC_NAMESPACE::windows::alloc::SubSlotClass::Small,
        ::LIBC_NAMESPACE::windows::alloc::ConsumerTag::SlabRegistry);
    if (!handle)
      return nullptr;
    auto *new_l2 = static_cast<L2Entry *>(handle.ptr());
    // Stash the substrate token in the slot's slack so destroy() can
    // release the page without parallel bookkeeping. The token write
    // happens-before the CAS-RELEASE on l1[idx]; threads that ACQUIRE
    // the published pointer also see the token.
    stash_token(new_l2, kL2TokenOffset, handle.token());
    (void)handle.detach_ptr();

    L2Entry *expected = nullptr;
    if (l1[l1_idx].compare_exchange_strong(expected, new_l2,
                                           cpp::MemoryOrder::ACQ_REL,
                                           cpp::MemoryOrder::ACQUIRE))
      return new_l2;

    // Lost the race — return ours to the substrate, use the winner's.
    ::LIBC_NAMESPACE::windows::alloc::substrate_release(
        ::LIBC_NAMESPACE::windows::alloc::SubSlotHandle::from_detached(
            new_l2, fetch_token(new_l2, kL2TokenOffset)));
    return expected;
  }

  // Allocate an L3 bitmap for a 4GB region. CAS into L2[idx].
  // Returns the winning pointer (ours or the racer's).
  [[nodiscard]] LIBC_INLINE Bitmap *ensure_l3(L2Entry *l2, unsigned l2_idx) {
    Bitmap *l3 = l2[l2_idx].load(cpp::MemoryOrder::ACQUIRE);
    if (l3)
      return l3;

    auto handle = ::LIBC_NAMESPACE::windows::alloc::substrate_acquire(
        ::LIBC_NAMESPACE::windows::alloc::SubSlotClass::Small,
        ::LIBC_NAMESPACE::windows::alloc::ConsumerTag::SlabRegistry);
    if (!handle)
      return nullptr;
    auto *new_l3 = static_cast<Bitmap *>(handle.ptr());
    stash_token(new_l3, kL3TokenOffset, handle.token());
    (void)handle.detach_ptr();

    Bitmap *expected = nullptr;
    if (l2[l2_idx].compare_exchange_strong(expected, new_l3,
                                           cpp::MemoryOrder::ACQ_REL,
                                           cpp::MemoryOrder::ACQUIRE))
      return new_l3;

    ::LIBC_NAMESPACE::windows::alloc::substrate_release(
        ::LIBC_NAMESPACE::windows::alloc::SubSlotHandle::from_detached(
            new_l3, fetch_token(new_l3, kL3TokenOffset)));
    return expected;
  }

  LIBC_INLINE void insert(uintptr_t base) {
    ensure_init();
    Key k = decompose(base);
    L2Entry *l2 = ensure_l2(k.l1);
    if (!l2)
      __builtin_trap();
    Bitmap *l3 = ensure_l3(l2, k.l2);
    if (!l3)
      __builtin_trap();
    l3[k.word].fetch_or(1ULL << k.bit, cpp::MemoryOrder::RELEASE);
  }

  // Multi-granule insert: stamp every 64 KB cell in [base, base + size).
  //
  // SlabPoolT<N> instantiations with N > 65536 back a single slab with
  // multiple consecutive 64 KB cells. `contains()` must answer true for
  // any address in the slab, so every cell gets its bit set here. Typical
  // case (1 MB slab aligned) stays inside one L3 word and collapses to a
  // single fetch_or with a 16-bit mask.
  //
  // Called once per arena growth (rare), not on the allocation hot path.
  // The per-cell loop is fine; the inner-word coalescing is pure icing.
  LIBC_INLINE void insert_range(uintptr_t base, size_t size) {
    LIBC_ASSERT((base & 0xFFFF) == 0 && "base must be 64 KB aligned");
    LIBC_ASSERT((size & 0xFFFF) == 0 && size > 0 &&
                "size must be a positive multiple of 64 KB");
    ensure_init();
    uintptr_t end = base + size;
    uintptr_t cursor = base;
    while (cursor < end) {
      Key k = decompose(cursor);
      L2Entry *l2 = ensure_l2(k.l1);
      if (!l2)
        __builtin_trap();
      Bitmap *l3 = ensure_l3(l2, k.l2);
      if (!l3)
        __builtin_trap();
      // Coalesce bits that fall in the same L3 word (64 cells = 4 MB span).
      // Build a mask for the run of cells [k.bit..63] that still lie in
      // the range, then stamp them in one atomic op.
      unsigned start_bit = k.bit;
      uintptr_t word_span_cells = 64 - start_bit;
      uintptr_t word_end = cursor + word_span_cells * 65536;
      if (word_end > end)
        word_end = end;
      uintptr_t cells = (word_end - cursor) / 65536;
      uint64_t mask = (cells == 64)
                          ? ~uint64_t{0}
                          : (((uint64_t{1} << cells) - 1) << start_bit);
      l3[k.word].fetch_or(mask, cpp::MemoryOrder::RELEASE);
      cursor = word_end;
    }
  }

  // Three-load hot path: L1 → L2 → L3 bitmap word. L1 is fully committed,
  // so every slot is safely readable without a secondary bitmap check.
  LIBC_INLINE bool contains(uintptr_t base) const {
    if (!init_latch_.is_ready())
      return false;
    Key k = decompose(base);
    L1Entry *l1 = l1_.load(cpp::MemoryOrder::RELAXED);
    L2Entry *l2 = l1[k.l1].load(cpp::MemoryOrder::ACQUIRE);
    if (!l2)
      return false;
    Bitmap *l3 = l2[k.l2].load(cpp::MemoryOrder::ACQUIRE);
    if (!l3)
      return false;
    return (l3[k.word].load(cpp::MemoryOrder::ACQUIRE) >> k.bit) & 1;
  }

  LIBC_INLINE void remove(uintptr_t base) {
    if (!init_latch_.is_ready())
      return;
    Key k = decompose(base);
    L1Entry *l1 = l1_.load(cpp::MemoryOrder::RELAXED);
    L2Entry *l2 = l1[k.l1].load(cpp::MemoryOrder::ACQUIRE);
    if (!l2)
      return;
    Bitmap *l3 = l2[k.l2].load(cpp::MemoryOrder::ACQUIRE);
    if (!l3)
      return;
    l3[k.word].fetch_and(~(1ULL << k.bit), cpp::MemoryOrder::RELEASE);

    // L2/L3 pages are not reclaimed when a region empties.
    // Reclamation would require serializing against concurrent insert()
    // to prevent atomic ops on decommitted memory (PAGE_NOACCESS fault).
    // Since each L3 covers a 4GB region and slab allocations cluster
    // in few regions, the retained cost is negligible in practice.
  }

  // Symmetric counterpart to insert_range. Silently skips ranges whose
  // L2 or L3 pages never got allocated — a remove that didn't match an
  // insert is a no-op, not an error, preserving remove()'s contract.
  LIBC_INLINE void remove_range(uintptr_t base, size_t size) {
    LIBC_ASSERT((base & 0xFFFF) == 0 && "base must be 64 KB aligned");
    LIBC_ASSERT((size & 0xFFFF) == 0 && size > 0 &&
                "size must be a positive multiple of 64 KB");
    if (!init_latch_.is_ready())
      return;
    uintptr_t end = base + size;
    uintptr_t cursor = base;
    L1Entry *l1 = l1_.load(cpp::MemoryOrder::RELAXED);
    while (cursor < end) {
      Key k = decompose(cursor);
      L2Entry *l2 = l1[k.l1].load(cpp::MemoryOrder::ACQUIRE);
      if (!l2) {
        // Skip the rest of this L2 region (4 TB) — nothing to clear.
        cursor = (cursor & ~((uintptr_t{1} << 42) - 1)) + (uintptr_t{1} << 42);
        continue;
      }
      Bitmap *l3 = l2[k.l2].load(cpp::MemoryOrder::ACQUIRE);
      if (!l3) {
        // Skip the rest of this L3 region (4 GB).
        cursor = (cursor & ~((uintptr_t{1} << 32) - 1)) + (uintptr_t{1} << 32);
        continue;
      }
      unsigned start_bit = k.bit;
      uintptr_t word_span_cells = 64 - start_bit;
      uintptr_t word_end = cursor + word_span_cells * 65536;
      if (word_end > end)
        word_end = end;
      uintptr_t cells = (word_end - cursor) / 65536;
      uint64_t mask = (cells == 64)
                          ? ~uint64_t{0}
                          : (((uint64_t{1} << cells) - 1) << start_bit);
      l3[k.word].fetch_and(~mask, cpp::MemoryOrder::RELEASE);
      cursor = word_end;
    }
  }

  // Process-fini sweep. Walks the directory tree and returns every
  // backing page (L3 → L2 → L1) to the substrate. Single-threaded by
  // contract: registered as a $P4 fini hook so it runs after all slab
  // consumers have torn down (SlabPool's $P4 fini precedes us in
  // bucket-order; substrate's $P1 fini is reverse-ordered to run last,
  // so substrate is still live for these substrate_release calls).
  //
  // L3 pages are released first so `contains()` (which the substrate's
  // own internal release path may consult) sees a consistent view if it
  // races. L2 then L1. After this returns the registry is non-functional;
  // any subsequent insert/contains/remove is a use-after-fini bug.
  LIBC_INLINE void destroy() {
    if (!init_latch_.is_ready())
      return;
    L1Entry *l1 = l1_.load(cpp::MemoryOrder::RELAXED);
    if (!l1)
      return;
    for (size_t i = 0; i < l1_entries_; ++i) {
      L2Entry *l2 = l1[i].load(cpp::MemoryOrder::RELAXED);
      if (!l2)
        continue;
      for (unsigned j = 0; j < kL2Slots; ++j) {
        Bitmap *l3 = l2[j].load(cpp::MemoryOrder::RELAXED);
        if (!l3)
          continue;
        ::LIBC_NAMESPACE::windows::alloc::substrate_release(
            ::LIBC_NAMESPACE::windows::alloc::SubSlotHandle::from_detached(
                l3, fetch_token(l3, kL3TokenOffset)));
      }
      ::LIBC_NAMESPACE::windows::alloc::substrate_release(
          ::LIBC_NAMESPACE::windows::alloc::SubSlotHandle::from_detached(
              l2, fetch_token(l2, kL2TokenOffset)));
    }
    ::LIBC_NAMESPACE::windows::alloc::substrate_release(
        ::LIBC_NAMESPACE::windows::alloc::SubSlotHandle::from_detached(
            l1, l1_handle_token_));
    l1_.store(nullptr, cpp::MemoryOrder::RELAXED);
    l1_handle_token_ = 0;
  }
};

// Global registry — defined in slab_registry.cpp.
extern SlabRegistry slab_registry;

// ===----------------------------------------------------------------------===//
// SlabPool — runtime-configurable slab allocator
//
// Templated on SlabBytesV (default 65536 = one NT allocation granule) so
// each pool instance carries a compile-time-correct slab layout:
//   - ptr_to_slab() mask: ~(kSlabBytes - 1)
//   - xthread tagged-pointer offset mask: kSlabBytes - 1
//   - page_occupancy[] / page_state / occupancy bitmap sizes in the header
//
// `using SlabPool = SlabPoolT<>` below preserves every existing call site
// byte-for-byte. Larger arenas (e.g. 1 MB multi-granule slabs backing the
// mapping table's L3 pages) instantiate SlabPoolT<N> directly.
// ===----------------------------------------------------------------------===//

template <size_t SlabBytesV, size_t MinSlotBytesV>
class alignas(64) SlabPoolT {
public:
  using Layout = SlabLayout<SlabBytesV, MinSlotBytesV>;
  using SlabHeader = SlabHeaderT<SlabBytesV, MinSlotBytesV>;
  using ThreadSlab = SlabHeader *;

  // Class-scope layout constants shadow the file-scope default-arena
  // values so every internal reference to kSlabBytes/kUsableBytes/etc.
  // automatically resolves to the per-instantiation value — no need to
  // rewrite the 100+ existing call sites inside the class body.
  static constexpr size_t kSlabBytes = Layout::kSlabBytes;
  static constexpr size_t kPageSize = Layout::kPageSize;
  static constexpr size_t kLeadingGuardOffset = Layout::kLeadingGuardOffset;
  static constexpr size_t kSlotStartOffset = Layout::kSlotStartOffset;
  static constexpr size_t kTrailingGuardOffset = Layout::kTrailingGuardOffset;
  static constexpr size_t kUsableBytes = Layout::kUsableBytes;
  static constexpr size_t kBodyOffset = Layout::kBodyOffset;
  static constexpr size_t kBodySize = Layout::kBodySize;
  static constexpr size_t kCanaryOffset = Layout::kCanaryOffset;
  static constexpr unsigned kSlotPages = Layout::kSlotPages;
  static constexpr unsigned kStateWords = Layout::kStateWords;
  static constexpr int kXthreadTagShift = Layout::kXthreadTagShift;
  static constexpr uintptr_t kXthreadOffsetMask = Layout::kXthreadOffsetMask;

  // Substrate slot class that backs each slab body. Compile-time
  // dispatch on the per-pool arena size: 64 KB slabs land in Medium
  // (64 KB substrate slots, 32 per 2 MB arena), 1 MB L3-pool slabs
  // land in Huge (1 MB substrate slots, 7 per 8 MB arena). Both classes
  // were sized in Phase 0.1 to match these slab sizes exactly.
  static_assert(kSlabBytes == 65536 || kSlabBytes == 1048576,
                "SlabPoolT only supports 64 KB or 1 MB slabs; add a "
                "matching SubSlotClass before instantiating elsewhere.");
  static constexpr ::LIBC_NAMESPACE::windows::alloc::SubSlotClass
      kSubstrateClass =
          (kSlabBytes == 65536)
              ? ::LIBC_NAMESPACE::windows::alloc::SubSlotClass::Medium
              : ::LIBC_NAMESPACE::windows::alloc::SubSlotClass::Huge;

  // Local bitset type for drain/compact sealed/touched/unsealed masks.
  // For default SlabBytesV (kSlotPages = 13) this is a single uint64_t and
  // compiles to the same tzcnt/blsr instructions the pre-template uint32_t
  // masks used; for larger arenas it transparently upgrades to a word array.
  using PageMask = PageBitset<kSlotPages>;

  // Pack a node pointer into an xthread word (intra-slab offset).
  // node may be nullptr (encodes as offset 0). No generation field — see
  // the comment block on SlabFreeNode for why ABA is structurally absent
  // (XCHG-push + XCHG-drain, no CAS anywhere).
  LIBC_INLINE static uintptr_t xthread_pack([[maybe_unused]] void *slab,
                                             void *node) {
    uintptr_t offset = node ? (reinterpret_cast<uintptr_t>(node) &
                                kXthreadOffsetMask) : 0;
    // Validate node belongs to this slab: high bits must match slab base.
    LIBC_ASSERT(!node ||
                (reinterpret_cast<uintptr_t>(node) & ~kXthreadOffsetMask) ==
                    reinterpret_cast<uintptr_t>(slab));
    return offset;
  }

  // Extract the node pointer from an xthread word.
  // Returns nullptr if the offset is 0.
  LIBC_INLINE static SlabFreeNode *xthread_unpack_node(void *slab,
                                                       uintptr_t packed) {
    uintptr_t offset = packed & kXthreadOffsetMask;
    if (!offset)
      return nullptr;
    return reinterpret_cast<SlabFreeNode *>(
        reinterpret_cast<uintptr_t>(slab) | offset);
  }

private:
  // Crystalline-W FreeFn for slab retirement. Called by the slab retire
  // domain once every reservation against the slab has drained. Performs
  // the substrate slot release that today's `full_release` does — but
  // gated on Crystalline confirming no in-flight cross-thread freer or
  // adoption-walker is still touching the slab.
  //
  // Static so it can be a template argument to CrystallineDomain<>; reads
  // the pool backpointer the slab carries (set in init_slab) to dispatch
  // back to the owning instance. The slab pointer is always non-null when
  // Crystalline fires the FreeFn (init_node was called on a real slab).
  LIBC_INLINE static void slab_release_callback(SlabHeader *slab) {
    if (LIBC_UNLIKELY(slab == nullptr))
      __builtin_trap();
    auto *pool = slab->pool;
    if (LIBC_UNLIKELY(pool == nullptr))
      __builtin_trap();
    pool->full_release(slab);
  }

  // -- Cache line 0: hot (CAS on every xthread free / alloc_slow) --

  // Lock-free stack of abandoned slabs (Treiber push + exchange-steal pop).
  //
  // Pop uses exchange(nullptr) which is ABA-free by construction — no
  // compare, no stale-read window. The stolen chain is walked locally
  // (single-threaded, zero data races). Push uses plain CAS which is
  // also ABA-immune (Treiber push produces a valid stack regardless of
  // interleaving). No tagged pointer needed on this stack.
  //
  // Walkers (alloc_slow's adoption loop, the cross-thread freer's
  // `pop_abandoned_specific`) hold a `slab_retire_domain_` reservation
  // for the duration of their walk so a concurrent last-freer's retire
  // cannot run `slab_release_callback` (and the substrate decommit
  // inside it) on a slab they are still dereferencing — Crystalline's
  // reservation drain is what closes the classical Treiber ABA window
  // here (a retired slab's VA cannot be recycled until walkers leave).
  cpp::Atomic<SlabHeader *> abandoned_head_{nullptr};

  // -- Cache line 1: cold (fork, slab lifecycle, init) --

  // All-slabs list (doubly-linked, for fork_reinit). Cold path only.
  alignas(64) SlabHeader *all_slabs_head_{nullptr};
  // Spinlock (not RawMutex — avoids futex/signal circular dep).
  cpp::Atomic<int> all_slabs_lock_{0};

  // Pool configuration (set once at init, guarded by init_latch_).
  // slot_size_ is uint32_t to fit the L3 pool's 65664 B slots.
  uint32_t slot_size_{0};
  uint16_t slots_per_slab_{0};
  uint8_t class_index_{0};
  // Infallible one-shot init latches. init() validates inputs (traps on
  // violation) then assigns slot_size_/slots_per_slab_/class_index_ under
  // try_begin(); init_tls() assigns tls_index_ likewise. Decomposed API
  // (try_begin + wait_ready + publish_ready) instead of ensure_init:
  // the init work never returns false, so the bounded-retry ladder would
  // never exercise a retry; the decomposed form also keeps trap-on-
  // failure inline with the work rather than wrapping it in a bool lambda.
  alloc_primitives::InitLatch init_latch_;
  alloc_primitives::InitLatch tls_init_latch_;

  // TLS slot for per-thread slab pointer. TLS_OUT_OF_INDEXES if not enabled.
  DWORD tls_index_{TLS_OUT_OF_INDEXES};

  // Crystalline-W domain protecting slab VA against in-flight readers
  // during retirement. Mirrors `g_substrate_domain` (va_substrate.cpp:309)
  // at slab grain.
  //
  // Freq=8: a slab retire publishes only after 8 retires have accumulated
  // in the calling thread's per-domain batch. Cross-thread freers'
  // protect() fast-path (era unchanged) hits ~8× more often than
  // with Freq=1, dropping per-call overhead from "2 atomic loads +
  // do_update slow path" down to "2 atomic loads + branch" in steady
  // state. Reclamation latency is bounded by 8 slabs queued per thread
  // before flush; thread_flush_trampoline publishes on thread exit, so
  // the residual queue is drained at the latest within one thread
  // lifecycle. The substrate's `g_substrate_domain` uses the same
  // Freq=8 — Crystalline-W's `try_retire` requires batch chains long
  // enough to outlast the eligible-slot scan in Phase A, so any Freq
  // ≤ active-thread-count would leak batches.
  //
  // STATIC across all SlabPoolT<...> instances of this template
  // instantiation. Per-instance domain isolation confers no structural
  // benefit: slab_release_callback already dispatches via slab->pool to
  // the owning instance's full_release. Sharing collapses the
  // size-class fan-out (40+ pools each registering their own domain
  // would otherwise overflow the per-thread inline domain slot table).
  //
  // First init() to win `slab_retire_domain_init_latch_.try_begin()`
  // registers the domain; later inits wait_ready and skip registration.
  // Tier A bring-up is still single-threaded by contract; the latch is
  // belt-and-suspenders against future cross-thread init paths.
  inline static ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
      SlabHeader, &slab_release_callback, /*Freq=*/8>
      slab_retire_domain_;
  inline static alloc_primitives::InitLatch slab_retire_domain_init_latch_;

  // Per-pool monotonic counter for Crystalline-W batch_link serial
  // stamping. Bumped once per slab in init_slab(). 32 bits = 4 billion
  // slab claims per pool; the serial table traps on overflow long
  // before that. STATIC across SlabPoolT<...> instances sharing the
  // same template instantiation, mirroring slab_retire_domain_'s
  // sharing rationale.
  inline static cpp::Atomic<uint32_t> crystalline_serial_counter_{0};
  // Per-pool serial → SlabHeader* lookup, used by
  // BatchLinkCodec<SlabHeader>::decode. Each instantiation of
  // SlabPoolT (Default 64KB, L3 1MB, ...) carries its own table.
  inline static ::LIBC_NAMESPACE::concurrent::CrystallineSerialTable<SlabHeader>
      crystalline_serial_table_;

public:
  // Codec accessor — used by BatchLinkCodec<SlabHeaderT<...>>::decode
  // (see specialization at the bottom of this header).
  [[nodiscard]] LIBC_INLINE static SlabHeader *
  slab_by_crystalline_serial(uint32_t serial) {
    return crystalline_serial_table_.lookup(serial);
  }

private:

  // -- TLS helpers (for pool-managed TLS) --

  LIBC_INLINE ThreadSlab tls_get_slab() {
    if (tls_index_ != TLS_OUT_OF_INDEXES)
      return static_cast<ThreadSlab>(teb_tls_get(tls_index_));
    return nullptr;
  }

  LIBC_INLINE void tls_set_slab(ThreadSlab slab) {
    if (tls_index_ != TLS_OUT_OF_INDEXES)
      teb_tls_set(tls_index_, slab);
  }

  // -- Lock helpers --

  LIBC_INLINE void lock_all_slabs() {
    int expected = 0;
    if (LIBC_LIKELY(all_slabs_lock_.compare_exchange_weak(
            expected, 1, cpp::MemoryOrder::ACQUIRE, cpp::MemoryOrder::RELAXED)))
      return;
    // Contended: hardware-monitor spin on the lock word, then TTAS
    // (test before CAS) to avoid unnecessary cache-line writes.
    // No yield after failed CAS — loop back to the hardware monitor
    // immediately. spin_on_raw is the right wait mechanism; yielding
    // after a CAS loss wastes a scheduler round-trip when the lock may
    // already be free.
    for (;;) {
      spin_wait::spin_on_raw(&all_slabs_lock_.val, 1);
      if (all_slabs_lock_.load(cpp::MemoryOrder::RELAXED) != 0)
        continue;
      expected = 0;
      if (all_slabs_lock_.compare_exchange_weak(
              expected, 1, cpp::MemoryOrder::ACQUIRE,
              cpp::MemoryOrder::RELAXED))
        return;
    }
  }

  LIBC_INLINE void unlock_all_slabs() {
    all_slabs_lock_.store(0, cpp::MemoryOrder::RELEASE);
  }

  LIBC_INLINE void all_slabs_insert(SlabHeader *slab) {
    lock_all_slabs();
    slab->all_prev = nullptr;
    slab->all_next = all_slabs_head_;
    if (all_slabs_head_)
      all_slabs_head_->all_prev = slab;
    all_slabs_head_ = slab;
    unlock_all_slabs();
  }

  LIBC_INLINE void all_slabs_remove(SlabHeader *slab) {
    lock_all_slabs();
    if (slab->all_prev)
      slab->all_prev->all_next = slab->all_next;
    else
      all_slabs_head_ = slab->all_next;
    if (slab->all_next)
      slab->all_next->all_prev = slab->all_prev;
    unlock_all_slabs();
  }

  // Hand the slab's VA back to the substrate. Slab-grain release —
  // acquired as a single substrate slot at alloc_slow, freed as a single
  // substrate slot here. Substrate decommits the slot's pages and the
  // arena's `live_count` is decremented; reuse comes from the substrate
  // pool, not a SlabPool-private recycle list.
  LIBC_INLINE void full_release(SlabHeader *slab) {
    slab_registry.remove_range(reinterpret_cast<uintptr_t>(slab), kSlabBytes);
    uint64_t token = slab->substrate_token;
    lock_all_slabs();
    if (slab->all_prev)
      slab->all_prev->all_next = slab->all_next;
    else
      all_slabs_head_ = slab->all_next;
    if (slab->all_next)
      slab->all_next->all_prev = slab->all_prev;
    unlock_all_slabs();
    ::LIBC_NAMESPACE::windows::alloc::substrate_release(
        ::LIBC_NAMESPACE::windows::alloc::SubSlotHandle::from_detached(slab,
                                                                       token));
  }

  // -- Freelist encoding (uniform across local_free and xthread) --
  //
  // Every chain link (node->next) is encoded:
  //   stored = real_next ^ cookie ^ &node->next
  //
  // Head pointers (SlabHeader::local_free, xthread head value) are RAW.
  // Since &node->next == node (first field), this simplifies to:
  //   stored = real_next ^ cookie ^ node
  //
  // Both local_free and xthread use the same encoding. Draining xthread
  // into local_free requires no per-node re-encoding — only the tail's
  // next pointer is patched to link the two chains.

  LIBC_INLINE static SlabFreeNode *encode_next(SlabFreeNode *ptr,
                                                uintptr_t cookie,
                                                SlabFreeNode **location) {
    return static_cast<SlabFreeNode *>(
        alloc_primitives::xor_encode_next(ptr, cookie, location));
  }

  LIBC_INLINE static SlabFreeNode *decode_next(SlabFreeNode *encoded,
                                                uintptr_t cookie,
                                                SlabFreeNode **location) {
    return static_cast<SlabFreeNode *>(
        alloc_primitives::xor_decode_next(encoded, cookie, location));
  }

  // -- Helpers --

  LIBC_INLINE static char *slab_slot(SlabHeader *slab, unsigned idx) {
    return reinterpret_cast<char *>(slab) + kSlotStartOffset +
           static_cast<size_t>(idx) * slab->slot_size;
  }

  // Map a slot pointer to its starting page index within the slot region.
  LIBC_INLINE static unsigned slot_page_index(SlabHeader *slab, void *slot) {
    auto offset = static_cast<size_t>(
        static_cast<char *>(slot) - reinterpret_cast<char *>(slab));
    return static_cast<unsigned>((offset - kSlotStartOffset) / kPageSize);
  }

  // Map a slot pointer to its ending page index (inclusive).
  LIBC_INLINE static unsigned slot_end_page_index(SlabHeader *slab,
                                                   void *slot) {
    auto end = static_cast<size_t>(
        static_cast<char *>(slot) + slab->slot_size - 1 -
        reinterpret_cast<char *>(slab));
    return static_cast<unsigned>((end - kSlotStartOffset) / kPageSize);
  }

  // -- Occupancy bitmap helpers --
  //
  // Slot index from a byte offset within the slot region. Uses precomputed
  // reciprocal multiplication instead of runtime division:
  //   slot_index = (byte_offset * slot_size_recip) >> 32
  // The uint32_t truncation is safe because kUsableBytes = 53248 < 2^32.
  // The reciprocal is exact for all slot sizes that evenly divide
  // kUsableBytes (enforced by init, verified in debug).
  LIBC_INLINE static unsigned slot_index_from_offset(SlabHeader *slab,
                                                      size_t byte_offset) {
    return static_cast<unsigned>(
        (static_cast<uint64_t>(static_cast<uint32_t>(byte_offset)) *
         slab->slot_size_recip) >> 32);
  }

  // Physical slot index from a pointer within the slab's slot region.
  LIBC_INLINE static unsigned slot_index(SlabHeader *slab, void *slot) {
    uint32_t byte_offset = static_cast<uint32_t>(
        static_cast<char *>(slot) -
        (reinterpret_cast<char *>(slab) + kSlotStartOffset));
    // Hint to the optimizer that byte_offset is within the usable slot
    // region. Enables range-check elision in callers.
    __builtin_assume(byte_offset < kUsableBytes);
    return slot_index_from_offset(slab, byte_offset);
  }

  // Number of bitmap words needed for this slab's slot count.
  // Reads the precomputed value from the header — no division.
  LIBC_INLINE static unsigned bitmap_words(SlabHeader *slab) {
    return slab->bitmap_word_count;
  }

  // Mark a slot as live in the occupancy bitmap.
  // REQUIRES: tid() == current_tid() (owner-only, non-atomic writes)
  LIBC_INLINE static void bitmap_set(SlabHeader *slab, void *slot) {
    LIBC_ASSERT(slab->tid() == current_tid());
    slab->occupancy.mark_live(slot_index(slab, slot));
  }

  // Mark a slot as dead in the occupancy bitmap.
  // REQUIRES: tid() == current_tid() (owner-only writes, RELAXED).
  // fetch_and is a true atomic RMW — robust even if the single-owner
  // invariant were ever violated, and makes the intent unambiguous.
  LIBC_INLINE static void bitmap_clear(SlabHeader *slab, void *slot) {
    LIBC_ASSERT(slab->tid() == current_tid());
    slab->occupancy.mark_dead(slot_index(slab, slot));
  }

  // page_occupancy invariant
  // ------------------------
  // page_occupancy[p] counts spanning slots that are NOT in the `free` set —
  // i.e. `live + virgin`. Each slot is in exactly one state:
  //
  //   virgin : never bump-allocated (index still ahead of bump)
  //   live   : bump-allocated OR popped from freelist, not yet freed
  //   free   : on local_free or in xthread queue awaiting drain
  //
  // Sealing is sound only when every spanning slot is `free` (page_occupancy
  // hits 0). Seeding virgins at init_slab is what makes that implication hold
  // — a seal can no longer catch the "live freed while virgins remain" trap
  // that lets bump hand out a slot on a PAGE_NOACCESS page.
  //
  // Transitions:
  //   bump  (virgin → live) : NO change to page_occupancy (both count)
  //   pop   (free   → live) : +1 per spanned page (local_free_pop handles)
  //   free  (live   → free) : -1 per spanned page (slot_vacate / fused paths)
  //   re-pop from recycled/recommitted page: +1 per spanned page

  // Increment page_occupancy for all pages a slot spans. Used on free→live
  // transitions (freelist pop, recommit handout). The bump path does NOT
  // call this: virgin slots were already counted at init_slab.
  // REQUIRES: tid() == current_tid() (owner-only, non-atomic writes)
  LIBC_INLINE static void slot_occupy(SlabHeader *slab, void *slot) {
    LIBC_ASSERT(slab->tid() == current_tid());
    unsigned first = slot_page_index(slab, slot);
    unsigned last = slot_end_page_index(slab, slot);
    for (unsigned pg = first; pg <= last && pg < kSlotPages; pg++)
      slab->page_occupancy[pg]++;
  }

  // Decrement page_occupancy for all pages a slot spans (live → free).
  // Returns true if ANY page hit zero (caller may want to seal). Hitting
  // zero under the virgin-seeded invariant guarantees the page has no
  // un-bumped slots left, so sealing cannot strand a later bump handout.
  // REQUIRES: tid() == current_tid() (owner-only, non-atomic writes)
  LIBC_INLINE static bool slot_vacate(SlabHeader *slab, void *slot) {
    LIBC_ASSERT(slab->tid() == current_tid());
    unsigned first = slot_page_index(slab, slot);
    unsigned last = slot_end_page_index(slab, slot);
    bool any_empty = false;
    for (unsigned pg = first; pg <= last && pg < kSlotPages; pg++) {
      if (--slab->page_occupancy[pg] == 0) {
        seal_slot_page(slab, pg);
        any_empty = true;
      }
    }
    return any_empty;
  }

  // Base address of a slot-region page.
  LIBC_INLINE static void *slot_page_base(SlabHeader *slab, unsigned pg) {
    return reinterpret_cast<char *>(slab) + kSlotStartOffset + pg * kPageSize;
  }

  // Page states (2 bits each, packed in page_state).
  static constexpr unsigned kPageCommitted = 0;
  static constexpr unsigned kPageSealed = 1;
  static constexpr unsigned kPageDecommitted = 2;

  // page_state is a uint64_t[kStateWords] with 2 bits per page, 32 pages
  // per word. Accessors compute (word, shift) from the page index. For the
  // default arena kStateWords == 1 and the compiler collapses `page_state[0]`
  // to a scalar load/store — same codegen as the pre-array uint32_t form.
  LIBC_INLINE static unsigned page_state_word(unsigned pg) { return pg >> 5; }
  LIBC_INLINE static unsigned page_state_shift(unsigned pg) {
    return (pg & 31u) * 2;
  }

  LIBC_INLINE static unsigned get_page_state(SlabHeader *slab, unsigned pg) {
    return static_cast<unsigned>(
        (slab->page_state[page_state_word(pg)] >> page_state_shift(pg)) & 3u);
  }

  LIBC_INLINE static void set_page_state(SlabHeader *slab, unsigned pg,
                                          unsigned state) {
    uint64_t &w = slab->page_state[page_state_word(pg)];
    unsigned shift = page_state_shift(pg);
    uint64_t mask = uint64_t{3} << shift;
    w = (w & ~mask) | (static_cast<uint64_t>(state) << shift);
  }

  // True iff every page is kPageCommitted (every state bit zero). Used on
  // the alloc hot path. For kStateWords == 1 this collapses to a single
  // load + compare; larger arenas iterate words (still one load per word).
  LIBC_INLINE static bool all_pages_committed(SlabHeader *slab) {
    for (unsigned w = 0; w < kStateWords; w++)
      if (slab->page_state[w] != 0)
        return false;
    return true;
  }

  LIBC_INLINE static void clear_all_page_states(SlabHeader *slab) {
    for (unsigned w = 0; w < kStateWords; w++)
      slab->page_state[w] = 0;
  }

  // Seal a slot-region page (PAGE_NOACCESS). Data preserved.
  LIBC_INLINE static void seal_slot_page(SlabHeader *slab, unsigned pg) {
    page_protect(slot_page_base(slab, pg), kPageSize, PAGE_NOACCESS);
    set_page_state(slab, pg, kPageSealed);
  }

  // Unseal a sealed page (restore PAGE_READWRITE). Freelist data intact.
  LIBC_INLINE static void unseal_slot_page(SlabHeader *slab, unsigned pg) {
    page_protect(slot_page_base(slab, pg), kPageSize, PAGE_READWRITE);
    set_page_state(slab, pg, kPageCommitted);
  }

  // Count sealed pages (state == kPageSealed = 01). The 2-bit page_state
  // packs every page in `kStateWords` 64-bit words; sealed pages have low
  // bit set and high bit clear. Mask the lo-bits-only (`& 0x55..55`) and
  // AND with the bitwise-inverted high bits (`& ~(s >> 1)`) to isolate
  // exactly the sealed-page bits, then popcount. For default kStateWords
  // == 1 this collapses to four ALU ops + one popcount.
  LIBC_INLINE static unsigned count_sealed_pages(SlabHeader *slab) {
    constexpr uint64_t kLoBits = 0x5555555555555555ULL;
    unsigned cnt = 0;
    for (unsigned w = 0; w < kStateWords; w++) {
      uint64_t s = slab->page_state[w];
      uint64_t sealed_mask = (s & kLoBits) & ~(s >> 1);
      cnt += static_cast<unsigned>(__builtin_popcountll(sealed_mask));
    }
    return cnt;
  }

  // Threshold for opportunistic compact-on-free. compact_sealed_pages
  // walks local_free + does one page_protect per page being unsealed +
  // one page_decommit per page being released, so we want to amortize
  // its cost across many seals. Half-the-slab-sealed (~26 KB pinned RSS
  // for the default 64 KB slab) is the trade-off point: low enough that
  // RSS doesn't drift unbounded under steady-state churn, high enough
  // that compact runs O(seals / kSlotPages) times per slab lifetime,
  // not O(seals).
  static constexpr unsigned kCompactThreshold =
      kSlotPages > 1 ? (kSlotPages / 2) : 1;

  // Decommit a sealed page. Physical memory returned. Data destroyed.
  // UAF protection persists (page stays PAGE_NOACCESS).
  LIBC_INLINE static void decommit_slot_page(SlabHeader *slab, unsigned pg) {
    page_decommit(slot_page_base(slab, pg), kPageSize);
    set_page_state(slab, pg, kPageDecommitted);
  }

  // Recommit a decommitted page. Zero-filled by OS. Slots available
  // for bump-style allocation (no freelist nodes — they were destroyed).
  LIBC_INLINE static bool recommit_slot_page(SlabHeader *slab, unsigned pg) {
    if (!page_commit(slot_page_base(slab, pg), kPageSize))
      return false;
    set_page_state(slab, pg, kPageCommitted);
    return true;
  }

  // Ensure a slot's page is accessible for allocator-internal reads.
  // Only unseals sealed pages. Decommitted pages are NOT touched here —
  // they require recommit + freelist reconstruction (handled by caller).
  LIBC_INLINE static void ensure_slot_accessible(SlabHeader *slab,
                                                  void *slot) {
    unsigned pg = slot_page_index(slab, slot);
    if (get_page_state(slab, pg) == kPageSealed)
      unseal_slot_page(slab, pg);
  }

  LIBC_INLINE static uint32_t current_tid() {
#if defined(__x86_64__)
    uint32_t tid;
    LIBC_INLINE_ASM("movl %%gs:0x48, %0" : "=r"(tid));
    return tid;
#elif defined(__aarch64__)
    uint64_t tid;
    LIBC_INLINE_ASM("ldr %0, [x18, #0x48]" : "=r"(tid));
    return static_cast<uint32_t>(tid);
#endif
  }

  LIBC_INLINE void init_slab(SlabHeader *slab) {
    // slab_tag below reads slab_registry.slab_secret_ — ensure the
    // registry has published a non-zero secret before we consume it.
    // Idempotent and ACQUIRE-fenced; fast path is a single atomic
    // load once steady state is reached. Without this, the first
    // slab in a process sees slab_secret_ == 0, writes slab_tag =
    // base, and every subsequent free() into it traps because the
    // later-initialized secret no longer matches.
    slab_registry.ensure_init();
    // Stamp the Crystalline-W codec serial. Monotonic per pool, never
    // recycled — Crystalline grace ensures the slab is fully drained
    // before the FreeFn fires, so even if a serial were reused (it
    // isn't), no stale batch_link could decode to the wrong slab.
    // Publish into the per-pool serial_table BEFORE any thread can
    // observe the slab via Crystalline retire (init_slab runs under
    // the slab's exclusive owner discipline).
    if (slab->crystalline_serial == 0) {
      uint32_t serial =
          crystalline_serial_counter_.fetch_add(1, cpp::MemoryOrder::RELAXED) +
          1;
      slab->crystalline_serial = serial;
      crystalline_serial_table_.insert(serial, slab);
    }
    slab->local_free = nullptr;
    slab->bump = 0;
    slab->slots_per_slab = slots_per_slab_;
    slab->slot_size = slot_size_;
    // Reciprocal: ceil(2^32 / slot_size). Exact for power-of-2 sizes;
    // for non-power-of-2, rounding up ensures (offset * recip) >> 32
    // gives the correct floor(offset / slot_size) for all in-range offsets.
    slab->slot_size_recip =
        static_cast<uint32_t>((0x100000000ULL + slot_size_ - 1) / slot_size_);
#ifndef NDEBUG
    // Verify reciprocal correctness at the boundaries: first and last
    // valid slot offsets must round-trip through the multiply-shift.
    // Catches rounding errors for non-power-of-2 slot sizes.
    {
      uint32_t last_off = static_cast<uint32_t>(
          (slots_per_slab_ - 1) * static_cast<uint32_t>(slot_size_));
      unsigned last_idx = static_cast<unsigned>(
          (static_cast<uint64_t>(last_off) * slab->slot_size_recip) >> 32);
      LIBC_ASSERT(last_idx == slots_per_slab_ - 1 &&
                  "reciprocal multiply gives wrong index for last slot");
      LIBC_ASSERT(static_cast<unsigned>(
                      (static_cast<uint64_t>(0) * slab->slot_size_recip) >>
                       32) == 0 &&
                  "reciprocal multiply gives wrong index for slot 0");
    }
#endif
    slab->class_index = class_index_;
    slab->bitmap_word_count =
        static_cast<uint16_t>((slots_per_slab_ + 63) / 64);
    slab->returned = 0;
    slab->pool = this;
    slab->claim(current_tid());
    slab->next_abandoned = nullptr;
    slab->all_next = nullptr;
    slab->all_prev = nullptr;
    // Seal-state defaults: alive slabs don't consult these fields,
    // but adopt-and-re-seal paths assume they start zeroed. Init here
    // is the single source of truth — abandon/adopt overwrite as needed.
    // Fresh slabs are alive immediately (claim follows below); the
    // `retire_initiated == kRetireIdle` value is the natural "no
    // adoption-lock held" state for a slab that was never abandoned.
    slab->sealed_target.store(0, cpp::MemoryOrder::RELAXED);
    slab->xthread_returned_count.store(0, cpp::MemoryOrder::RELAXED);
    // Stamp retire_initiated with state_'s current seq (post-claim() above)
    // so retire_initiated.seq matches state_.seq from this slab's first
    // moment. Future transitions advance both in lockstep.
    slab->retire_initiated.store(
        SlabHeader::pack_retire(slab->state_seq(), SlabHeader::kRetireIdle),
        cpp::MemoryOrder::RELAXED);
    // Seed per-slab secrets from OS entropy (independent keys).
    // Two-seed variant: freelist_cookie (primary) encodes freelist links;
    // canary_key (secondary) authenticates the post-free slot body. Keys
    // must stay separate — a single seed would let a freelist UAF forge
    // valid canaries. init_seed_or_trap is fail-closed on ProcessPrng
    // failure or a zero draw in either position.
    alloc_primitives::CanarySeed seeds;
    alloc_primitives::init_seed_or_trap(seeds);
    slab->freelist_cookie = seeds.primary;
    slab->canary_key = seeds.secondary;
    // Vyukov MPSC queue init must come after `freelist_cookie` is seeded —
    // it encodes the sentinel stub's `next` pointer using the cookie, so
    // reading it back with a later-seeded cookie would decode to garbage.
    xthread_queue_init(slab);
    // Authenticity tag: global secret XOR'd with slab base address.
    // Verified on every free() — one XOR + one CMP, cache line 1.
    // Guarded by ensure_init() at entry; assert catches any future
    // regression that reintroduces a zero-secret window here.
    LIBC_ASSERT(slab_registry.slab_secret_ != 0);
    slab->slab_tag = slab_registry.slab_secret_ ^
                     reinterpret_cast<uintptr_t>(slab);
    // Randomized bump start for heap spray resistance.
    // Lemire's fast range reduction: (rand16 * N) >> 16 maps [0, 65535]
    // uniformly onto [0, N-1] with at most 1/65536 bias — one MUL + SHR,
    // no branch, no modular bias from % on non-power-of-2 slot counts.
    uint16_t rand_val;
    if (!::ProcessPrng(reinterpret_cast<unsigned char *>(&rand_val),
                       sizeof(rand_val)))
      __builtin_trap();
    slab->bump_offset = static_cast<uint16_t>(
        (static_cast<uint32_t>(rand_val) * slots_per_slab_) >> 16);
    // Seed page_occupancy with the virgin spanning-slot count per page.
    // Under the live+virgin invariant this is the starting count; bump
    // handouts are a no-op, free decrements, and seal-at-zero triggers
    // correctly only after every spanning slot has been freed.
    //
    // O(kSlotPages), not O(slots_per_slab): slot i spans page p iff
    //   i*ss < pg_end  and  (i+1)*ss > pg_start
    // which translate to
    //   i_lo = floor(pg_start / ss)     (smallest i satisfying #2)
    //   i_hi = floor((pg_end - 1) / ss) (largest i satisfying #1)
    // after clamping i_hi to n-1. Count per page = i_hi - i_lo + 1.
    const uint32_t ss = slot_size_;
    const unsigned n = slots_per_slab_;
    for (unsigned p = 0; p < kSlotPages; p++) {
      size_t pg_start = static_cast<size_t>(p) * kPageSize;
      size_t pg_end = pg_start + kPageSize;
      unsigned i_lo = static_cast<unsigned>(pg_start / ss);
      unsigned i_hi = static_cast<unsigned>((pg_end - 1) / ss);
      if (i_hi >= n)
        i_hi = n - 1;
      slab->page_occupancy[p] =
          i_lo <= i_hi ? static_cast<uint16_t>(i_hi - i_lo + 1) : uint16_t{0};
    }
    clear_all_page_states(slab); // All pages kPageCommitted.
    // Bounded clear matching the live-prefix width. Trailing words
    // beyond the live range are guaranteed zero because the header
    // page lands here freshly committed: the substrate decommits the
    // slot's pages on release, so the next acquire's commit-on-write
    // returns OS-zeroed pages. SlabPool instances have a fixed
    // slot_size for their lifetime, so bitmap_word_count is constant
    // per pool — no size-class churn can expose previously-unreached
    // words on a substrate-recycled slot.
    slab->occupancy.clear_first_words(bitmap_words(slab));
  }

  // Result of drain_xthread: slot count and bitmask of pages touched.
  struct DrainResult {
    unsigned count;
    PageMask touched_mask; // One bit per slot-region page with drained nodes.
  };

  // Drain the per-slab cross-thread MPSC queue into local_free.
  //
  // Walks head_ → head_->next, dequeueing one entry per iteration,
  // mirroring the try_pop algorithm in signal/pending/rt_queue.h. Key
  // properties (inherited from that design):
  //
  //   * Producers are wait-free (one ACQ_REL XCHG + two RELEASE stores).
  //   * Consumer never takes ownership of an entry until it has observed
  //     a non-null back-link from that entry's predecessor — no sentinel
  //     game, no fallback wait-or-leak.
  //   * An in-flight producer between Phase 2 (tail-XCHG) and Phase 3
  //     (prev->next publish) makes the current head look like "last
  //     real entry with no successor." We resolve that via a short
  //     UMWAIT (tsc_multiplier=256 ≈ 1 µs) on the producer's impending
  //     Phase-3 write; if the write still hasn't landed, we **leave the
  //     entry in the queue** and return what we have drained so far.
  //     The next drain triggered by alloc_from_slab picks it up.
  //
  // The "h is truly the last real entry" branch re-inserts the permanent
  // xthread_stub as the new tail so head_ can advance past h — identical
  // in structure to rt_queue's stub re-push. If a producer races in
  // between our tail check and our stub-XCHG, the stub-push still closes
  // the chain correctly (it appends behind whoever the current tail is);
  // we simply reread h->next to cover either outcome.
  //
  // Page unsealing, bitmap clearing, and page_occupancy decrement happen
  // per dequeued entry, same as the previous batch walk. Sealing is
  // still deferred to seal_empty_pages_after_drain — freelist nodes
  // remain accessible throughout drain.
  // Validate an xthread chain link before storing it as the new
  // xthread_head or otherwise dereferencing it. Accepts nullptr (queue-
  // empty sentinel) and the per-slab stub (re-inserted as tail in case
  // (b)); every other value must point inside the slot region. Without
  // this guard a UAF or post-cookie bit-flip could store a wild pointer
  // into xthread_head, and the next drain iteration would AV on
  // `h->next` before the post-dequeue trap can fire. One unsigned
  // compare against compile-time constants on the success path; cold
  // trap on corruption.
  LIBC_INLINE static void validate_xthread_link(SlabHeader *slab,
                                                 SlabFreeNode *node) {
    if (!node || node == &slab->xthread_stub)
      return;
    uintptr_t off = reinterpret_cast<uintptr_t>(node) -
                    reinterpret_cast<uintptr_t>(slab);
    if (LIBC_UNLIKELY(off < kSlotStartOffset ||
                       off >= kTrailingGuardOffset))
      __builtin_trap();
  }

  LIBC_INLINE static DrainResult drain_xthread(SlabHeader *slab) {
    uintptr_t ck = slab->freelist_cookie;
    PageMask unsealed_mask;
    PageMask touched_mask;
    uintptr_t slab_addr = reinterpret_cast<uintptr_t>(slab);

    SlabFreeNode *drained_head = nullptr;
    SlabFreeNode *drained_tail = nullptr;
    unsigned count = 0;

    for (;;) {
      SlabFreeNode *h = slab->xthread_head;
      SlabFreeNode *next = decode_next(
          __atomic_load_n(&h->next, __ATOMIC_ACQUIRE), ck, &h->next);
      validate_xthread_link(slab, next);

      // Transparently skip the permanent stub node.
      if (h == &slab->xthread_stub) {
        if (!next)
          break; // Queue empty — or stub is predecessor of a mid-push
                 // producer; next drain triggered by the next alloc
                 // picks it up once Phase 3 lands.
        slab->xthread_head = next;
        h = next;
        next = decode_next(
            __atomic_load_n(&h->next, __ATOMIC_ACQUIRE), ck, &h->next);
        validate_xthread_link(slab, next);
      }

      // h is a real entry. If its successor link isn't visible yet, we
      // either (a) need to wait briefly for a producer's in-flight Phase
      // 3 write, or (b) h is genuinely the queue tail and we need to
      // re-insert stub so head_ can advance past it.
      if (!next) {
        uintptr_t tail_packed =
            slab->xthread_tail.load(cpp::MemoryOrder::ACQUIRE);
        SlabFreeNode *tail = xthread_unpack_node(slab, tail_packed);
        if (tail != h) {
          // Case (a): producer mid-push (tail is their node, Phase 3
          // writing our h->next still in flight). Short UMWAIT on the
          // encoded-null bit pattern — wakes on any cache-line write.
          uint64_t null_bits =
              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
                  encode_next(nullptr, ck, &h->next)));
          spin_wait::spin_on_raw<uint64_t>(
              reinterpret_cast<uint64_t *>(&h->next), null_bits,
              /*tsc_multiplier=*/256);
          next = decode_next(
              __atomic_load_n(&h->next, __ATOMIC_ACQUIRE), ck, &h->next);
          validate_xthread_link(slab, next);
          if (!next)
            break; // Still incomplete — leave h in queue. No leak: h
                   // stays reachable via xthread_head, and next drain
                   // pass finds it with Phase 3 long since landed.
        } else {
          // Case (b): h is currently the tail. Re-insert the stub as the
          // new tail so head_ can advance past h.
          __atomic_store_n(
              &slab->xthread_stub.next,
              encode_next(nullptr, ck, &slab->xthread_stub.next),
              __ATOMIC_RELEASE);
          uintptr_t stub_packed = xthread_pack(slab, &slab->xthread_stub);
          uintptr_t prev_packed = slab->xthread_tail.exchange(
              stub_packed, cpp::MemoryOrder::ACQ_REL);
          auto *prev = xthread_unpack_node(slab, prev_packed);
          __atomic_store_n(
              &prev->next,
              encode_next(&slab->xthread_stub, ck, &prev->next),
              __ATOMIC_RELEASE);
          // Usually prev == h (→ h->next = stub). If a producer raced in
          // between our tail check and our XCHG, prev is that producer's
          // node and they (or their Phase 3) wrote h->next = their_node.
          // Either outcome leaves h->next non-null and walkable.
          next = decode_next(
              __atomic_load_n(&h->next, __ATOMIC_ACQUIRE), ck, &h->next);
          validate_xthread_link(slab, next);
          if (!next)
            break; // Sneak-race producer's Phase 3 not yet visible;
                   // retry on next drain.
        }
      }

      // Dequeue h: advance head_ past it.
      slab->xthread_head = next;

      // Process h: bounds, unseal, bitmap, page_occupancy decrement.
      uintptr_t off = reinterpret_cast<uintptr_t>(h) - slab_addr;
      if (LIBC_UNLIKELY(off < kSlotStartOffset ||
                         off >= kTrailingGuardOffset))
        __builtin_trap();
      unsigned pg = slot_page_index(slab, h);
      if (!unsealed_mask.test(pg) &&
          get_page_state(slab, pg) == kPageSealed) {
        unseal_slot_page(slab, pg);
        unsealed_mask.set(pg);
      }
      bitmap_clear(slab, h);
      unsigned first = pg;
      unsigned last = slot_end_page_index(slab, h);
      for (unsigned p = first; p <= last && p < kSlotPages; p++) {
        touched_mask.set(p);
        --slab->page_occupancy[p];
      }

      if (!drained_head)
        drained_head = h;
      drained_tail = h;
      count++;
    }

    if (!drained_tail)
      return {0, PageMask{}};

    // Splice drained chain onto local_free. drained_tail->next currently
    // points into xthread_free territory (stub, a still-queued entry, or
    // a nullptr sentinel) — overwrite it to link into local_free.
    drained_tail->next =
        encode_next(slab->local_free, ck, &drained_tail->next);
    slab->local_free = drained_head;

    // Overflow guard: returned + count must not exceed bump (the number of
    // slots ever handed out). A corrupted xthread chain could inject extra
    // nodes, pushing returned past bump — which would make the epoch check
    // (returned == bump) pass prematurely, releasing a slab with live slots.
    // Trap unconditionally: this corruption is unrecoverable.
    // Use wider arithmetic to avoid uint16_t underflow if returned > bump
    // due to prior corruption (belt-and-suspenders).
    if (LIBC_UNLIKELY(static_cast<unsigned>(slab->returned) + count >
                      static_cast<unsigned>(slab->bump)))
      __builtin_trap();

    slab->returned += count;
    return {count, touched_mask};
  }

  // Seal pages whose occupancy dropped to zero during drain_xthread.
  // drain_xthread cannot seal during its walk because freelist nodes on
  // those pages are still being chased. Call this after drain completes
  // and before allocating, so physical memory from emptied pages is
  // returned promptly rather than waiting for the next alloc_slow pass.
  //
  // touched_mask limits the scan to only pages that had occupancy
  // decremented during drain — avoids checking all kSlotPages when
  // cross-thread frees landed on just 1-2 pages (common case).
  // REQUIRES: tid() == current_tid()
  LIBC_INLINE static void seal_empty_pages_after_drain(SlabHeader *slab,
                                                        PageMask touched_mask) {
    LIBC_ASSERT(slab->tid() == current_tid());
    bool any_sealed = false;
    while (touched_mask.any()) {
      unsigned pg = touched_mask.first_set();
      touched_mask.clear_lowest(); // blsr — clear lowest set bit.
      if (slab->page_occupancy[pg] == 0 &&
          get_page_state(slab, pg) == kPageCommitted) {
        seal_slot_page(slab, pg);
        any_sealed = true;
      }
    }
    // Opportunistic compaction — see same call site in the owner fast-
    // free path above for rationale. Pure xthread-fed workloads (the
    // alloc-on-thread-A / free-on-thread-B pattern) only seal here, so
    // without this trigger they never see compaction at all.
    if (any_sealed && count_sealed_pages(slab) >= kCompactThreshold)
      compact_sealed_pages(slab);
  }

  // Pop one node from local_free. Restores sealed/decommitted pages,
  // verifies canary (detects UAF writes), then clears it.
  //
  // Fast path (>98% of calls): all pages committed, slot within one page.
  // Skips slot_end_page_index, page_state reads, and the loop entirely.
  // Precomputes byte_offset once for page index, slot index, and bitmap.
  [[nodiscard]] LIBC_INLINE static void *local_free_pop(SlabHeader *slab) {
    SlabFreeNode *node = slab->local_free;

    // Precompute byte offset from slot region start — shared by page
    // index derivation and bitmap slot index (avoids redundant ptr math).
    size_t byte_offset = static_cast<size_t>(
        reinterpret_cast<char *>(node) -
        (reinterpret_cast<char *>(slab) + kSlotStartOffset));

    // Bounds check: a corrupted local_free head (raw pointer, not encoded)
    // would produce an OOB write to page_occupancy before the canary check
    // runs. One compare against a compile-time constant, ~1 cycle.
    if (LIBC_UNLIKELY(byte_offset >= kUsableBytes))
      __builtin_trap();

    unsigned first_pg = static_cast<unsigned>(byte_offset >> 12);

    // Fast path: all pages committed AND slot fits within one page.
    // The intra-page check avoids computing slot_end_page_index entirely.
    if (LIBC_LIKELY(all_pages_committed(slab) &&
                    (byte_offset & (kPageSize - 1)) +
                        slab->slot_size <= kPageSize)) {
      slab->page_occupancy[first_pg]++;
    } else {
      // Slow path: spanning slot or sealed/decommitted pages.
      unsigned last_pg = static_cast<unsigned>(
          (byte_offset + slab->slot_size - 1) >> 12);
      for (unsigned pg = first_pg; pg <= last_pg && pg < kSlotPages; pg++) {
        unsigned state = get_page_state(slab, pg);
        if (state == kPageSealed)
          unseal_slot_page(slab, pg);
        else if (state == kPageDecommitted) {
          if (LIBC_UNLIKELY(!recommit_slot_page(slab, pg)))
            __builtin_trap(); // Commit charge exhausted — unrecoverable.
        }
        slab->page_occupancy[pg]++;
      }
    }

    slab->returned--;

    uintptr_t ck = slab->freelist_cookie;
    SlabFreeNode *next_head = decode_next(node->next, ck, &node->next);

    // Validate the decoded next pointer before it becomes the new
    // local_free head. A bit-flip in node->next decodes to a wild
    // address that silently propagates as the freelist head — the
    // corruption only surfaces on the *next* pop as a hard fault.
    // Catching it here confines damage to this alloc, not the next.
    if (next_head) {
      uintptr_t next_off = reinterpret_cast<uintptr_t>(next_head) -
                            reinterpret_cast<uintptr_t>(slab);
      if (LIBC_UNLIKELY(next_off < kSlotStartOffset ||
                         next_off >= kTrailingGuardOffset))
        __builtin_trap(); // Corrupted freelist next pointer.
    }
    slab->local_free = next_head;

    // Verify then clear canary. A mismatch means a UAF write corrupted
    // the slot between free() and this alloc.
    if (slab->slot_size >= kCanaryOffset + sizeof(uintptr_t)) {
      auto *canary_ptr = reinterpret_cast<uintptr_t *>(
          reinterpret_cast<char *>(node) + kCanaryOffset);
      if (*canary_ptr != make_canary(slab, node))
        __builtin_trap(); // UAF write detected.
      *canary_ptr = 0;
    }

    // Bitmap set using precomputed byte_offset (shares ptr subtraction
    // with page index above instead of recomputing in slot_index).
    slab->occupancy.mark_live(slot_index_from_offset(slab, byte_offset));

    return node;
  }

  // Compact sealed pages: unlink their freelist nodes, then decommit.
  // Returns physical memory to the OS while preserving UAF protection
  // (decommitted pages are still PAGE_NOACCESS). Called on the slow path.
  LIBC_INLINE static void compact_sealed_pages(SlabHeader *slab) {
    // Quick check: any sealed pages?
    if (all_pages_committed(slab))
      return; // All pages committed, nothing to compact.

    // Snapshot which pages are sealed, then unseal them all so the
    // freelist walk can read node data.
    PageMask sealed_mask;
    for (unsigned pg = 0; pg < kSlotPages; pg++) {
      if (get_page_state(slab, pg) == kPageSealed) {
        sealed_mask.set(pg);
        page_protect(slot_page_base(slab, pg), kPageSize, PAGE_READWRITE);
      }
    }
    if (!sealed_mask.any())
      return;

    uintptr_t ck = slab->freelist_cookie;

    // Walk local_free, rebuild chain excluding nodes on sealed pages.
    SlabFreeNode *new_head = nullptr;
    SlabFreeNode *new_tail = nullptr;
    SlabFreeNode *node = slab->local_free;
    unsigned removed = 0;

    while (node) {
      // Check ALL pages the slot spans — a slot starting on a committed
      // page may extend onto a sealed page. If any spanned page is in the
      // sealed set, the node must be removed; otherwise decommitting the
      // sealed page leaves a freelist entry pointing to inaccessible memory.
      unsigned first_pg = slot_page_index(slab, node);
      unsigned last_pg = slot_end_page_index(slab, node);
      SlabFreeNode *next = decode_next(node->next, ck, &node->next);

      // Per-page test across the [first_pg, last_pg] span. For the default
      // arena this compiles to two masked bit tests on a single uint64_t;
      // for larger arenas PageMask::test walks the appropriate word.
      bool spans_sealed = false;
      for (unsigned p = first_pg; p <= last_pg; p++) {
        if (sealed_mask.test(p)) {
          spans_sealed = true;
          break;
        }
      }
      if (spans_sealed) {
        removed++;
      } else {
        if (new_tail) {
          new_tail->next = encode_next(node, ck, &new_tail->next);
        } else {
          new_head = node;
        }
        new_tail = node;
        node->next = encode_next(nullptr, ck, &node->next);
      }
      node = next;
    }

    slab->local_free = new_head;
    slab->returned -= removed;

    // Decommit sealed pages. Physical memory returned, UAF protection
    // preserved (decommitted pages are PAGE_NOACCESS).
    for (unsigned pg = 0; pg < kSlotPages; pg++) {
      if (sealed_mask.test(pg))
        decommit_slot_page(slab, pg);
    }
  }

  // Recommit a decommitted page and add its slots to local_free.
  // Returns the first slot (for immediate use by alloc), or nullptr.
  //
  // Uses index-based iteration (not offset stepping) to correctly find
  // all slot positions on the page regardless of slot_size alignment
  // with page boundaries. Also recovers spanning slots from the previous
  // page whose tail was decommitted, provided the start page is committed.
  [[nodiscard]] LIBC_INLINE static void *recommit_page_slots(SlabHeader *slab) {
    for (unsigned pg = 0; pg < kSlotPages; pg++) {
      if (get_page_state(slab, pg) != kPageDecommitted)
        continue;
      if (!recommit_slot_page(slab, pg))
        continue;

      uint32_t ss = slab->slot_size;
      uintptr_t ck = slab->freelist_cookie;
      size_t pg_start = static_cast<size_t>(pg) * kPageSize; // Offset within slot region.
      size_t pg_end = pg_start + kPageSize;

      // First slot index starting on this page: ceil(pg_start / ss).
      unsigned first_idx = static_cast<unsigned>((pg_start + ss - 1) / ss);

      // Check for a spanning slot from the previous page: the slot just
      // before first_idx may start on page pg-1 and extend onto page pg.
      // Compaction removed it (the fix above checks all spanned pages);
      // re-add it if its start page is now committed.
      if (first_idx > 0) {
        unsigned span_idx = first_idx - 1;
        size_t span_start = static_cast<size_t>(span_idx) * ss;
        size_t span_end = span_start + ss;
        if (span_start < pg_start && span_end > pg_start) {
          // This slot spans from the previous page onto ours.
          unsigned start_pg = static_cast<unsigned>(span_start / kPageSize);
          if (get_page_state(slab, start_pg) == kPageCommitted) {
            auto *slot = slab_slot(slab, span_idx);
            auto *node = reinterpret_cast<SlabFreeNode *>(slot);
            node->next = encode_next(slab->local_free, ck, &node->next);
            if (ss >= kCanaryOffset + sizeof(uintptr_t))
              *reinterpret_cast<uintptr_t *>(
                  reinterpret_cast<char *>(slot) + kCanaryOffset) =
                  make_canary(slab, slot);
            slab->local_free = node;
            slab->returned++;
          }
        }
      }

      void *first = nullptr;
      unsigned pushed = 0;
      for (unsigned idx = first_idx; idx < slab->slots_per_slab; idx++) {
        size_t slot_off = static_cast<size_t>(idx) * ss;
        if (slot_off >= pg_end)
          break; // Past this page.

        // If the slot spans onto the next page, only add it if the next
        // page is committed. Otherwise defer — it will be recovered when
        // the next page is recommitted.
        size_t slot_end = slot_off + ss;
        if (slot_end > pg_end) {
          unsigned next_pg = static_cast<unsigned>(
              (slot_end - 1) / kPageSize);
          if (next_pg < kSlotPages &&
              get_page_state(slab, next_pg) != kPageCommitted)
            continue;
        }

        auto *slot = slab_slot(slab, idx);

        if (!first) {
          // First slot is returned directly to the caller.
          first = slot;
          slot_occupy(slab, slot);
          bitmap_set(slab, slot);
          continue;
        }
        // Remaining slots go to local_free with proper canary.
        auto *node = reinterpret_cast<SlabFreeNode *>(slot);
        node->next = encode_next(slab->local_free, ck, &node->next);
        if (ss >= kCanaryOffset + sizeof(uintptr_t)) {
          *reinterpret_cast<uintptr_t *>(
              reinterpret_cast<char *>(slot) + kCanaryOffset) =
              make_canary(slab, slot);
        }
        slab->local_free = node;
        pushed++;
      }
      // Restore returned count for slots re-added to freelist.
      // (Compaction subtracted them; they're free again now.)
      slab->returned += pushed;
      return first;
    }
    return nullptr;
  }

  // Allocate from a specific slab (owner thread).
  // REQUIRES: tid() == current_tid()
  [[nodiscard]] LIBC_INLINE static void *alloc_from_slab(SlabHeader *slab) {
    LIBC_ASSERT(slab->tid() == current_tid());
    // 1. Pop from local_free (zero atomics, decode hardened pointer).
    if (slab->local_free)
      return local_free_pop(slab);

    // 2. Drain cross-thread returns from the MPSC queue. Cheap when
    // empty — drain_xthread's first iteration reads xthread_head (owner-
    // private) and stub->next (one ACQUIRE load) and returns if empty.
    auto dr = drain_xthread(slab);
    if (dr.count) {
      // Seal pages emptied by cross-thread frees — returns physical memory
      // promptly instead of deferring to the next compact_sealed_pages pass.
      seal_empty_pages_after_drain(slab, dr.touched_mask);
      if (slab->local_free)
        return local_free_pop(slab);
    }

    // 3. Bump allocate from virgin slots (zero atomics).
    // bump_offset randomizes the starting slot within the slab.
    //
    // No slot_occupy here: page_occupancy was seeded in init_slab to count
    // every spanning slot (virgin included), so virgin→live is a no-op.
    // This is what keeps bump safe against mid-slab seal/decommit: pages
    // can only seal when every spanning slot has been freed, so a virgin
    // slot's pages are guaranteed kPageCommitted.
    if (slab->bump < slab->slots_per_slab) {
      unsigned raw = slab->bump++;
      // Branchless modular reduction: sum < 2*N, so subtract-and-select.
      // Replaces runtime div (~20-90 cycles) with sub+cmov (~1 cycle).
      unsigned sum = raw + slab->bump_offset;
      unsigned idx = sum >= slab->slots_per_slab
                         ? sum - slab->slots_per_slab
                         : sum;
      char *slot = slab_slot(slab, idx);
      bitmap_set(slab, slot);
      return slot;
    }

    // 4. Compact sealed pages → decommit (returns physical memory,
    // preserves UAF protection). Then recommit one for fresh slots.
    compact_sealed_pages(slab);
    void *slot = recommit_page_slots(slab);
    if (slot)
      return slot;

    return nullptr; // Slab fully exhausted (all pages live).
  }

  // Initialize the per-slab Vyukov MPSC queue to empty.
  // Head = tail = &stub, stub->next = encoded nullptr.
  // Called once per slab from init_slab, and from fork_reinit for
  // dead-thread slabs where the snapshotted queue state is unreliable.
  LIBC_INLINE static void xthread_queue_init(SlabHeader *slab) {
    uintptr_t ck = slab->freelist_cookie;
    slab->xthread_stub.next =
        encode_next(nullptr, ck, &slab->xthread_stub.next);
    slab->xthread_head = &slab->xthread_stub;
    slab->xthread_tail.store(xthread_pack(slab, &slab->xthread_stub),
                              cpp::MemoryOrder::RELAXED);
  }

  // Push a freed slot onto the cross-thread MPSC queue. Vyukov enqueue:
  // wait-free on the producer side (one RELEASE store, one ACQ_REL XCHG,
  // one RELEASE store). No retry loop, no sentinel, no producer cost
  // that grows with contention — producers serialize at the XCHG on
  // xthread_tail only.
  //
  // The consumer (drain_xthread) walks head→head->next and leaves any
  // entry whose successor link is still null (i.e. the producer has
  // XCHG'd but not yet written the back-link) in the queue for a later
  // drain pass. No consumer wait beyond a ~1 µs UMWAIT, no leaks.
  LIBC_INLINE static void xthread_push(SlabHeader *slab, SlabFreeNode *node) {
    uintptr_t ck = slab->freelist_cookie;

    // Our node is the new tail — its next is nullptr until a later
    // producer appends behind us. RELEASE publishes the slot's hardened
    // payload to any consumer that later observes node as tail.
    __atomic_store_n(&node->next,
                     encode_next(nullptr, ck, &node->next),
                     __ATOMIC_RELEASE);

    // ACQ_REL XCHG: install ourselves as tail, obtain predecessor. ACQUIRE
    // synchronizes with the predecessor's own tail-XCHG so we can safely
    // write into prev->next below. RELEASE publishes our payload writes.
    uintptr_t desired = xthread_pack(slab, node);
    uintptr_t prev_packed = slab->xthread_tail.exchange(
        desired, cpp::MemoryOrder::ACQ_REL);
    auto *prev = xthread_unpack_node(slab, prev_packed);

    // Link predecessor → us. RELEASE pairs with the consumer's ACQUIRE
    // load of prev->next in try_pop so all of our payload writes are
    // visible when the consumer observes the link.
    __atomic_store_n(&prev->next,
                     encode_next(node, ck, &prev->next),
                     __ATOMIC_RELEASE);
  }

  // Push an encoded chain [head..tail] onto the cross-thread MPSC queue.
  // Interior links are already set by the caller. We only need to
  // terminate the chain (tail->next = encoded nullptr) and splice the
  // head behind the current queue tail.
  LIBC_INLINE static void xthread_push_chain(SlabHeader *slab,
                                              SlabFreeNode *head,
                                              SlabFreeNode *tail) {
    uintptr_t ck = slab->freelist_cookie;

    // Terminate the chain. RELEASE publishes caller-side payload writes.
    __atomic_store_n(&tail->next,
                     encode_next(nullptr, ck, &tail->next),
                     __ATOMIC_RELEASE);

    // Install our chain_tail as queue tail; obtain predecessor.
    uintptr_t desired = xthread_pack(slab, tail);
    uintptr_t prev_packed = slab->xthread_tail.exchange(
        desired, cpp::MemoryOrder::ACQ_REL);
    auto *prev = xthread_unpack_node(slab, prev_packed);

    // Splice: predecessor → chain_head.
    __atomic_store_n(&prev->next,
                     encode_next(head, ck, &prev->next),
                     __ATOMIC_RELEASE);
  }

  // Double-free check only. Traps if the slot already carries a valid
  // canary (which means it was freed and not re-allocated). Does NOT
  // write anything — the caller places the canary at the appropriate
  // point in its own sequence.
  LIBC_INLINE static void check_canary(void *slot, SlabHeader *slab) {
    if (slab->slot_size >= kCanaryOffset + sizeof(uintptr_t)) {
      auto *cp = reinterpret_cast<uintptr_t *>(static_cast<char *>(slot) +
                                                kCanaryOffset);
      if (*cp == make_canary(slab, slot))
        __builtin_trap(); // Double-free detected.
    }
  }

  // Non-trapping canary predicate. Returns true iff the slot carries a
  // valid canary — i.e. it was hardened by harden_slot/set_canary. For
  // slot sizes too small to hold a canary the contract is vacuously
  // true (there is nothing to verify). Intended for debug assertions
  // that express a "slot must already be hardened" precondition.
  LIBC_INLINE static bool has_canary(void *slot, SlabHeader *slab) {
    if (slab->slot_size < kCanaryOffset + sizeof(uintptr_t))
      return true;
    auto *cp = reinterpret_cast<uintptr_t *>(static_cast<char *>(slot) +
                                              kCanaryOffset);
    return *cp == make_canary(slab, slot);
  }

  // Place the canary at offset kCanaryOffset within the slot.
  LIBC_INLINE static void set_canary(void *slot, SlabHeader *slab) {
    if (slab->slot_size >= kCanaryOffset + sizeof(uintptr_t)) {
      *reinterpret_cast<uintptr_t *>(static_cast<char *>(slot) +
                                     kCanaryOffset) =
          make_canary(slab, slot);
    }
  }

  // Full hardening: check → zero → canary. Used when the page stays
  // committed (other slots are live — data must be scrubbed for info
  // leak prevention). One canary write, not two.
  LIBC_INLINE static void harden_slot(void *slot, SlabHeader *slab) {
    check_canary(slot, slab);
    __builtin_memset(slot, 0, slab->slot_size);
    set_canary(slot, slab);
  }

  // Canary-only hardening: no memset. Used when this free will seal the
  // page (PAGE_NOACCESS) — no info leak window. The kernel zeros the
  // page on eventual recommit from the zero-page list.
  LIBC_INLINE static void harden_slot_seal(void *slot, SlabHeader *slab) {
    check_canary(slot, slab);
    set_canary(slot, slab);
  }

public:
  /// Derive slab header from any pointer within the slab.
  LIBC_INLINE static SlabHeader *ptr_to_slab(void *ptr) {
    return reinterpret_cast<SlabHeader *>(
        reinterpret_cast<uintptr_t>(ptr) &
        ~static_cast<uintptr_t>(kSlabBytes - 1));
  }

  /// Encode a freelist next pointer (public for posix_alloc thread cache).
  LIBC_INLINE static SlabFreeNode *
  encode_free_next(SlabFreeNode *ptr, uintptr_t cookie,
                   SlabFreeNode **location) {
    return encode_next(ptr, cookie, location);
  }

  /// Decode a freelist next pointer (public for posix_alloc thread cache).
  LIBC_INLINE static SlabFreeNode *
  decode_free_next(SlabFreeNode *encoded, uintptr_t cookie,
                   SlabFreeNode **location) {
    return decode_next(encoded, cookie, location);
  }

  /// Read thread ID from TEB without syscall (public for posix_alloc).
  LIBC_INLINE static uint32_t get_current_tid() { return current_tid(); }

  /// Compute per-slot canary (public for posix_alloc alloc-time verification).
  LIBC_INLINE static uintptr_t make_canary(SlabHeader *slab, void *slot) {
    return alloc_primitives::derive_canary(slab->canary_key, slot);
  }

  /// Push an encoded chain of `count` nodes [head..tail] onto xthread_free
  /// with one CAS. Public for posix_alloc batched drain. Applies the same
  /// reservation + post-push-tid-recheck + last-freer-retire protocol as
  /// per-slot xthread free, but accounts for `count` slots in one
  /// fetch_add.
  LIBC_INLINE static void push_chain_xthread(SlabHeader *slab,
                                              SlabFreeNode *head,
                                              SlabFreeNode *tail,
                                              uint16_t count) {
    auto *pool = slab->pool;
    pool->retire_reservation_acquire();

    // Pre-push snapshot — compared against post_state below to gate the
    // post-seal contribution against a transition during our chain
    // push. See the rationale on the (seq, tid) snapshot check in
    // `free()`'s xthread branch.
    uint64_t pre_state = slab->state();
    xthread_push_chain(slab, head, tail);

    uint64_t post_state = slab->state();
    uint32_t tid_post = static_cast<uint32_t>(post_state);
    if (LIBC_UNLIKELY(tid_post == 0 && pre_state == post_state &&
                      count != 0)) {
      uint16_t old_count = slab->xthread_returned_count.fetch_add(
          count, cpp::MemoryOrder::ACQ_REL);
      uint16_t new_count = static_cast<uint16_t>(old_count + count);
      if (LIBC_LIKELY(new_count !=
                      slab->sealed_target.load(cpp::MemoryOrder::RELAXED))) {
        pool->retire_reservation_drop();
        return;
      }
      // Last-freer retire-claim, cycle-tagged. Expected value is
      // (pre_seq, kRetireIdle) — pre_seq came from the high 32 of
      // pre_state, captured before xthread_push_chain. If between our
      // pre/post snapshot match and this CAS the slab transitioned
      // through a full adopt + re-abandon cycle, retire_initiated
      // carries the new cycle's seq and our CAS observes a seq
      // mismatch (low byte alone is again kRetireIdle in the new
      // cycle, which is exactly the residual hazard the cycle tag
      // closes). On any failure (kRetireAdopting, kRetireDone, or
      // matching state byte but mismatched seq) we cleanly fall
      // through; our slot is already in the xthread queue and will
      // be drained by the next adopter.
      uint32_t pre_seq = static_cast<uint32_t>(pre_state >> 32);
      uint32_t expected =
          SlabHeader::pack_retire(pre_seq, SlabHeader::kRetireIdle);
      uint32_t desired =
          SlabHeader::pack_retire(pre_seq, SlabHeader::kRetireDone);
      if (LIBC_LIKELY(slab->retire_initiated.compare_exchange_strong(
              expected, desired, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::RELAXED))) {
        pool->pop_abandoned_specific(slab);
        pool->slab_retire_domain_.init_node(slab);
        pool->slab_retire_domain_.retire(slab);
      }
    }

    pool->retire_reservation_drop();
  }

  /// Apply hardening (canary check + zero + canary place) to a slot.
  /// Public for posix_alloc which manages its own thread cache.
  LIBC_INLINE static void harden_freed_slot(void *slot, SlabHeader *slab) {
    harden_slot(slot, slab);
  }

  /// Validate a slot pointer: range, slab authenticity, alignment,
  /// double-free. All four checks always on — total ~7 cycles.
  ///
  /// Returns the precomputed byte_offset (slot addr minus slot region
  /// start) so callers reuse it for page indices and bitmap index,
  /// avoiding a redundant subtraction on the free hot path.
  ///
  /// Check breakdown:
  ///   1. Range: single unsigned compare (~1 cy).
  ///   2. Slab tag: XOR + compare (~2 cy). Replaces 4 field-consistency
  ///      checks (pool, slot_size, slots_per_slab, bump/returned) with
  ///      one unforgeable validation. Catches non-slab pointers,
  ///      corrupted headers, use-after-release, cross-pool misrouting.
  ///   3. Alignment: reciprocal round-trip, MUL + compare (~3 cy).
  ///      Replaces runtime div (20-90 cy) with verify-by-reconstruction.
  ///   4. Double-free: bitmap RELAXED load + shift + test (~1 cy).
  LIBC_INLINE static size_t validate_slot(void *slot, SlabHeader *slab) {
    uintptr_t base = reinterpret_cast<uintptr_t>(slab);

    // 1. Range: single unsigned compare against compile-time constant.
    //    Produces byte_offset for caller reuse (page indices, bitmap).
    size_t byte_offset = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(slot) - base - kSlotStartOffset);
    if (LIBC_UNLIKELY(byte_offset >= kUsableBytes))
      __builtin_trap();

    // 2. Slab authenticity: global_secret ^ slab_base must match the
    //    tag written at init. One XOR + one compare, on cache line 1
    //    (already fetched for slot_size_recip).
    if (LIBC_UNLIKELY(slab->slab_tag !=
                      (slab_registry.slab_secret_ ^ base)))
      __builtin_trap();

    // 3. Alignment: idx * slot_size must exactly reconstruct byte_offset.
    //    The reciprocal multiply truncates, so a mid-slot pointer
    //    computes the wrong idx whose round-trip won't match. One MUL
    //    (~3 cy) replaces the old modular div (~20-90 cy).
    unsigned idx = slot_index_from_offset(slab, byte_offset);
    if (LIBC_UNLIKELY(static_cast<size_t>(idx) * slab->slot_size !=
                      byte_offset))
      __builtin_trap();

    // 4. Double-free: occupancy bit must be set for a live slot.
    //    One RELAXED load — plain MOV on x86/AArch64.
    if (LIBC_UNLIKELY(!slab->occupancy.is_live(idx)))
      __builtin_trap();

    // Debug-only: full structural integrity (pool/slots_per_slab/bump/
    // returned/class consistency). Redundant with the tag check for
    // corruption detection, but useful for pinpointing which field broke.
    LIBC_ASSERT(slab->pool && "null pool backpointer");
    LIBC_ASSERT(slab->slot_size >= sizeof(SlabFreeNode) &&
                slab->slot_size <= kUsableBytes && "slot_size out of range");
    LIBC_ASSERT(slab->slots_per_slab > 0 &&
                static_cast<size_t>(slab->slots_per_slab) * slab->slot_size
                    <= kUsableBytes && "slots_per_slab overflow");
    LIBC_ASSERT(slab->bump <= slab->slots_per_slab &&
                "bump exceeds slot count");
    LIBC_ASSERT(slab->pool->class_index_ == slab->class_index &&
                "cross-pool class mismatch");

    return byte_offset;
  }

  /// Count live (allocated) slots in a slab via occupancy bitmap.
  /// Walks only bitmap_words(slab) backing words — trailing words for
  /// slots_per_slab < kMaxSlots are always zero (init_slab clears, and
  /// writes never reach indices >= slots_per_slab), but scanning them
  /// would add up to 832 bytes of loads on tiny-slab configurations.
  LIBC_INLINE static unsigned popcount_live(SlabHeader *slab) {
    unsigned count = 0;
    unsigned words = bitmap_words(slab);
    for (unsigned w = 0; w < words; w++)
      count += static_cast<unsigned>(__builtin_popcountll(
          slab->occupancy.template word_at<cpp::MemoryOrder::RELAXED>(w)));
    return count;
  }

  /// Verify bitmap consistency after drain_xthread(). Two checks:
  ///   1. Total popcount must equal (bump - returned).
  ///   2. Every slot on local_free must have its bit CLEAR.
  /// Compiled out in release (LIBC_ASSERT). Catches both missing
  /// set/clear calls and swapped-bit corruption that total-only
  /// checking would miss.
  LIBC_INLINE static void assert_bitmap_consistent(
      [[maybe_unused]] SlabHeader *slab) {
#ifndef NDEBUG
    // Check 1: totals.
    LIBC_ASSERT(popcount_live(slab) ==
                    static_cast<unsigned>(slab->bump - slab->returned) &&
                "occupancy bitmap popcount diverged from alloc/free count");
    // Check 2: every free slot has its bit clear.
    uintptr_t ck = slab->freelist_cookie;
    SlabFreeNode *node = slab->local_free;
    while (node) {
      unsigned idx = slot_index(slab, node);
      LIBC_ASSERT(!slab->occupancy.is_live(idx) &&
                  "free slot has occupancy bit set");
      // Unseal if needed to read next pointer (sealed pages are
      // PAGE_NOACCESS). Only needed for the debug walk.
      ensure_slot_accessible(slab, node);
      node = decode_next(node->next, ck, &node->next);
    }
#endif
  }

  /// Callback type for for_each_live. Function pointer + opaque context,
  /// matching the ReactorCallback pattern used throughout the codebase.
  /// No templates — avoids code bloat from the bitmap scan loop.
  using SlabSlotCallback = void (*)(void *slot, void *ctx);

  /// Iterate all live slots in a slab. Scans the occupancy bitmap with
  /// tzcnt/blsr — skips 64 dead slots per word in a single instruction.
  ///
  /// The bitmap is owner-only. Cross-thread frees are not reflected
  /// until drain_xthread() runs. Callers that need an exact view
  /// should drain first (fork_reinit, compaction, diagnostics).
  ///
  /// Early-terminates when all live slots have been visited (avoids
  /// scanning trailing empty bitmap words on sparse slabs).
  /// Prefetches the next slot's cache line while processing the
  /// current one to hide memory latency on the scattered access pattern.
  LIBC_INLINE static void for_each_live(SlabHeader *slab,
                                         SlabSlotCallback cb, void *ctx) {
    unsigned words = bitmap_words(slab);
    unsigned remaining = static_cast<unsigned>(slab->bump - slab->returned);
    for (unsigned w = 0; w < words && remaining > 0; w++) {
      uint64_t bits =
          slab->occupancy.template word_at<cpp::MemoryOrder::RELAXED>(w);
      while (bits) {
        unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
        unsigned idx = w * 64 + bit;
        // Prefetch next live slot while processing current one.
        // The bitmap-driven access pattern is scattered — the hardware
        // prefetcher can't predict it. Locality hint 3 (L1 temporal):
        // the callback will read the slot immediately, so bring data
        // all the way to L1 for minimum-latency access.
        uint64_t next_bits = bits & (bits - 1);
        if (LIBC_LIKELY(next_bits != 0)) {
          unsigned next_bit = static_cast<unsigned>(__builtin_ctzll(next_bits));
          __builtin_prefetch(slab_slot(slab, w * 64 + next_bit), 0, 3);
        }
        cb(static_cast<void *>(slab_slot(slab, idx)), ctx);
        bits = next_bits;
        if (--remaining == 0)
          return;
      }
    }
  }

  /// Walk all live slots across every slab owned by this pool.
  /// Acquires the all-slabs spinlock for the duration of the walk.
  /// The callback may return early by setting *ctx to a sentinel —
  /// use a FindResult pattern (see named_semaphore.cpp for example).
  LIBC_INLINE void for_each_live_all(SlabSlotCallback cb, void *ctx) {
    lock_all_slabs();
    SlabHeader *slab = all_slabs_head_;
    while (slab) {
      for_each_live(slab, cb, ctx);
      slab = slab->all_next;
    }
    unlock_all_slabs();
  }

  // TLS cleanup callback for pool-managed TLS. The slab's pool
  // backpointer routes the abandon to the correct pool instance.
  static void fls_abandon_callback(PVOID val) {
    auto *slab = static_cast<SlabHeader *>(val);
    if (slab && slab->pool)
      slab->pool->abandon(slab);
  }

  /// Stamp a Crystalline-W reservation on the slab retire domain — pins
  /// every slab whose retire was published in the current era. Cross-
  /// thread freers and abandoned-chain walkers MUST bracket their slab
  /// dereferences with `retire_reservation_acquire` ... drop so a
  /// concurrent last-freer's retire cannot run substrate_release while
  /// the dereference is in flight. Drop is a second `anchor()` which
  /// re-publishes the slot's era (cheaper than `clear_all`'s full walk).
  ///
  /// Both endpoints publish era only; the slab pointer the caller is
  /// dereferencing is held alive by the caller's existing pin path
  /// (slab_registry membership for cross-thread free, the abandoned
  /// chain link for the chain walker).
  LIBC_INLINE void retire_reservation_acquire() {
    slab_retire_domain_.anchor(0);
  }
  LIBC_INLINE void retire_reservation_drop() {
    slab_retire_domain_.anchor(0);
  }

  /// Initialize with a given slot size. Thread-safe and idempotent —
  /// concurrent calls with the same arguments are safe (second caller
  /// spins until the first finishes).
  LIBC_INLINE void init(size_t slot_size, size_t slot_align = sizeof(void *),
                        uint8_t class_index = 0) {
    if (!init_latch_.try_begin()) {
      // Another thread is initializing or already done. wait_ready
      // encapsulates the hardware-monitor spin; returns immediately when
      // state is already READY (first ACQUIRE load short-circuits).
      init_latch_.wait_ready();
      return;
    }
    // Self-install the SHARED slab retire domain into the global
    // Crystalline registry on the first init() across all instances.
    // Must complete-happen-before any cross-thread free can call
    // retire/protect on this domain. Tier A bring-up is
    // single-threaded by contract; the latch makes this safe even if
    // a future bring-up path interleaves pool inits across threads.
    if (slab_retire_domain_init_latch_.try_begin()) {
      slab_retire_domain_.init_registration();
      slab_retire_domain_init_latch_.publish_ready();
    } else {
      slab_retire_domain_init_latch_.wait_ready();
    }

    // Slab layout is compiled against kPageSize. Trap unconditionally if
    // the runtime page size differs — guard/commit math silently corrupts.
    // kSlabBytes must also be an integer multiple of the NT allocation
    // granularity (64 KB on x64); the substrate's per-class slots are
    // already aligned to multiples of the granule, so this check just
    // guards against a misconfigured kSlabBytes parameter.
    // Cannot be static_assert (runtime OS query); must survive NDEBUG.
    const size_t alloc_gran =
        ::LIBC_NAMESPACE::windows::get_alloc_granularity();
    if (LIBC_UNLIKELY(::LIBC_NAMESPACE::windows::get_page_size() !=
                          kPageSize ||
                      alloc_gran == 0 ||
                      (kSlabBytes % alloc_gran) != 0))
      __builtin_trap();

    // xthread_free uses intra-slab offsets (16-bit) rather than absolute
    // pointers, so it is independent of VA width. The only constraint is
    // that slabs are exactly kSlabBytes (64KB), which is validated above.
    // Slab-base reconstruction uses `slab_base | offset`, which is correct
    // for all 64KB-aligned bases regardless of VA size.

    // Validate slot_size: must support freelist linkage (>= 8 bytes),
    // fit in the uint32_t header field, and produce a slots_per_slab
    // that fits in the uint16_t header field. Upper bound is
    // kUsableBytes so a single slot can't overflow the arena body.
    // A too-small size corrupts freelists; a too-large one overflows
    // the per-slab counters and silently wraps.
    if (LIBC_UNLIKELY(slot_size < sizeof(SlabFreeNode) ||
                      slot_size > kUsableBytes ||
                      kUsableBytes / slot_size == 0 ||
                      kUsableBytes / slot_size > 65535))
      __builtin_trap();

    // Validate alignment: kSlotStartOffset (8192) is page-aligned, so all
    // power-of-2 alignments up to 4096 are naturally satisfied. Slot size
    // must also be a multiple of the alignment to maintain alignment across
    // consecutive slots within a slab.
    if (LIBC_UNLIKELY(slot_align > kSlotStartOffset ||
                      (slot_align & (slot_align - 1)) != 0 ||
                      (slot_size % slot_align) != 0))
      __builtin_trap();

    slot_size_ = static_cast<uint16_t>(slot_size);
    slots_per_slab_ = static_cast<uint16_t>(kUsableBytes / slot_size);
    class_index_ = class_index;
    // Publishes the above assignments via the latch's RELEASE store.
    init_latch_.publish_ready();
  }

  /// Enable pool-managed TLS. Allocates a TEB TLS slot for
  /// single-instruction hot path reads + .CRT$XLC cleanup.
  /// Thread-safe and idempotent.
  ///
  /// `phase` is the cleanup phase (see tls_cleanup_state.h). Pools that
  /// back Phase-4 lifecycle objects (start_args_pool, g_bucket_entry_pool,
  /// the lifecycle pool itself) MUST pass kTlsCleanupPhaseAllocator (=2)
  /// or lower so their slab-abandon callback runs AFTER lifecycle_cleanup
  /// has finished retiring nodes that live in this slab. Higher-tier pools
  /// (named semaphores, file descriptors, etc.) also default to Allocator;
  /// nothing in the pool's own contract requires a specific phase.
  LIBC_INLINE void
  init_tls(uint8_t phase = internal::kTlsCleanupPhaseAllocator) {
    if (!tls_init_latch_.try_begin()) {
      tls_init_latch_.wait_ready();
      return;
    }
    tls_index_ = internal::tls_alloc();
    if (tls_index_ != TLS_OUT_OF_INDEXES)
      internal::tls_cleanup_register(tls_index_, fls_abandon_callback,
                                      phase);
    tls_init_latch_.publish_ready();
  }

  /// Reverse of init_tls(): unregister the cleanup callback and free the
  /// TEB TLS slot. Caller must guarantee no thread will read the slot
  /// after this returns (subsystem fini is the only safe site). Idempotent.
  LIBC_INLINE void fini_tls() {
    if (tls_index_ == TLS_OUT_OF_INDEXES)
      return;
    internal::tls_cleanup_unregister(tls_index_);
    internal::tls_free(tls_index_);
    tls_index_ = TLS_OUT_OF_INDEXES;
  }

  /// Allocate using pool-managed TLS (call init_tls() first).
  /// Reads the thread's slab from TEB, allocates, updates TLS on slow path.
  [[nodiscard]] LIBC_INLINE void *tls_alloc() {
    ThreadSlab slab = tls_get_slab();
    void *slot = alloc(slab);
    if (!slot) {
      slot = alloc_slow(&slab);
      if (slot)
        tls_set_slab(slab);
    }
    return slot;
  }

  /// Release every slab still owned by this pool back to the substrate.
  /// Called from the pool's fini handler at $P4 (substrate is $P1, so
  /// fini's reverse-phase order guarantees substrate is still up when
  /// we hit it). Walks `all_slabs_head_` and hands each slab's substrate
  /// token back via `substrate_release`; the substrate decommits the
  /// slot pages and updates its own arena bookkeeping.
  LIBC_INLINE void destroy() {
    lock_all_slabs();
    SlabHeader *slab = all_slabs_head_;
    all_slabs_head_ = nullptr;
    unlock_all_slabs();
    while (slab) {
      SlabHeader *next = slab->all_next;
      slab_registry.remove_range(reinterpret_cast<uintptr_t>(slab), kSlabBytes);
      uint64_t token = slab->substrate_token;
      ::LIBC_NAMESPACE::windows::alloc::substrate_release(
          ::LIBC_NAMESPACE::windows::alloc::SubSlotHandle::from_detached(
              slab, token));
      slab = next;
    }
  }

  /// Allocate from the thread's current slab (fast path).
  /// Returns nullptr if slab is null or exhausted — call alloc_slow().
  [[nodiscard]] LIBC_INLINE static void *alloc(ThreadSlab slab) {
    if (!slab)
      return nullptr;
    return alloc_from_slab(slab);
  }

  // Push a slab onto the abandoned stack (Treiber stack push).
  //
  // Pure Treiber CAS: write slab->next_abandoned first, then publish via
  // CAS. The chain is always fully linked from any consumer's view — a
  // concurrent exchange-steal never observes a partially-constructed
  // successor because the next pointer is set *before* the CAS succeeds.
  //
  // Unlike xthread_free (the per-slab cross-thread free queue), the
  // abandoned stack is MPMC: any alloc_slow caller can exchange-steal
  // the chain. A Vyukov-style queue would need a consumer lock to handle
  // multiple consumers, whereas Treiber's chain-complete-before-publish
  // invariant gives us MPMC correctness for free with exchange-steal.
  //
  // ABA safety: CAS-pop is never used on this stack — consumers only
  // call `exchange(nullptr)`. Treiber push is ABA-immune by construction
  // (the CAS only compares the head value and splices; an ABA'd head is
  // still a valid splice target, never a stale chain reference). No
  // generation tag needed.
  //
  // Contention cost: ~30-50 cycles per failed CAS. Under N-way push
  // contention the producer retries at most ~N times on average before
  // winning, so total push latency is ~N × 50 cycles ≈ sub-microsecond
  // even at 100-way contention. Abandoned-stack pushes are rare anyway
  // (per-thread on exit, or per-failed-adoption in alloc_slow).
  LIBC_INLINE void push_abandoned(SlabHeader *slab) {
    SlabHeader *old = abandoned_head_.load(cpp::MemoryOrder::RELAXED);
    for (;;) {
      slab->next_abandoned = old;
      if (abandoned_head_.compare_exchange_weak(
              old, slab, cpp::MemoryOrder::RELEASE,
              cpp::MemoryOrder::RELAXED))
        return;
      // Failed CAS updates `old` with the current head value; retry
      // immediately with the fresh value. No pause, no backoff — the
      // failed CAS itself is the forward-progress step.
    }
  }

  // Push an already-linked chain [chain_head..chain_tail] onto the
  // abandoned stack in one CAS. Interior chain links were set by the
  // caller; we only need to splice chain_tail->next_abandoned onto the
  // current head and CAS chain_head in.
  LIBC_INLINE void push_abandoned_chain(SlabHeader *chain_head) {
    if (!chain_head)
      return;
    SlabHeader *chain_tail = chain_head;
    while (chain_tail->next_abandoned)
      chain_tail = chain_tail->next_abandoned;
    SlabHeader *old = abandoned_head_.load(cpp::MemoryOrder::RELAXED);
    for (;;) {
      chain_tail->next_abandoned = old;
      if (abandoned_head_.compare_exchange_weak(
              old, chain_head, cpp::MemoryOrder::RELEASE,
              cpp::MemoryOrder::RELAXED))
        return;
    }
  }

  /// Slow path: try adopting an abandoned slab, then allocate fresh.
  /// Updates *current_slab for future fast-path allocs.
  [[nodiscard]] LIBC_INLINE void *alloc_slow(ThreadSlab *current_slab) {
    // 0. Try the current slab one more time (may have been emptied by
    // cross-thread frees we hadn't drained yet). alloc_from_slab itself
    // will compact-then-recommit at step 4 if the freelist is exhausted —
    // no need for a speculative compact_sealed_pages here (that walked
    // the freelist O(n) on every alloc_slow even when drain_xthread or
    // bump would yield a slot in O(1)).
    if (*current_slab) {
      void *slot = alloc_from_slab(*current_slab);
      if (slot)
        return slot;
    }

    // Capture the arena affinity hint and abandon the prior slab before
    // rotating. Without this, the prior slab is silently dropped from
    // *current_slab on adoption (step 1) or fresh-acquire (step 2): its
    // tid_owner stays set, owner-side drain_xthread / seal+decommit never
    // re-fire, and the slab leaks until process exit. abandon() drains
    // pending xthread frees, transitions tid -> 0, and either retires
    // (empty) or pushes onto abandoned_head_ for adoption — the same
    // protocol the FLS thread-exit callback uses, just triggered on
    // rotation instead. The arena hint must be captured BEFORE abandon
    // because empty-at-abandon retires through Crystalline and the slab's
    // VA may be queued for substrate decommit; arena_of is pure pointer
    // arithmetic on the slab base (no deref) so it stays valid, but
    // capturing first keeps the order obvious.
    SlabHeader *prior = *current_slab;
    ::LIBC_NAMESPACE::windows::alloc::ArenaHeader *hint =
        prior ? ::LIBC_NAMESPACE::windows::alloc::arena_of(prior,
                                                           kSubstrateClass)
              : nullptr;
    if (prior) {
      abandon(prior);
      *current_slab = nullptr;
    }

    // 1. Try to adopt abandoned slabs.
    //
    // Exchange-steal: atomically take the entire abandoned chain into
    // local ownership. Because retired slabs no longer linger on the
    // chain (they self-retire via Crystalline at threshold), the chain
    // contains only genuinely-non-empty sealed slabs plus the rare race
    // case of a slab whose retire_initiated has just won — that one is
    // skipped explicitly below. The kMaxAdoptAttempts cap that previously
    // bounded this walk is dropped: empties no longer accumulate.
    //
    // Reservation bracket: the walk dereferences slab->next_abandoned
    // and slab body fields. Without a Crystalline pin, a concurrent
    // last-freer's retire could fire slab_release_callback (and the
    // substrate decommit inside it) on a slab we're still walking.
    // retire_reservation_acquire/drop closes that window — Crystalline
    // waits for our reservation to drain before firing FreeFn.
    //
    // Probe-then-bracket: a RELAXED load of abandoned_head_ tests for an
    // empty chain before paying for the reservation pair. If empty, we
    // skip straight to fresh-slab allocation. TOCTOU window between the
    // probe and the eventual exchange is benign: a concurrent push that
    // lands in the gap is missed for adoption this round, and the fresh
    // slab we allocate below absorbs the workload — correctness-equivalent
    // (the abandoned slab will be picked up on the next alloc_slow call).
    if (abandoned_head_.load(cpp::MemoryOrder::RELAXED) != nullptr) {
      retire_reservation_acquire();
      {
        SlabHeader *chain =
            abandoned_head_.exchange(nullptr, cpp::MemoryOrder::ACQUIRE);

        while (chain) {
          // Detach head from chain — we own this node locally, no races.
          SlabHeader *next = chain->next_abandoned;
          chain->next_abandoned = nullptr;

          // Take the adoption transition lock by CAS-ing the
          // retire_initiated low byte from kRetireIdle to
          // kRetireAdopting while preserving the high-24 cycle seq.
          // The cycle tag is owned by whichever transition last
          // published kRetireIdle (original abandon or a prior
          // re-abandon); we do not advance it here — adopter's claim()
          // / re-abandon's release() handle that on the matching
          // retire_initiated stamp.
          //
          // Closes the race in which an in-flight cross-thread freer
          // (still observing the prior cycle's tid==0) would otherwise
          // win `retire_initiated kRetireIdle → kRetireDone` against
          // our stale sealed_target / xthread_returned_count and
          // trigger Crystalline retire on a slab we are about to
          // claim. The freer's CAS expects (pre_seq, kRetireIdle); our
          // successful CAS publishes (same_seq, kRetireAdopting) so
          // their CAS sees the byte change and fails. CAS-loop because
          // a concurrent stale-cycle freer might first transition the
          // byte to kRetireDone — observing anything other than
          // kRetireIdle in the byte means we must not touch the slab;
          // skip.
          uint32_t cur = chain->retire_initiated.load(
              cpp::MemoryOrder::ACQUIRE);
          bool got_lock = false;
          while (SlabHeader::unpack_retire_state(cur) ==
                 SlabHeader::kRetireIdle) {
            uint32_t desired = (cur & 0xFFFFFF00u) |
                               static_cast<uint32_t>(SlabHeader::kRetireAdopting);
            if (chain->retire_initiated.compare_exchange_weak(
                    cur, desired, cpp::MemoryOrder::ACQ_REL,
                    cpp::MemoryOrder::ACQUIRE)) {
              got_lock = true;
              break;
            }
          }
          if (LIBC_UNLIKELY(!got_lock)) {
            chain = next;
            continue;
          }

          // We hold the lock. Resetting seal-state is safe under the lock
          // (any racing freer's retire CAS sees kRetireAdopting → fails);
          // the subsequent RELEASE-claim publishes them to the next ACQUIRE
          // of tid by a cross-thread freer — so a freer that observes our
          // new tid skips post-seal entirely, and a freer that still
          // observes 0 from the prior cycle is gated by the lock CAS.
          chain->sealed_target.store(0, cpp::MemoryOrder::RELAXED);
          chain->xthread_returned_count.store(0, cpp::MemoryOrder::RELAXED);
          chain->claim(current_tid()); // RELEASE — publishes resets

          // Drain cross-thread returns.
          drain_xthread(chain);

          // Epoch check: if fully empty after drain, retire via
          // Crystalline (mirrors empty-at-abandon path). The lock is
          // currently held (kRetireAdopting); transitioning to
          // kRetireDone is unique to us, so a plain store is
          // sufficient. Stamp with the post-release seq so any stale
          // freer's CAS still mismatches even after we mark Done.
          if (chain->returned == chain->bump) {
            chain->release();
            chain->retire_initiated.store(
                SlabHeader::pack_retire(chain->state_seq(),
                                         SlabHeader::kRetireDone),
                cpp::MemoryOrder::RELAXED);
            slab_retire_domain_.init_node(chain);
            slab_retire_domain_.retire(chain);
            chain = next;
            continue;
          }

          // Try to allocate from this slab.
          void *slot = alloc_from_slab(chain);
          if (slot) {
            *current_slab = chain;
            // Push remaining un-examined chain back.
            push_abandoned_chain(next);
            // Slab is now alive (tid != 0). The lock stays held
            // (kRetireAdopting) for the entire alive lifetime — any
            // delayed cross-thread freer that still observes the prior
            // cycle's tid==0 is gated by the lock CAS. The lock is
            // released back to kRetireIdle only at the next abandon
            // (re-seal path below or via SlabPoolT::abandon), and that
            // release is paired with the abandon's release()-tid=0 which
            // republishes a fresh sealed_target / count to subsequent
            // freers.
            retire_reservation_drop();
            return slot;
          }

          // Slab fully consumed — re-seal as alive→sealed and re-abandon.
          // Order matters: set new (target, count) FIRST, then stamp
          // retire_initiated with the about-to-be post-release seq
          // (state_seq()+1), then release(). The retire_initiated
          // stamp lands BEFORE release() so any cross-thread freer
          // that arrives in the post-release window observes a
          // consistent state_=(new_seq, 0) AND retire_initiated=
          // (new_seq, kRetireIdle) pair, and a real freer of the new
          // cycle can succeed its last-freer CAS. Stamping AFTER
          // release would open a window where the freer's CAS expects
          // (new_seq, kRetireIdle) but retire_initiated still carries
          // the lock-cycle (current_seq, kRetireAdopting), causing
          // even the legitimate last-freer to fail and the slab to
          // never auto-retire.
          chain->sealed_target.store(
              static_cast<uint16_t>(chain->bump - chain->returned),
              cpp::MemoryOrder::RELAXED);
          chain->xthread_returned_count.store(0,
                                              cpp::MemoryOrder::RELAXED);
          chain->retire_initiated.store(
              SlabHeader::pack_retire(chain->state_seq() + 1,
                                       SlabHeader::kRetireIdle),
              cpp::MemoryOrder::RELEASE);
          chain->release();
          push_abandoned(chain);
          chain = next;
        }
      }
      retire_reservation_drop();
    }

    // 2. Allocate a fresh slab from the substrate. Each slab body is
    // backed by exactly one substrate slot (Medium for default 64 KB
    // pools, Huge for 1 MB L3 pools). The substrate handles VA
    // reservation, arena placement, mapping-table registration, and
    // generation-tagged release on `full_release` / `destroy`.
    //
    // Affinity hint (captured at top of alloc_slow before abandon): if
    // we had a prior current_slab, its arena is the right place to ask
    // the substrate for the next slab — sibling slabs in the same arena
    // pack the substrate's L1 bitmap densely and preserve TLB locality.
    // The substrate re-validates the hint internally; a stale or
    // quarantined arena (including one whose slot we just retired)
    // silently falls through to the active arena.
    auto handle =
        ::LIBC_NAMESPACE::windows::alloc::substrate_acquire_uncommitted(
            kSubstrateClass,
            ::LIBC_NAMESPACE::windows::alloc::ConsumerTag::SlabPool, hint);
    if (!handle)
      return nullptr; // VA exhaustion — unrecoverable upstream.

    auto *base = static_cast<char *>(handle.ptr());

    // Commit the header page and the body region in two ranges; leave the
    // leading and trailing guard pages decommitted (PAGE_NOACCESS) so an
    // overflow off either end of the body traps at the MMU. The substrate
    // released this slot in PAGE_NOACCESS state, so commit is what makes
    // the header / body readable; the guard pages keep the substrate's
    // default protection.
    if (!::LIBC_NAMESPACE::windows::alloc::substrate_commit_subrange(
            handle, /*offset=*/0, kPageSize)) {
      ::LIBC_NAMESPACE::windows::alloc::substrate_release(handle);
      return nullptr;
    }
    if (!::LIBC_NAMESPACE::windows::alloc::substrate_commit_subrange(
            handle, kBodyOffset, kBodySize)) {
      ::LIBC_NAMESPACE::windows::alloc::substrate_release(handle);
      return nullptr;
    }
    ::LIBC_NAMESPACE::windows::alloc::substrate_decommit_subrange(
        handle, kLeadingGuardOffset, kPageSize);
    ::LIBC_NAMESPACE::windows::alloc::substrate_decommit_subrange(
        handle, kTrailingGuardOffset, kPageSize);

    auto *slab = reinterpret_cast<SlabHeader *>(base);
    slab->substrate_token = handle.token();
    (void)handle.detach_ptr(); // ownership transferred to slab->substrate_token
    init_slab(slab);
    all_slabs_insert(slab);
    slab_registry.insert_range(reinterpret_cast<uintptr_t>(slab), kSlabBytes);

    *current_slab = slab;
    return alloc_from_slab(slab);
  }

  /// Free a slot. Zeros, checks canary, routes to local_free (owner) or
  /// xthread (non-owner). Epoch-based: never triggers slab release.
  /// Seals the slot's page if all slots on it are now freed.
  LIBC_INLINE static void free(void *slot) {
    if (!slot)
      return;

    SlabHeader *slab = ptr_to_slab(slot);
    // validate_slot returns precomputed byte_offset — reuse it for
    // page indices and bitmap index (one SUB saved vs recomputing).
    size_t byte_offset = validate_slot(slot, slab);

    if (slab->tid() == current_tid()) {
      unsigned first_pg = static_cast<unsigned>(byte_offset >> 12);
      unsigned last_pg = static_cast<unsigned>(
          (byte_offset + slab->slot_size - 1) >> 12);

      // Will-seal check: if ALL spanned pages have occupancy 1, this free
      // empties them all. Skip memset — kernel zeros on recommit.
      bool will_seal = true;
      for (unsigned pg = first_pg; pg <= last_pg && pg < kSlotPages; pg++) {
        if (slab->page_occupancy[pg] != 1) {
          will_seal = false;
          break;
        }
      }

      if (will_seal)
        harden_slot_seal(slot, slab);
      else
        harden_slot(slot, slab);

      auto *node = static_cast<SlabFreeNode *>(slot);
      node->next = encode_next(slab->local_free, slab->freelist_cookie,
                               &node->next);
      slab->local_free = node;
      slab->returned++;

      // Bitmap clear using precomputed byte_offset.
      slab->occupancy.mark_dead(slot_index_from_offset(slab, byte_offset));

      // Fused vacate + seal using precomputed page indices
      // (avoids recomputing slot_page_index/slot_end_page_index).
      bool any_sealed = false;
      for (unsigned pg = first_pg; pg <= last_pg && pg < kSlotPages; pg++) {
        if (--slab->page_occupancy[pg] == 0) {
          seal_slot_page(slab, pg);
          any_sealed = true;
        }
      }
      // Opportunistic compaction when half the slab's pages are sealed.
      // Without this, slabs in steady-state alloc/free churn that never
      // saturate their bump pointer accumulate SEALED-but-resident pages
      // — the only existing decommit path is alloc_slow's bump-exhaust
      // branch, which never fires under churn. compact unseals freelist
      // nodes on sealed pages, removes them from local_free, then
      // decommits — physical RSS is reclaimed while VA + UAF protection
      // are preserved.
      if (any_sealed && count_sealed_pages(slab) >= kCompactThreshold)
        compact_sealed_pages(slab);
    } else {
      // Non-owner. Stamp a Crystalline reservation BEFORE any further
      // slab access — protects against a concurrent last-freer's retire
      // that would otherwise let substrate_release decommit the slab
      // pages while we're still writing predecessor->next inside
      // xthread_push.
      //
      // pool is hoisted once: every reservation/domain access below
      // shares the same indirection, saving repeat loads through the
      // slab header.
      auto *pool = slab->pool;
      pool->retire_reservation_acquire();

      // Always full zero (can't read owner-only counter).
      // Bitmap bit stays set — cleared when owner drains xthread.
      harden_slot(slot, slab);

      // Capture the (seq, tid) snapshot BEFORE push. Compared against
      // the post-push snapshot below to detect any ownership transition
      // that interleaved with our work — see the comment block on
      // `state_` and the rationale beneath the post-push check.
      uint64_t pre_state = slab->state();
      xthread_push(slab, static_cast<SlabFreeNode *>(slot));

      // Re-snapshot AFTER the push. ACQUIRE pairs with the owner /
      // adopter / re-abandon RELEASE-stores on `state_`. We enter the
      // post-seal contribution path ONLY when:
      //   (i)  pre_state == post_state — no transition occurred during
      //        my push window. If the seq advanced, an abandon (and
      //        possibly an adoption) ran during my push; my slot may
      //        already have been pulled into `returned` by their
      //        drain, so a fetch_add here would be a phantom +1
      //        against the new cycle's freshly-reset count and could
      //        prematurely fire last-freer retire while real
      //        outstanding slots remain in app code (UAF on later
      //        free).
      //   (ii) low-32 of post_state == 0 — sealed cycle.
      // The snapshot equality check is the load-bearing race gate.
      // The sealed branch is rare relative to alive-state cross-thread
      // free — UNLIKELY pushes the cold path off the hot icache line.
      uint64_t post_state = slab->state();
      uint32_t tid_post = static_cast<uint32_t>(post_state);
      if (LIBC_UNLIKELY(tid_post == 0 && pre_state == post_state)) {
        // Post-seal cross-thread free: contribute to the retire count.
        // fetch_add returns the OLD value; (old + 1) == sealed_target
        // means we are the last freer.
        uint16_t old_count = slab->xthread_returned_count.fetch_add(
            1, cpp::MemoryOrder::ACQ_REL);
        if (LIBC_LIKELY(static_cast<uint16_t>(old_count + 1) !=
                        slab->sealed_target.load(
                            cpp::MemoryOrder::RELAXED))) {
          // Not the last freer — no further slab access from us. Drop
          // the reservation NOW so Crystalline can advance epoch sooner
          // (other threads' pending retires aren't pinned by us anymore).
          pool->retire_reservation_drop();
          return;
        }
        // Threshold crossed. Claim the retire via cycle-tagged CAS.
        // Expected = (pre_seq, kRetireIdle), desired = (pre_seq,
        // kRetireDone). Load-bearing race gates:
        //   - Concurrent adopter holds kRetireAdopting → byte
        //     mismatch → CAS fails.
        //   - Concurrent freer or fork-reinit raced and wrote
        //     kRetireDone → byte mismatch → CAS fails.
        //   - Adopter has gone full adopt + re-abandon between our
        //     pre/post snapshot and now → retire_initiated carries
        //     the new cycle's seq → seq mismatch → CAS fails. This
        //     is the residual sliver of the phantom-freer race that
        //     the cycle tag exists to close: the post-snapshot match
        //     no longer guarantees we are still in the same cycle by
        //     the time we reach this CAS, but the seq tag binds the
        //     CAS to the cycle our pre-snapshot belonged to.
        uint32_t pre_seq = static_cast<uint32_t>(pre_state >> 32);
        uint32_t expected =
            SlabHeader::pack_retire(pre_seq, SlabHeader::kRetireIdle);
        uint32_t desired =
            SlabHeader::pack_retire(pre_seq, SlabHeader::kRetireDone);
        if (LIBC_LIKELY(slab->retire_initiated.compare_exchange_strong(
                expected, desired, cpp::MemoryOrder::ACQ_REL,
                cpp::MemoryOrder::RELAXED))) {
          // Splice off abandoned_head_ before publishing to Crystalline
          // (best-effort — see pop_abandoned_specific contract). Order
          // matters: pop first so a concurrent alloc_slow walker doesn't
          // observe target as adoptable after our retire publication.
          pool->pop_abandoned_specific(slab);
          pool->slab_retire_domain_.init_node(slab);
          pool->slab_retire_domain_.retire(slab);
        }
      }

      // Drop the reservation. Crystalline waits for this drop before
      // firing slab_release_callback (substrate_release on the slab's
      // backing slot), so any in-flight predecessor-link write inside
      // xthread_push has already completed by the time decommit runs.
      pool->retire_reservation_drop();
    }
  }

  /// Return a slot that is ALREADY HARDENED (zeroed + canary written by
  /// the caller's harden_freed_slot) to its slab — either local_free
  /// (owner) or xthread (non-owner). No memset, no canary rewrite, no
  /// double-free check: the double-free guard is the caller's job and
  /// must have run before the slot entered any cache/bin/chain. The
  /// function name states the contract; violating it is a bug.
  ///
  /// Callers: slab_free direct path, drain_cache_bin overflow fallback,
  /// flush_cache_bins. All feed hardened slots.
  LIBC_INLINE static void return_hardened_slot(void *slot) {
    SlabHeader *slab = ptr_to_slab(slot);
    size_t byte_offset = validate_slot(slot, slab);

    // Precondition: slot must already carry a valid canary. Debug-only
    // — zero cost in release builds.
    LIBC_ASSERT(has_canary(slot, slab));

    auto *node = static_cast<SlabFreeNode *>(slot);
    uintptr_t ck = slab->freelist_cookie;

    if (slab->tid() == current_tid()) {
      node->next = encode_next(slab->local_free, ck, &node->next);
      slab->local_free = node;
      slab->returned++;
      // Bitmap clear + page ops using validate_slot's byte_offset.
      slab->occupancy.mark_dead(slot_index_from_offset(slab, byte_offset));
      unsigned first_pg = static_cast<unsigned>(byte_offset >> 12);
      unsigned last_pg = static_cast<unsigned>(
          (byte_offset + slab->slot_size - 1) >> 12);
      for (unsigned pg = first_pg; pg <= last_pg && pg < kSlotPages; pg++) {
        if (--slab->page_occupancy[pg] == 0)
          seal_slot_page(slab, pg);
      }
    } else {
      // Same Crystalline-protected protocol as `free()`'s xthread branch:
      // reservation bracket + post-push (seq, tid) snapshot recheck
      // gating the last-freer retire. See `free()` above for the full
      // rationale on the pre/post `state()` snapshot — this path
      // mirrors it exactly so phantom freer fetch_adds are rejected on
      // the batched-return code path too.
      auto *pool = slab->pool;
      pool->retire_reservation_acquire();

      // Capture pre-push snapshot.
      uint64_t pre_state = slab->state();
      // Bitmap bit stays set — cleared when owner drains xthread.
      xthread_push(slab, node);

      uint64_t post_state = slab->state();
      uint32_t tid_post = static_cast<uint32_t>(post_state);
      if (LIBC_UNLIKELY(tid_post == 0 && pre_state == post_state)) {
        uint16_t old_count = slab->xthread_returned_count.fetch_add(
            1, cpp::MemoryOrder::ACQ_REL);
        if (LIBC_LIKELY(static_cast<uint16_t>(old_count + 1) !=
                        slab->sealed_target.load(
                            cpp::MemoryOrder::RELAXED))) {
          pool->retire_reservation_drop();
          return;
        }
        // Cycle-tagged last-freer CAS — see `free()`'s xthread branch
        // for the full rationale on the (pre_seq, kRetireIdle) →
        // (pre_seq, kRetireDone) gate that closes the residual
        // phantom-freer race.
        uint32_t pre_seq = static_cast<uint32_t>(pre_state >> 32);
        uint32_t expected =
            SlabHeader::pack_retire(pre_seq, SlabHeader::kRetireIdle);
        uint32_t desired =
            SlabHeader::pack_retire(pre_seq, SlabHeader::kRetireDone);
        if (LIBC_LIKELY(slab->retire_initiated.compare_exchange_strong(
                expected, desired, cpp::MemoryOrder::ACQ_REL,
                cpp::MemoryOrder::RELAXED))) {
          pool->pop_abandoned_specific(slab);
          pool->slab_retire_domain_.init_node(slab);
          pool->slab_retire_domain_.retire(slab);
        }
      }

      pool->retire_reservation_drop();
    }
  }

  /// Prepare a slot for batch return during drain: clear its bitmap bit
  /// and decrement page occupancy without sealing. Pages stay accessible
  /// so the caller can continue walking freelist chain pointers.
  /// Precomputes byte_offset once for both bitmap and page operations.
  /// REQUIRES: slab->tid() == current_tid()
  LIBC_INLINE static void drain_prepare_slot(SlabHeader *slab, void *slot) {
    size_t byte_offset = static_cast<size_t>(
        static_cast<char *>(slot) -
        (reinterpret_cast<char *>(slab) + kSlotStartOffset));
    // Bitmap clear.
    slab->occupancy.mark_dead(slot_index_from_offset(slab, byte_offset));
    // Page occupancy decrement (no sealing).
    unsigned first_pg = static_cast<unsigned>(byte_offset >> 12);
    unsigned last_pg = static_cast<unsigned>(
        (byte_offset + slab->slot_size - 1) >> 12);
    for (unsigned pg = first_pg; pg <= last_pg && pg < kSlotPages; pg++)
      --slab->page_occupancy[pg];
  }

  /// Seal pages whose occupancy dropped to zero during a drain batch.
  /// Call after the chain walk completes and all nodes have been spliced
  /// into local_free. Returns physical memory from emptied pages promptly.
  /// Without a touched_mask, scans all slot-region pages (safe fallback
  /// for external drain paths that don't track per-page masks).
  /// REQUIRES: slab->tid() == current_tid()
  LIBC_INLINE static void seal_drained_pages(SlabHeader *slab) {
    // Full scan: set all slot-region page bits.
    seal_empty_pages_after_drain(slab, PageMask::all());
  }

  /// Abandon a slab (thread exit). Drains xthread, compacts, then either
  /// retires the slab to Crystalline (empty) or seals it for last-freer-
  /// triggered retire (live slots remain).
  ///
  /// The empty-at-abandon path goes through Crystalline rather than calling
  /// `full_release` directly so a concurrent abandoned-chain walker (which
  /// pinned the slab via `retire_reservation_acquire`) cannot fault on a
  /// substrate-decommitted slab — Crystalline waits for the walker's
  /// reservation to drain before firing `slab_release_callback`.
  ///
  /// The non-empty path RELEASE-stores tid=0 *after* setting sealed_target
  /// and zeroing the count; cross-thread freers' subsequent ACQUIRE-load
  /// of tid pairs with that store and observes the seal-time values.
  /// Pushes that landed in MPSC after our drain but before our release()
  /// are not in `returned` — their producers' post-push tid recheck will
  /// see 0 and contribute the missing fetch_add. (Their queue nodes are
  /// stranded slab-internal storage; the queue itself is never drained
  /// post-seal — the slab-wide retire decommits the entire body.)
  LIBC_INLINE void abandon(ThreadSlab slab) {
    if (!slab)
      return;

    // Drain pending xthread frees (updates returned + bitmap).
    drain_xthread(slab);
    assert_bitmap_consistent(slab);

    // Deliberately NOT calling compact_sealed_pages here. compact splices
    // sealed-page nodes out of `local_free` and decrements `returned` by
    // the count of nodes destroyed. In `alloc_from_slab` that is paired
    // with an immediate `recommit_page_slots` that re-pushes the same
    // slots and re-increments `returned`, leaving the (bump - returned)
    // outstanding-slot count invariant intact. In abandon there is no
    // matched recommit: a compact would inflate (bump - returned) by
    // `removed`, and the subsequent `sealed_target = bump - returned`
    // would expect cross-thread fetch_adds for slots that no longer
    // exist (their VA is decommitted; no app holds them). Last-freer
    // would never be reached, parking the slab on `abandoned_head_`
    // forever for never-self-retire. The next adopter's first
    // `alloc_from_slab` step 4 runs the matched compact+recommit pair,
    // so memory is returned at the next active use rather than at
    // abandon. The cost is sealed pages staying PAGE_NOACCESS-but-
    // committed across the abandoned window — physical memory only,
    // and only for slabs that wait for adoption.

    // Empty-at-abandon: hand directly to Crystalline. release() must
    // happen BEFORE the retire so any concurrent reader observes
    // tid==0 (and therefore a stable sealed_target) before they could
    // observe the retire publication. By construction returned==bump
    // implies every freed slot has already been drained — there is no
    // in-flight cross-thread freer that could race the retire CAS, so
    // a plain store of kRetireDone is sufficient.
    if (slab->returned == slab->bump) {
      slab->release();
      // Stamp retire_initiated with the post-release seq so a stale
      // freer arriving after release observes (current_seq, kRetireDone)
      // — their CAS expects (their_pre_seq, kRetireIdle) and fails on
      // both the byte AND the seq, regardless of which prior cycle
      // they belonged to.
      slab->retire_initiated.store(
          SlabHeader::pack_retire(slab->state_seq(), SlabHeader::kRetireDone),
          cpp::MemoryOrder::RELAXED);
      slab_retire_domain_.init_node(slab);
      slab_retire_domain_.retire(slab);
      return;
    }

    // Sealed-with-live-slots: configure last-freer-triggered retire.
    // Order: target / count first; then stamp retire_initiated with
    // the about-to-be post-release seq (state_seq() + 1); then
    // release(). Stamping BEFORE release ensures any cross-thread
    // freer that arrives in the post-release window observes a
    // consistent state_=(new_seq, 0) AND retire_initiated=(new_seq,
    // kRetireIdle) pair, so the legitimate last freer's
    // (new_seq, kRetireIdle)→(new_seq, kRetireDone) CAS can succeed.
    // Stamping AFTER release would expose a window where the freer's
    // CAS expects (new_seq, kRetireIdle) but retire_initiated still
    // carries the prior cycle's value, causing even the legitimate
    // last freer to fail and the slab to never auto-retire.
    slab->sealed_target.store(
        static_cast<uint16_t>(slab->bump - slab->returned),
        cpp::MemoryOrder::RELAXED);
    slab->xthread_returned_count.store(0, cpp::MemoryOrder::RELAXED);
    slab->retire_initiated.store(
        SlabHeader::pack_retire(slab->state_seq() + 1,
                                 SlabHeader::kRetireIdle),
        cpp::MemoryOrder::RELEASE);
    slab->release();  // RELEASE — pairs with cross-thread freer ACQUIRE
    push_abandoned(slab);
  }

  /// Targeted removal from the abandoned stack. Used only by the last-
  /// freer retire path — the `retire_initiated kRetireIdle →
  /// kRetireDone` CAS guarantees exactly one caller for a given target,
  /// so concurrent splices on the same target are structurally
  /// impossible.
  ///
  /// Exchange-pop-all + local splice + re-push. ABA-free by
  /// construction: the SWAP claims the entire chain into a thread-local
  /// snapshot, eliminating the read-then-CAS window in which a
  /// concurrent pop+re-push could leave a successful CAS writing a
  /// stale next-link. Mirrors `alloc_slow`'s adoption pattern and
  /// substrate's `ArenaPool::pop_abandoned_filtered`, so both consumers
  /// of the abandoned stack share one primitive shape.
  ///
  /// Soundness vs. concurrent adopters: only one thread can hold the
  /// chain at a time (exchange is atomic). If we win the SWAP, target
  /// is in our local view; we splice and re-push the rest. If an
  /// adopter won the SWAP first, our SWAP returns null; the adopter
  /// will encounter target during its walk, see retire_initiated ==
  /// kRetireDone, and drop it (without re-pushing). target reaches
  /// Crystalline via the caller's subsequent `retire(target)` call
  /// regardless of which thread cleared it from the stack.
  ///
  /// Caller MUST hold a `slab_retire_domain_` reservation for the
  /// duration of this call (and the surrounding free / retire window).
  /// The reservation defers Crystalline's `slab_release_callback` —
  /// without it, `target` (or any other slab on the chain) could be
  /// substrate-decommitted between our SWAP and our walk, producing
  /// an AV on `chain->next_abandoned`. Every existing caller satisfies
  /// this — see free()/return_hardened_slot()/push_chain_xthread().
  LIBC_INLINE void pop_abandoned_specific(SlabHeader *target) {
    if (!target)
      return;
    // SWAP-claim the whole chain. ABA-immune (no compare).
    SlabHeader *chain =
        abandoned_head_.exchange(nullptr, cpp::MemoryOrder::ACQ_REL);
    if (chain == nullptr)
      return; // Adopter (or another pop_abandoned_specific) raced and won.

    // Walk: splice out target if present; build a local "keep" chain of
    // everything else. Walking is on thread-local data (we own the
    // chain post-SWAP), so no atomics inside the loop.
    SlabHeader *keep_head = nullptr;
    SlabHeader *keep_tail = nullptr;
    while (chain != nullptr) {
      SlabHeader *next = chain->next_abandoned;
      chain->next_abandoned = nullptr;
      if (chain == target) {
        chain = next;
        continue; // splice out
      }
      if (keep_tail)
        keep_tail->next_abandoned = chain;
      else
        keep_head = chain;
      keep_tail = chain;
      chain = next;
    }

    if (keep_head)
      push_abandoned_chain(keep_head);
  }

  /// Release a slab if fully empty (returned == bump). For use by
  /// external drain paths that detect empty non-active slabs.
  /// Caller must be the owner thread.
  LIBC_INLINE void release_if_empty(SlabHeader *slab) {
    if (slab->returned == slab->bump)
      full_release(slab);
  }

  /// Reset after fork. Only the forking thread survives.
  /// Single-threaded post-fork: no concurrent threads exist, so atomic
  /// operations and locks are unnecessary. Claim all slabs under our TID
  /// before draining so bitmap_clear's owner assertion holds.
  LIBC_INLINE void fork_reinit() {
    uint32_t my_tid = current_tid();

    // Clear the abandoned stack — all dead-thread slabs are reachable
    // via all_slabs_head_ and will be processed below.
    abandoned_head_.store(nullptr, cpp::MemoryOrder::RELAXED);

    // Reset the spinlock (may have been held by a dead thread).
    all_slabs_lock_.store(0, cpp::MemoryOrder::RELAXED);

    // Walk all slabs. Single-threaded — no lock needed.
    //
    // Every dead-thread slab's xthread MPSC queue is in an unreliable
    // snapshot state (mid-push nodes may be stranded from threads that
    // didn't fork with us). Re-init the queue to empty before touching
    // the slab; any pending cross-thread frees are lost by construction
    // (the producing threads don't exist in the child process, so their
    // Phase 3 will never land). The stranded slots stay marked live in
    // the bitmap — equivalent to them having never been freed at all.
    SlabHeader *slab = all_slabs_head_;
    while (slab) {
      SlabHeader *next = slab->all_next;
      if (slab->tid() != 0 && slab->tid() != my_tid) {
        // Dead-thread slab: claim under our TID, reset the xthread
        // queue, then decide whether to free or push to abandoned.
        // Reset seal-state: parent-side counters are meaningless in the
        // child (any partial post-seal pushes would have come from
        // non-forking threads whose contributions are gone). The child
        // re-publishes via the standard abandon path if the slab is
        // non-empty. Single-threaded post-fork → all stores RELAXED;
        // there are no concurrent readers to synchronise with.
        slab->claim(my_tid);
        xthread_queue_init(slab);
        // Stamp retire_initiated with state_'s post-claim seq so it
        // tracks state_.seq across the upcoming release. Single-
        // threaded so no race with cross-thread freers; the seq tag
        // matters only when this slab transitions back to a
        // multi-threaded post-seal cycle later.
        slab->retire_initiated.store(
            SlabHeader::pack_retire(slab->state_seq(),
                                     SlabHeader::kRetireIdle),
            cpp::MemoryOrder::RELAXED);
        slab->sealed_target.store(0, cpp::MemoryOrder::RELAXED);
        slab->xthread_returned_count.store(0, cpp::MemoryOrder::RELAXED);
        slab->release();
        if (slab->returned == slab->bump) {
          // Empty: retire via Crystalline. Single-threaded post-fork
          // means no readers to wait on, so the FreeFn fires promptly.
          slab->retire_initiated.store(
              SlabHeader::pack_retire(slab->state_seq(),
                                       SlabHeader::kRetireDone),
              cpp::MemoryOrder::RELAXED);
          slab_retire_domain_.init_node(slab);
          slab_retire_domain_.retire(slab);
        } else {
          // Sealed-with-live-slots: configure last-freer-triggered retire.
          // Re-stamp retire_initiated with the post-release seq so any
          // future cross-thread freer's CAS uses a matching seq tag.
          slab->sealed_target.store(
              static_cast<uint16_t>(slab->bump - slab->returned),
              cpp::MemoryOrder::RELAXED);
          slab->retire_initiated.store(
              SlabHeader::pack_retire(slab->state_seq(),
                                       SlabHeader::kRetireIdle),
              cpp::MemoryOrder::RELAXED);
          push_abandoned(slab);
        }
      } else if (slab->tid() == my_tid) {
        // Self slab: the queue may contain partially-published entries
        // from dead non-forking threads. Re-init to empty rather than
        // draining — the producers can't complete Phase 3 post-fork.
        xthread_queue_init(slab);
        // Self slab is alive; parent-side seal state cannot apply.
        // Reset for cleanliness; stamp retire_initiated with state_'s
        // current seq so it stays in lockstep for any future abandon.
        slab->retire_initiated.store(
            SlabHeader::pack_retire(slab->state_seq(),
                                     SlabHeader::kRetireIdle),
            cpp::MemoryOrder::RELAXED);
        slab->sealed_target.store(0, cpp::MemoryOrder::RELAXED);
        slab->xthread_returned_count.store(0, cpp::MemoryOrder::RELAXED);
      }
      slab = next;
    }
  }
};

// ===----------------------------------------------------------------------===//
// Compile-time layout regression guards.
//
// Verifies that non-default SlabPool instantiations — the ones used by the
// mapping table's L2 and L3 page pools — compile cleanly and fit within the
// 4 KB header page budget. Every build re-checks these; if a future edit
// breaks a large-arena layout (bitmap overflow, state-word miscount, etc.),
// the failure points here rather than surfacing later as a cryptic error at
// the pool's first use.
// ===----------------------------------------------------------------------===//
namespace layout_checks {

// L2 pool: 64 KB arena, 8 KB minimum slot (one L2 page per slot, 6 per arena).
using L2Header = SlabHeaderT<65536, 8192>;
static_assert(SlabLayout<65536, 8192>::kMaxSlots == 53248 / 8192);
static_assert(sizeof(L2Header) <= 4096,
              "L2 SlabHeader must fit in the 4 KB header page");

// L3 pool: 1 MB arena, 65664 B minimum slot (one L3 page per slot, 15 per arena).
using L3Header = SlabHeaderT<1048576, 65664>;
static_assert(SlabLayout<1048576, 65664>::kMaxSlots ==
              (1048576 - 12288) / 65664);
static_assert(SlabLayout<1048576, 65664>::kStateWords ==
              (SlabLayout<1048576, 65664>::kSlotPages + 31) / 32);
static_assert(sizeof(L3Header) <= 4096,
              "L3 SlabHeader must fit in the 4 KB header page");

// Default pool: unchanged from pre-template. Byte-identical bitmap sizing.
static_assert(SlabLayout<>::kMaxSlots == 53248 / 8);
static_assert(SlabLayout<>::kStateWords == 1);

} // namespace layout_checks

} // namespace internal

namespace concurrent {
// BatchLinkCodec for SlabHeaderT — partial specialization on the
// pool template parameters. Decodes via the per-pool serial table
// reachable through `SlabPoolT<S, M>::slab_by_crystalline_serial`.
// Encoded value is `1 + crystalline_serial`; serial is monotonic
// per-pool and never zero by construction (init_slab adds 1 after
// the fetch_add).
template <size_t SlabBytesV, size_t MinSlotBytesV>
struct BatchLinkCodec<
    ::LIBC_NAMESPACE::internal::SlabHeaderT<SlabBytesV, MinSlotBytesV>> {
  using Node =
      ::LIBC_NAMESPACE::internal::SlabHeaderT<SlabBytesV, MinSlotBytesV>;
  using Pool =
      ::LIBC_NAMESPACE::internal::SlabPoolT<SlabBytesV, MinSlotBytesV>;
  LIBC_INLINE static uint32_t encode(CrystallineNode *n) noexcept {
    return 1u + static_cast<Node *>(n)->crystalline_serial;
  }
  LIBC_INLINE static CrystallineNode *decode(uint32_t code) noexcept {
    return Pool::slab_by_crystalline_serial(code - 1u);
  }
};
} // namespace concurrent

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SLAB_POOL_H
