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
// crystalline_batches[]. 
inline constexpr uint32_t kMaxCrystallineDomains = 16;

// Reservation-slots per thread per domain (WFRTracker's MAX_WFR).
// Concurrency-depth parameter, NOT a thread/memory scale cap: bounds
// the maximum number of pointer reservations one thread holds
// simultaneously on one domain. Reference default; skiplists up to
// MAX_LEVEL=8 (pred/succ pinned simultaneously) and radix/B+tree
// traversals fit comfortably. Expose `+2` extra internal slots
// for the helping-protocol's parent/helpee scratch (see
// WFRTracker.hpp help_thread's use of slots[mytid].state[hr_num]
// and slots[mytid].era[hr_num+1]).
inline constexpr uint32_t kCrystallineHrNum = 16;
inline constexpr uint32_t kCrystallineSlotCount = kCrystallineHrNum + 2;

// -------------------------------------------------------------------------
// CrystallineNode — empty tag base for every retirable user type.
// -------------------------------------------------------------------------
//
// Carries identity only (so `static_cast<CrystallineNode*>(NodeT*)` at
// the slot-pool / batch-chain boundary type-checks, and `is_base_of_v<
// CrystallineNode, NodeT>` works as a SFINAE predicate). The storage
// — `next`/`slot`/`birth_era` union, `refs`/`batch_next` union, and
// `batch_link` — lives in the derived class, emitted by the macro
// `LIBC_CRYSTALLINE_NODE_FIELDS(Self)` below.
//
// Why empty: derived classes pin offsets via `static_assert(offsetof(...))`
// — e.g. `RegionDesc::view_prot == 20`, `DescBacking::generation == 20`.
// C++17 [class]/7 requires, for `offsetof` to be unconditionally
// supported, that ALL non-static data members of the class and its base
// classes be first-declared in the same class. With fields in the base
// AND fields in the derived class, the derived class is non-standard-
// layout and `offsetof` becomes conditionally-supported (clang fires
// `-Winvalid-offsetof`). Making the base empty puts every NSDM in the
// derived class — `offsetof` becomes ISO-conformant, and the layout is
// guaranteed by `[class.mem]/26` (NSDM in the same access-control
// region are laid out in declaration order with natural alignment),
// not by the Itanium-ABI tail-padding-reuse rule.
//
// Under empty-base optimization the empty `CrystallineNode` contributes
// zero bytes to any standard-layout derived class, so the macro fields
// land at offset 0 in the derived class — same byte layout as today,
// achieved by ordinary layout rules.
//
// All field semantics live on the macro below; see that comment for the
// 1:1 mapping to WFRTracker.hpp's `struct WFRInfo`.
struct CrystallineNode {};

// Forward declaration for the `slot` alias in the first union of
// LIBC_CRYSTALLINE_NODE_FIELDS below — the full definition appears
// later in this file, but only a pointer-to-incomplete is needed by
// the macro.
struct CrystallineWordPair;

// -------------------------------------------------------------------------
// LIBC_CRYSTALLINE_NODE_FIELDS(Self) — emit the intrusive Crystalline-W
// node fields into a derived class.
// -------------------------------------------------------------------------
//
// Ported 1:1 from WFRTracker.hpp's `struct WFRInfo` (lines 84-95). The
// three-way first union encodes the node's role at any moment:
//
//   First union (8 B at offset 0):
//     next        (inserted state)  — atomic chain link in a slot's list
//     slot        (prepare state)   — target CrystallineWordPair* for a
//                                      try_retire slot assignment
//     birth_era   (anchor / refs node) — era stamped by init_node
//
//   Second union (8 B at offset 8):
//     refs        (anchor)           — modular-addend refcount on the
//                                      batch anchor node
//     batch_next  (non-anchor)       — chain link for walking a batch's
//                                      nodes in retire order
//
//   batch_link    (4 B at offset 16) — 32-bit encoded anchor reference;
//                                      see crystalline_domain.h for the
//                                      bit layout and BatchLinkCodec
//                                      specialization contract.
//
// `Self` is the enclosing derived class — `next` and `batch_next` carry
// `Self*` so chain walks inside the template body stay typed (avoiding
// universal-pointer downcasts on every hop). At the slot-pool / batch-
// chain boundary the template upcasts to `CrystallineNode*` (the
// universal type erased into the slot's `Atomic<CrystallineNode*>`
// view); the upcast is well-defined empty-base.
//
// All fields are owned by the runtime; user code must never write them.
// Zero-initialization is the valid "freshly allocated, not yet retired"
// state — `init_node()` stamps `birth_era` and `batch_link = 0u` at
// publication time.
//
// Macro shape rationale (vs. a CRTP template base):
//   - A CRTP template base with these as NSDM would re-introduce the
//     non-standard-layout split (base has NSDM, derived has NSDM).
//   - A macro emits the fields *as members of the derived class itself*,
//     keeping the derived class standard-layout when its own field set
//     respects standard-layout rules.
// The chain-link member is named `cn_next` rather than `next` to keep the
// macro composable with derived types that already carry a `next` field
// of their own (notably the skiplist's `next[]` per-level link array in
// `SkiplistNodeBase`). The reference paper / WFRTracker.hpp calls this
// field `next`; the rename is local to the macro and the substrate
// template — it doesn't change the algorithm.
#define LIBC_CRYSTALLINE_NODE_FIELDS(Self)                                     \
  union {                                                                      \
    ::LIBC_NAMESPACE::cpp::Atomic<Self *> cn_next;                             \
    ::LIBC_NAMESPACE::concurrent::CrystallineWordPair *slot;                   \
    uint64_t birth_era;                                                        \
  };                                                                           \
  union {                                                                      \
    ::LIBC_NAMESPACE::cpp::Atomic<uintptr_t> refs;                             \
    Self *batch_next;                                                          \
  };                                                                           \
  ::LIBC_NAMESPACE::cpp::Atomic<uint32_t> batch_link

// Layout-reference type — never instantiated standalone, only used to
// pin the field layout the macro produces independently of any
// particular derived class. Standard-layout, so `offsetof` here is
// unconditionally supported and asserts the byte layout once for the
// whole substrate.
struct CrystallineNodeLayoutRef : public CrystallineNode {
  LIBC_CRYSTALLINE_NODE_FIELDS(CrystallineNodeLayoutRef);
};

static_assert(sizeof(CrystallineNodeLayoutRef) == 24,
              "CrystallineNodeLayoutRef must be 24 B — the macro emits two "
              "8-byte unions plus a 4-byte batch_link, rounded to alignof(8) "
              "for the atomic-uintptr_t union member; derived classes rely "
              "on the 4-byte slot at offset 20..23 being available for a "
              "natural 4-byte first user field (RegionDesc::view_prot, "
              "DescBacking::generation, ArenaHeader::arena_serial, ...).");
static_assert(alignof(CrystallineNodeLayoutRef) == 8,
              "CrystallineNodeLayoutRef must be 8 B aligned (atomic uintptr_t "
              "in the second union)");
static_assert(__builtin_offsetof(CrystallineNodeLayoutRef, batch_link) == 16,
              "batch_link must be at offset 16 — the 4-byte tail slot at "
              "[20..23] is the substrate-shared first-user-field landing pad");

// -------------------------------------------------------------------------
// Invalid-slot sentinel (lifted from crystalline_domain.h to the leaf
// header so both crystalline_domain.h and crystalline_slot_pool.h share
// a single definition).
// -------------------------------------------------------------------------
//
// Templated default returns `CrystallineNode*` (the universal type used
// at the slot-pool boundary). Explicit-typed variants give NodeT*-typed
// sentinels for chain comparisons inside the template body where the
// chain field is `Atomic<NodeT*>`.
inline constexpr uintptr_t kCrystallineInvPtr64 = static_cast<uintptr_t>(-1LL);
template <typename T = CrystallineNode>
LIBC_INLINE T *crystalline_inv_ptr() {
  return reinterpret_cast<T *>(kCrystallineInvPtr64);
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
// helpers during help_thread / help_read. All five fields are
// "for helpee only" — helpers never write them, they only read and
// then CAS the result word to indicate a produced value. Layout sized
// at 48 bytes — fits the slot-array indexing math the algorithm relies
// on.
//
// Deviation from the reference's `state_t`: the reference stores a
// single `pointer` field (`std::atomic<T**>`) — the atomic address the
// helpee was about to dereference. Helpers redo the load via
// `obj->load()`. That works because the reference assumes every
// protected load is a single atomic-pointer load.
//
// This port generalizes: ART's `art_node_get_child` is a multi-step
// scan (N4/N16/N48 indirection), and the skiplist's link decode is a
// packed-Link load + chunk-table indirection. To support those, we
// publish a (load_thunk, load_ctx) pair — a free-function pointer +
// stack-borne context — that helpers invoke instead of redoing a
// single atomic load. The simple atomic-pointer case feeds through
// the same machinery via a static atomic-load thunk in
// CrystallineDomain.
struct CrystallineStateT {
  CrystallineWordPair result;                            // {ptr | invptr64, seqno}
  cpp::Atomic<uint64_t> birth_era;                       // parent birth-era snapshot
  cpp::Atomic<CrystallineNode *(*)(void *)> load_thunk;  // helpee load callable
  cpp::Atomic<void *> load_ctx;                          // opaque ctx for thunk
  cpp::Atomic<CrystallineNode *> parent;                 // parent node's anchor
};

static_assert(sizeof(CrystallineStateT) == 48,
              "CrystallineStateT must be 48 bytes — slot-array sizing math "
              "depends on this");

// -------------------------------------------------------------------------
// Per-thread per-domain slot — the WFRSlot of the reference.
// -------------------------------------------------------------------------
//
// Indexed access contract (lifted verbatim from WFRTracker.hpp):
//   first[0 .. hr_num-1]    — reservation-slot head pointers
//   first[hr_num]           — help-protocol parent-reservation scratch
//   first[hr_num+1]         — help-protocol helpee scratch
//   era[i]                  — paired with first[i], carries (era, seqno)
//   state[i]                — paired with first[i]/era[i], helping state
//
// Field naming note: the reference paper Figs. 5/6/10/13 use "era"
// (Hazard-Era lineage). The codebase previously used "epoch", which
// reads like a classical EBR (Epoch-Based Reclamation) global
// reclamation-cycle counter — misleading, since Crystalline-W's
// counter is an allocation-generation watermark bumped by `init_node`,
// used purely for slot-eligibility filtering on retire batches. The
// rename to `era` aligns with paper terminology and makes the
// algorithm auditable line-by-line against the reference figures.
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
  CrystallineWordPair era[kCrystallineSlotCount];
  CrystallineStateT state[kCrystallineSlotCount];
  // Lock-free linkage substrate hookup. CrystallineSlotPool uses the
  // `next` field (16-bit pool index) for its Treiber freelist and the
  // `state` byte for the FREE/CLAIMED two-state machine.
  cpp::Atomic<linkage::Link> link;
  // Slot-lifecycle generation counter — substrate-mandatory for
  // consumers of harris_walk_attempt / harris_unlink. Bumped on every
  // freelist_push (slot leaves the active chain and re-enters the
  // free-pool); captured by release_slot before the splice; re-checked
  // by the walker at the target site. Closes the slot-lifecycle ABA
  // window the per-link tag (T1) doesn't cover — T1 defends one
  // lifecycle's link writes, `generation` defends across reclaim+
  // realloc cycles.
  cpp::Atomic<uint32_t> generation;
};

inline constexpr size_t kCrystallineDomainSlotNaturalSize =
    kCrystallineSlotCount *
        (sizeof(CrystallineWordPair) * 2 + sizeof(CrystallineStateT)) +
    sizeof(cpp::Atomic<linkage::Link>) + sizeof(cpp::Atomic<uint32_t>);

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

// Pin the substrate-required offsets structurally. arrays:
//   first[18]:   0..287
//   era[18]:     288..575
//   state[18]:   576..1439
//   link:        1440..1447   (8 bytes, naturally aligned)
//   generation:  1448..1451   (4 bytes, naturally aligned)
//   tail pad:    1452..1471   (20 bytes, alignas(64) round-up)
LINKAGE_REQUIRES_LINK_AT(CrystallineDomainSlot, 1440);
LINKAGE_REQUIRES_GENERATION_AT(CrystallineDomainSlot, 1448);

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
// triggers help_read + global era increment.
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
  uint64_t alloc_counter;     // init_node counter — drives era bumps
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
