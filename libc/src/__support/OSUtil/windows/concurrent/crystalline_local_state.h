//===-- Crystalline per-thread local state — POD defs ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-thread POD state for Crystalline-W SMR, ported 1:1 from the
// reference WFRTracker.hpp (Nikolaev & Ravindran, PPoPP 2024 /
// wfsmr/benchmark/src/trackers/WFRTracker.hpp). A verbatim copy of the
// reference lives alongside this file as crystalline_reference.hpp.inc
// for audit/diff.
//
// Shape summary
// -------------
// CrystallineDomainSlot records live in per-domain CrystallineSlotPools
// (crystalline_slot_pool.h) — process-lifetime VA, demand-committed,
// recycled via lock-free Treiber freelist. Per-thread retire batches
// (CrystallineBatch) live directly on ThreadScratchState. domain_id
// (assigned at registry_push time) indexes both ThreadScratchState's
// crystalline_slot_idx[] (uint16_t pool index, 0 = none claimed) and
// crystalline_batches[] (owner-exclusive retire bookkeeping).
//
// Why this lives here, not in crystalline_domain.h: the slot pool and
// thread_scratch consumers all need the slot/batch POD definitions
// without pulling in the templated CrystallineDomain<>. Splitting the
// POD defs into this leaf header breaks the include cycle.
//
// Contract
// --------
// - Zero-init is a valid state: no slots pinned, no batches pending.
//   The GuardedRegion eager-commit returns zero-filled pages, so the
//   first CrystallineDomain::read() on a brand-new thread sees a
//   clean region with no setup call required.
// - Slot / batch fields are mutated ONLY by the owning thread for
//   its own slots/batches. Cross-thread readers (help_thread,
//   try_retire's slot-selection loop, registry walks) ONLY read
//   via the explicit atomic operations in CrystallineDomain<>;
//   they never write.
// - Fork-reinit zeroes every surviving thread's region from a
//   single-threaded context before any other thread restart.
//
// Layout math (MAX_WFR=16, kMaxCrystallineDomains=8)
// --------------------------------------------------
//   CrystallineWordPair               = 16 B
//   CrystallineStateT (result+3 ptrs) = 48 B
//   CrystallineDomainSlot natural     = 18×(16+16+48) + linkage::Link = 1448 B
//     alignas(64) — cross-thread reads hit this struct, so line
//     isolation between adjacent domains' slots matters. sizeof
//     rounds up to 1472 B (one line of tail padding, ~2% overhead).
//   CrystallineBatch packed           = 48 B  (alignof 8 — owner-only
//                                              writes, no cross-thread
//                                              access, no line iso needed)
//   Slots no longer live inline in ThreadScratchState — they reside in
//   per-domain CrystallineSlotPools (see crystalline_slot_pool.h). Each
//   pool reserves up to 1<<16 slot VA, demand-committed; the freelist
//   linkage lives in the slot's `link` field, consumed by the
//   lock_free_linkage substrate.
//   Batches are still per-thread, owner-exclusive; they live directly
//   on ThreadScratchState as a kMaxCrystallineDomains-sized array.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_LOCAL_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_LOCAL_STATE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/lock_free_linkage.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace concurrent {

// -------------------------------------------------------------------------
// Compile-time parameters
// -------------------------------------------------------------------------

// Maximum Crystalline domains per process. Structural compile-time
// ceiling: each domain's domain_id (assigned at registry_push time)
// indexes ThreadScratchState::crystalline_slot_idx[] /
// crystalline_batches[]. Bumping requires a rebuild. 8 is the projected
// libc-wide cap — slab / page / mapping-table / fd-table / future
// subsystems — with headroom.
inline constexpr uint32_t kMaxCrystallineDomains = 8;

// Reservation-slots per thread per domain (WFRTracker's MAX_WFR).
// Concurrency-depth parameter, NOT a thread/memory scale cap: bounds
// the maximum number of pointer reservations one thread holds
// simultaneously on one domain. Reference default; skiplists up to
// MAX_LEVEL=8 (pred/succ pinned simultaneously) and radix/B+tree
// traversals fit comfortably. Expose `+2` extra internal slots
// for the helping-protocol's parent/helpee scratch (see
// WFRTracker.hpp help_thread's use of slots[mytid].state[hr_num]
// and slots[mytid].epoch[hr_num+1]).
inline constexpr uint32_t kCrystallineHrNum = 16;
inline constexpr uint32_t kCrystallineSlotCount = kCrystallineHrNum + 2;

// -------------------------------------------------------------------------
// CrystallineNode — intrusive base for every retirable user type.
// -------------------------------------------------------------------------
//
// Ported 1:1 from WFRTracker.hpp's `struct WFRInfo` (lines 84-95). The
// three-way union encodes the node's role in the algorithm at any
// moment in time:
//
//   - List nodes in a slot's retirement chain: `next` (atomic link to
//     next node in slot chain).
//   - List nodes being prepared for publication by try_retire: `slot`
//     (raw pointer to the target slot's `first` word-pair).
//   - The anchor ("refs") node of a batch: `birth_epoch` in the first
//     union and `refs` in the second.
//
//   - `batch_link` — points at the anchor (refs-node) of the batch
//     this node belongs to. WFR_IS_RNODE(batch_link) (low bit set)
//     identifies the node itself as the anchor. See WFR_RNODE /
//     WFR_IS_RNODE in crystalline_domain.h.
//
//   - Second union:
//       * refs        — modular-addend refcount on the anchor node.
//       * batch_next  — chain link for walking a batch's nodes
//                       (non-anchor role).
//
// All fields are owned by the runtime; user code must never write them.
// Zero-initialization is the valid "freshly allocated, never retired"
// state — `batch_link == nullptr` is what retire() checks to decide
// whether a pointer is live or already retired.
//
// The full struct is ABI-compatible with WFRInfo — fields are in the
// same order and same sizes. A NodeT that inherits CrystallineNode and
// adds trailing fields works the same way as the reference's
// `char block[sizeof(WFRInfo) + sizeof(T)]` layout.
struct CrystallineNode;

// -------------------------------------------------------------------------
// Invalid-slot sentinel (lifted from crystalline_domain.h to the leaf
// header so both crystalline_domain.h and crystalline_slot_pool.h share
// a single definition).
// -------------------------------------------------------------------------
inline constexpr uintptr_t kCrystallineInvPtr64 = static_cast<uintptr_t>(-1LL);
LIBC_INLINE CrystallineNode *crystalline_inv_ptr() {
  return reinterpret_cast<CrystallineNode *>(kCrystallineInvPtr64);
}

// 16-byte aligned word-pair atomic — union of two uint64_t halves
// (for single-half RELAXED ops) and one __uint128_t (for paired CAS).
// Matches WFRTracker.hpp's `union word_pair_t` exactly. Type-punning
// the three views is intentional: Crystalline relies on single-half
// writes to be ordinary 8-byte atomic ops and paired CAS to be one
// cmpxchg16b, verified correct on x86-64 (cx16 in the driver baseline)
// and AArch64 LSE2.
struct alignas(16) CrystallineWordPair {
  union {
    cpp::Atomic<uint64_t> pair[2];
    cpp::Atomic<CrystallineNode *> list[2];
    cpp::Atomic<__uint128_t> full;
  };
  LIBC_INLINE constexpr CrystallineWordPair() : pair{} {}
};

static_assert(sizeof(CrystallineWordPair) == 16,
              "CrystallineWordPair must be 16 bytes — layout math and "
              "cmpxchg16b use depend on this");
static_assert(alignof(CrystallineWordPair) == 16,
              "CrystallineWordPair must be 16-byte aligned for cmpxchg16b");

// Non-atomic companion: read from a dcas_load / CAS expected/desired
// temporary and decompose into its halves. Exists so the algorithm's
// value-pair bit-level manipulations (seqno in pair[1], pointer in
// pair[0]) compile to trivial plain-memory shuffles rather than
// atomic ops on scratch values.
union CrystallineValuePair {
  CrystallineNode *list[2];
  uint64_t pair[2];
  __uint128_t full;
};

static_assert(sizeof(CrystallineValuePair) == 16,
              "CrystallineValuePair must be 16 bytes");

// Per-reservation-slot helping state (reference's `struct state_t`).
// Written by the slot's owner when it enters slow_path; read by
// helpers during help_thread / help_read. All four fields are
// "for helpee only" — helpers never write them, they only read and
// then CAS the result word to indicate a produced value. The
// trailing _pad keeps the struct 48 bytes exactly for the array-
// indexed layout math to hold.
struct CrystallineStateT {
  CrystallineWordPair result;           // {ptr | invptr64, seqno}
  cpp::Atomic<uint64_t> epoch;          // birth_epoch snapshot
  cpp::Atomic<uint64_t> pointer;        // atomic<T*>* being dereferenced
  cpp::Atomic<CrystallineNode *> parent; // parent node's anchor
  void *_pad;
};

static_assert(sizeof(CrystallineStateT) == 48,
              "CrystallineStateT must be 48 bytes — reference's state_t "
              "layout used in the slot-array sizing");

// -------------------------------------------------------------------------
// Per-thread per-domain slot — the WFRSlot of the reference.
// -------------------------------------------------------------------------
//
// Indexed access contract (lifted verbatim from WFRTracker.hpp):
//   first[0 .. hr_num-1]    — reservation-slot head pointers
//   first[hr_num]           — help-protocol parent-reservation scratch
//   first[hr_num+1]         — help-protocol helpee scratch
//   epoch[i]                — paired with first[i], carries (epoch, seqno)
//   state[i]                — paired with first[i]/epoch[i], helping state
//
// 64-byte alignment isolates adjacent per-domain slot structs within
// one thread's region from sharing a cache line. Cross-thread readers
// (help_read / try_retire) walk every live thread's region indexing
// by domain_id; the owner thread writes its own slots concurrently,
// so a shared line would invalidate the reader's cache on every
// owner write. alignof(64) is the minimum line isolation on x86-64
// (128-byte pairing under DMLC is a perf tune we can adopt later
// without algorithmic change).
struct alignas(64) CrystallineDomainSlot {
  CrystallineWordPair first[kCrystallineSlotCount];
  CrystallineWordPair epoch[kCrystallineSlotCount];
  CrystallineStateT state[kCrystallineSlotCount];
  // Lock-free linkage substrate hookup. CrystallineSlotPool uses the
  // `next` field (16-bit pool index) for its Treiber freelist and the
  // `state` byte for the FREE/CLAIMED two-state machine. Tag bumping
  // (T1) defends every freelist push/pop against ABA without a separate
  // generation counter.
  cpp::Atomic<linkage::Link> link;
};

inline constexpr size_t kCrystallineDomainSlotNaturalSize =
    kCrystallineSlotCount *
        (sizeof(CrystallineWordPair) * 2 + sizeof(CrystallineStateT)) +
    sizeof(cpp::Atomic<linkage::Link>);

static_assert(sizeof(CrystallineDomainSlot) >=
                  kCrystallineDomainSlotNaturalSize,
              "CrystallineDomainSlot cannot be smaller than the sum of its "
              "array members — compiler reordering would break the array-"
              "indexed layout the algorithm relies on");
static_assert(sizeof(CrystallineDomainSlot) -
                      kCrystallineDomainSlotNaturalSize <
                  64,
              "CrystallineDomainSlot tail padding exceeds one cache line — "
              "bumping kCrystallineHrNum may have crossed an alignment "
              "boundary; re-check the sizing math");

// Pin the substrate-required offset structurally. arrays:
//   first[18]: 0..287
//   epoch[18]: 288..575
//   state[18]: 576..1439
//   link:      1440..1447   (8 bytes, naturally aligned on the 8-byte
//                            boundary that follows the state[] array)
LINKAGE_REQUIRES_LINK_AT(CrystallineDomainSlot, 1440);

// -------------------------------------------------------------------------
// Per-thread per-domain retire-batch.
// -------------------------------------------------------------------------
//
// Equivalent of the reference's WFRBatch. Only the owning thread
// writes; helpers never touch. The dynamic batch (first ... last chain
// via batch_next) accumulates retires; try_retire publishes the batch
// across the K slots using the modular-addend refcount trick.
//
// `alloc_counter` splits the reference's separate per-thread
// alloc_counters[] into the same struct (removes the need for a
// second padded array). Bumped by init_node(); every Freq-th bump
// triggers help_read + global epoch increment.
//
// Packed tight at 48 B (alignof 8). No cross-thread access — only
// the owner writes, and only the owner reads. Lives on ThreadScratchState
// (per-thread arena) so retire bookkeeping shares the owner's L1 with
// the rest of the per-thread allocator hot data.
struct CrystallineBatch {
  CrystallineNode *first;     // batch chain head (most recent retire)
  CrystallineNode *last;      // batch chain tail (anchor / refs node)
  CrystallineNode *list;      // chain of refs-nodes ready to reclaim
  uint64_t counter;           // retire counter — drives try_retire cadence
  uint64_t list_count;        // free-cache population (≤ MAX_WFRC)
  uint64_t alloc_counter;     // init_node counter — drives epoch bumps
};

static_assert(sizeof(CrystallineBatch) == 48,
              "CrystallineBatch must pack to exactly 48 bytes — owner-only "
              "access, no cross-thread line isolation needed");

// CrystallineThreadRegion was removed when slots moved out of the
// ThreadScratch arena into per-domain CrystallineSlotPools. The only
// per-thread Crystalline state still inline-with-the-arena is the
// `CrystallineBatch batches[kMaxCrystallineDomains]` array, which lives
// directly on ThreadScratchState (owner-exclusive access, no
// cross-thread reachability needed past flush-on-exit).

} // namespace concurrent
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_LOCAL_STATE_H
