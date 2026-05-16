//===-- CrystallineDomain — wait-free SMR primitive ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Crystalline-W safe memory reclamation (Nikolaev & Ravindran, PPoPP '24).
// Ported line-by-line from the reference implementation at
// wfsmr/benchmark/src/trackers/WFRTracker.hpp; a verbatim copy is preserved
// alongside this file as crystalline_reference.hpp.inc for audit/diff.
//
// Properties
// ----------
// Crystalline-W incorporates in a single algorithm:
//   * wait-freedom (via help_thread helping protocol),
//   * asynchronous reclamation (retires do not block foreign readers),
//   * balanced reclamation workload (each thread reaps its own retires
//     on average — no stampede onto a single reaper),
//   * bounded memory under starving threads (Crystalline-L theorem).
//
// Integration shape
// -----------------
// Per-thread reservation slots live in a per-domain CrystallineSlotPool
// (see crystalline_slot_pool.h) — VA reserved up front, demand-committed
// on growth. Each thread carries a uint16_t slot index per domain on its
// ThreadScratchState (`crystalline_slot_idx[domain_id]`); 0 = "no slot
// claimed yet". The first protect()/init_node() call on a new thread
// for this domain lazy-claims a slot (Treiber-pop, lock-free); thread
// exit releases it (Treiber-push). Per-thread retire batches (CrystallineBatch)
// also live on ThreadScratchState — they are owner-exclusive and need no
// cross-thread reachability.
//
// Cross-thread helping (try_retire phase A, help_read, slow_path parent-
// handoff) walks the slot pool by integer index `[1, pool_.committed())`.
// The pool's storage is process-lifetime (substrate Safety Triad T3); a
// peer walk reading a slot whose owner just exited sees the constructor-
// init tombstone (`first[j].list[0] == crystalline_inv_ptr()`) the
// release path stamped before pushing the slot back to the freelist, and
// early-exits via the same eligibility check (`crystalline_domain.h:900`)
// the algorithm has always used for inactive slots. No reader fences, no
// quiescence, no RCU.
//
// API shape
// ---------
// The API mirrors the reference paper §4.2 Fig. 10's `protect()`
// primitive (no Handle/Guard wrapper; callers manage reservation
// indices and pass the parent node themselves). Method surface:
//
//   * init_node(NodeT*)                                — stamp birth_era,
//     called by the user after allocating a NodeT and before publishing
//     it to a lock-free data structure. Replaces the reference's alloc().
//   * protect(atomic<NodeT*>&, index, NodeT* parent)   — atomic-pointer
//     overload, paper Fig. 10 canonical. 16-attempt era-stability
//     fast path + bounded slow_path (paper §5 Lemma 5.2 / 5.3).
//   * protect<LoadThunk>(ctx, index, NodeT* parent)    — generalized
//     overload for multi-step loads (ART scan, encoded-link decode).
//     Same algorithm; `LoadThunk` is a free-function pointer published
//     into state[index] for foreign-thread helpers to invoke.
//   * retire(NodeT*)                                    — submit for
//     deferred reclamation.
//   * clear_all()                                       — drop every
//     reservation on the current thread (for API boundaries).
//   * current_era()                                     — introspection.
//
// Deviations from the reference
// -----------------------------
// 1. Reference `read()` is renamed to `protect()` to match paper §4.2
//    Fig. 10 directly. Algorithm-identical.
// 2. Reference has no `reserve_slot()` (advance era without loading).
//    A previous draft of this port carried one; it enabled an unbounded
//    `load → reserve_slot → re-load → equality-check` antipattern at
//    consumer sites and has been deleted. The remaining no-load case is
//    `is_walk_range`'s predecessor-pin rotation: the `walk_prev` is
//    already pinned by the previous iteration's `pinned_read_link_target`
//    on the cur slot, and on advance the prev slot's era is refreshed
//    via `anchor()` so `try_retire` won't skip the slot. This is
//    bounded (two slots, ping-pong rotation) and load-bearing (the
//    prev slot needs an era ≥ batch min_era for retire-attachment to
//    find it) — distinct from the deleted `DomainPin` antipattern,
//    which was a per-operation outer "pin" that the inner protect()s
//    on real loads already covered. `anchor()` is the first-class API
//    for this case; callers MUST hold the pinned pointer alive through
//    an external mechanism (see `anchor()`'s declaration below).
// 3. The thunk overload of `protect()` extends Fig. 10 to multi-step
//    loads; slow_path / help_thread publish a (load_thunk, load_ctx)
//    pair via `state[index]` so foreign helpers can invoke the load
//    on the helpee's behalf. Atomic-pointer `protect` feeds through
//    the same generalization via a static `atomic_load_thunk` member.
// 4. Naming: paper Figs. 5/6/10/13 use "era" (Hazard-Era lineage).
//    The codebase previously used "epoch", which read like classical
//    EBR (Epoch-Based Reclamation). Renamed to `era` to match the
//    paper and avoid the EBR connotation — Crystalline-W's counter
//    is an allocation-generation watermark, not a reclamation cycle.
//
// NodeT contract
// --------------
// User types inherit from CrystallineNode. The base class provides the
// intrusive metadata fields (next / batch_link / refs / batch_next /
// birth_era). Zero-initialization is a valid "never retired" state
// (batch_link == 0u is the retired-check discriminator). User
// allocation is outside the primitive — init_node stamps the runtime
// state but doesn't allocate. FreeFn (template parameter) is invoked
// once per node when the batch's refcount wraps to zero, and receives
// the full NodeT* for the user-side reclaim.
//
// Each consumer must specialize `BatchLinkCodec<NodeT>` (defined
// below) — the codec encodes the back-pointer from a retired node to
// its batch's anchor as a 31-bit slot identifier so `batch_link` can
// fit in 4 bytes. See the BatchLinkCodec contract for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_DOMAIN_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_DOMAIN_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/type_traits.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain_registry.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_local_state.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_slot_pool.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace concurrent {

// -------------------------------------------------------------------------
// CrystallineNode — empty tag base; field semantics
// -------------------------------------------------------------------------
//
// The empty-tag-base definition is in crystalline_local_state.h, alongside
// `LIBC_CRYSTALLINE_NODE_FIELDS(Self)` which emits the intrusive runtime
// fields directly into each derived class (keeping the derived class
// standard-layout so `offsetof` is unconditionally supported per
// [support.types.layout]/1).
//
// The macro is a 1:1 port of WFRTracker.hpp's `struct WFRInfo`, with one
// substrate-level deviation: `batch_link` is encoded as a 32-bit slot
// code via `BatchLinkCodec<NodeT>` rather than as a raw pointer. The
// encoding keeps every Crystalline-managed type's intrusive header at
// 20 bytes (a 4-byte trailing slot is the natural landing pad for a
// 4-byte first user field) — the back-reference from a retired node to
// its batch's anchor is a slow-path-only field, never read on the
// `protect()` fast path, so the codec's encode/decode indirection is
// confined to retire / reclaim.
//
// The two type-punned unions encode the node's current role:
//
//   First union:
//     next        (inserted state)  — atomic chain link, `Atomic<NodeT*>`
//     slot        (prepare state)   — target CrystallineWordPair* for a
//                                      try_retire slot assignment
//     birth_era   (anchor / refs node) — era stamped by init_node
//
//   Second union:
//     refs        (anchor)           — modular-addend refcount on the
//                                      batch anchor node
//     batch_next  (non-anchor)       — chain link `NodeT*` for walking a
//                                      batch's nodes in retire order
//
// `batch_link` is a 32-bit code:
//   * 0x00000000              — unretired (sentinel; codecs never emit 0)
//   * (1u << 31) | code(self) — this node IS its batch's anchor; the
//                                bottom 31 bits encode the chain head
//                                (the most-recently-retired node in the
//                                batch, batch.first at retire-close)
//   * code(anchor)            — non-anchor retired; bottom 31 bits
//                                encode the anchor node
//
// The bit-31 RNODE tag is namespace-distinct from the bit-0 RNODE tag
// the chain pointer carries in `next` (see crystalline_rnode /
// crystalline_is_rnode below) — the two share no semantics beyond
// "marks the anchor end of a chain in their respective fields."
//
// All fields are owned by the runtime; user code must never read or
// write them. Zero-initialization is the valid "freshly allocated, not
// yet retired" state — init_node() stamps birth_era and batch_link=0u
// at publication time.

// -------------------------------------------------------------------------
// Sentinels and tag-bit helpers — reference's macros, typed.
// -------------------------------------------------------------------------

// `kCrystallineInvPtr64` and `crystalline_inv_ptr()` live in
// crystalline_local_state.h so the slot-pool consumer can reach them
// without depending on this template header.

// Anchor-node marker bits in `refs`. The reference uses 1<<63 on initial
// retire and 1<<62 for the parent-handoff adjs accounting. Modular
// arithmetic on uintptr_t — wrap to zero is the free trigger.
inline constexpr uintptr_t kCrystallineProtect1 =
    static_cast<uintptr_t>(1ULL << 63);
inline constexpr uintptr_t kCrystallineProtect2 =
    static_cast<uintptr_t>(1ULL << 62);

// Tag bit 0 on chain pointers (the value stored in `next`) distinguishes
// "the next link IS a tagged self-reference to the anchor" from "the
// next link is a plain pointer to another non-anchor chain entry". This
// is the chain-pointer RNODE namespace; it is SEPARATE from the bit-31
// RNODE tag that lives in `batch_link` (see `kCrystallineBatchLinkRnodeBit`
// below).
//
// Templated on the pointer type so call sites can pass either the
// type-erased `CrystallineNode *` (slot-pool boundary, batch-chain
// crossings) or the typed `NodeT *` (chain field is `Atomic<NodeT*>`
// in the derived class via LIBC_CRYSTALLINE_NODE_FIELDS) without
// requiring round-trip casts. Both forms perform the same bit
// arithmetic on the raw pointer value.
template <typename T>
LIBC_INLINE T *crystalline_rnode(T *n) {
  return reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(n) ^ 1U);
}
template <typename T>
LIBC_INLINE bool crystalline_is_rnode(T *n) {
  return (reinterpret_cast<uintptr_t>(n) & 1U) != 0;
}

// -------------------------------------------------------------------------
// `batch_link` 32-bit code layout
// -------------------------------------------------------------------------
//
// bit 31     — RNODE tag: this node IS its batch's anchor; bottom 31
//              bits encode the chain head (most-recent retire).
// bits 30..0 — codec slot encoding produced by `BatchLinkCodec<NodeT>`.
//
// 0x00000000 is the unretired sentinel. Codecs MUST never emit 0 and
// MUST never set bit 31 in their encode output.
inline constexpr uint32_t kCrystallineBatchLinkRnodeBit = 1u << 31;
inline constexpr uint32_t kCrystallineBatchLinkCodeMask = 0x7FFFFFFFu;

// -------------------------------------------------------------------------
// BatchLinkCodec<NodeT>
// -------------------------------------------------------------------------
//
// Per-NodeT trait that encodes a CrystallineNode pointer (which is
// always a `NodeT *` upcast, since each domain only retires its own
// NodeT) into a 31-bit slot identifier and decodes it back. The
// substrate calls `BatchLinkCodec<NodeT>::encode(...)` /
// `BatchLinkCodec<NodeT>::decode(...)` on every retire / reclaim slow-
// path access to `batch_link`.
//
// Contract — every specialization must satisfy:
//   1. `encode(node) ∈ [1, 0x7FFFFFFFu]` — never 0, never has bit 31 set.
//   2. `decode(encode(node)) == node` — bijective round-trip.
//   3. `encode` and `decode` are static, noexcept, and may be called on
//      any thread at any time after the NodeT was first published to
//      Crystalline (init_node having run is sufficient).
//   4. `decode` may safely run on a NodeT whose memory is still
//      committed but whose user-visible content has been torn down by
//      the FreeFn — only the encoded identity must remain resolvable.
//
// Each consumer's TU specializes the template. The primary template
// is left undefined: a missing specialization fires a clean "no
// definition for BatchLinkCodec<...>" link error, surfacing unported
// consumers immediately.
template <typename NodeT> struct BatchLinkCodec;

// Cap on the free-list cache held per-thread during a traverse walk
// (reference's MAX_WFRC). Bounds memory the reaper carries between
// explicit free_list() flushes. Reference default = 12.
inline constexpr uint32_t kCrystallineFreeCacheCap = 12;

// -------------------------------------------------------------------------
// CrystallineDomain<NodeT, FreeFn, Freq>
// -------------------------------------------------------------------------
//
// NodeT  — user retirable type, inherits CrystallineNode.
// FreeFn — `void (*)(NodeT*)` invoked once per node on final reclaim.
// Freq   — retire counter threshold for try_retire triggering. Matches
//          the reference's `emptyFreq` parameter. Default 128.
// MaxIdx — per-domain reservation-slot budget (paper's MAX_IDX, the
//          WFRTracker constructor's `hr_num`). Each consumer declares
//          the maximum number of pointer reservations any one thread
//          holds simultaneously on this domain; the slot type sizes
//          first[]/era[]/state[] as MaxIdx + 2. Default mirrors the
//          pre-E3 global cap so domains that don't tune it keep the
//          old footprint.
template <typename NodeT, auto FreeFn, uint32_t Freq = 128,
          uint32_t MaxIdx = kCrystallineDefaultMaxIdx>
class CrystallineDomain {
  static_assert(cpp::is_base_of_v<CrystallineNode, NodeT>,
                "NodeT must derive from CrystallineNode");
  static_assert(Freq > 0, "Freq must be positive");
  static_assert(MaxIdx > 0, "MaxIdx must be positive");

  using SlotT = CrystallineDomainSlot<MaxIdx>;

public:
  // Trivially constructible — every member zero-inits via in-class
  // initializers / aggregate value-init (cpp::Atomic has a constexpr
  // default ctor; CrystallineDomainDescriptor is a POD). This keeps
  // file-scope CrystallineDomain<> instances out of the global-ctor
  // table (libc builds with -Wglobal-constructors as -Werror).
  //
  // The domain is NOT usable in this state — `init_registration()`
  // must be called before any read()/retire()/init_node() use, and
  // the call must complete-happen-before any other thread observes
  // the domain. Caller orchestrates: the substrate consumer drives
  // it from `va_substrate_init_fn` (Tier A Phase 1, single-threaded).
  LIBC_INLINE constexpr CrystallineDomain() = default;

  // Self-install descriptor into the global registry. Idempotency NOT
  // provided (the underlying `registry_push` would assign a fresh
  // domain_id on a second call and corrupt the slot accounting).
  // Caller is responsible for one-shot semantics — the natural Tier A
  // bootstrap ordering provides this for free.
  LIBC_INLINE void init_registration() {
    descriptor_.context = static_cast<void *>(this);
    descriptor_.fork_reinit_fn = &CrystallineDomain::fork_reinit_trampoline;
    descriptor_.fini_fn = &CrystallineDomain::fini_trampoline;
    descriptor_.thread_flush_fn =
        &CrystallineDomain::thread_flush_trampoline;
    descriptor_.release_slot_fn =
        &CrystallineDomain::release_slot_trampoline;
    descriptor_.warm_thread_fn =
        &CrystallineDomain::warm_thread_trampoline;
    descriptor_.name = "crystalline_domain";
    if (LIBC_UNLIKELY(!pool_.init()))
      __builtin_trap();
    registry_push(&descriptor_);
    domain_id_ = descriptor_.domain_id;
    // era starts at 1 (reference uses 0 as sentinel / "uninitialized")
    era_.store(1, cpp::MemoryOrder::RELEASE);
    slow_counter_.store(0, cpp::MemoryOrder::RELEASE);
  }

  CrystallineDomain(const CrystallineDomain &) = delete;
  CrystallineDomain &operator=(const CrystallineDomain &) = delete;

  // -----------------------------------------------------------------------
  // Public API — 1:1 with WFRTracker's surface (minus `tid` — implicit).
  // -----------------------------------------------------------------------

  // Stamp birth_era on a freshly allocated node. Replaces the
  // reference's alloc() — the user handles allocation, then calls this
  // to prepare the node for use in the lock-free data structure.
  // Pure era stamp — era ticks move to try_retire close (see below).
  LIBC_INLINE void init_node(NodeT *node) {
    node->birth_era = era_.load(cpp::MemoryOrder::ACQUIRE);
    node->batch_link.store(0u, cpp::MemoryOrder::RELAXED);
  }

  // -----------------------------------------------------------------------
  // protect() — paper §4.2 Fig. 10 primitive. Two overloads.
  // -----------------------------------------------------------------------
  //
  // Both overloads run the era-stability fast path: up to 16 attempts
  // matching `prev_era` against `current_era()` while the load returns
  // a pointer; if the global era keeps drifting past us, fall through
  // to slow_path, which is bounded by the active-slot count via the
  // helping protocol (paper §5 Lemma 5.2 / 5.3).
  //
  // `parent` is the node whose field is being dereferenced. Used by
  // slow_path's parent-handoff accounting to defer reclamation of
  // `parent` itself if it gets retired while we're stalled — see
  // slow_path's PROTECT2 / active-chain CAS scan (lines below).
  //
  // Atomic-pointer overload — the canonical Fig. 10 shape, used wherever
  // the protected field is a single `cpp::Atomic<NodeT *>`. The static
  // member thunk inside dispatches to this overload's load through the
  // same generalized slow_path machinery, so a slow_path activation
  // here uses the exact same helping protocol as the thunk overload.
  [[nodiscard]] LIBC_INLINE NodeT *
  protect(cpp::Atomic<NodeT *> &obj, uint32_t index, NodeT *parent) {
    SlotT &my = my_slot_state();
    uint64_t prev_era =
        my.era[index].pair[0].load(cpp::MemoryOrder::ACQUIRE);
    uint32_t attempts = 16;
    do {
      NodeT *ptr = obj.load(cpp::MemoryOrder::ACQUIRE);
      uint64_t curr_era = current_era();
      if (curr_era == prev_era)
        return ptr;
      prev_era = do_update(curr_era, index);
    } while (--attempts != 0);

    return slow_path(&atomic_load_thunk, &obj, index, parent);
  }

  // Generalized overload — for multi-step protected loads (ART's
  // N4/N16/N48 child-by-key scan, the skiplist's encoded Link decode).
  // `LoadThunk` is a non-type compile-time function-pointer parameter
  // of type `NodeT *(*)(void *)`; `ctx` is a stack-borne struct holding
  // everything the thunk needs. The compile-time function pointer means
  // the fast-path call inlines (no indirect call); the same pointer is
  // published into `state[index].load_thunk` for slow-path helpers to
  // invoke if convergence fails.
  //
  // Contract on `LoadThunk`:
  //   * Pure function (no captured state, no side effects beyond reads).
  //   * Safe to invoke from a foreign helper thread holding `parent`
  //     pinned via the parent-handoff protocol — i.e., the thunk may
  //     dereference `parent` and any pointer reachable from it via
  //     Crystalline-managed loads, but must NOT assume any other slot
  //     is held by the helper.
  //   * Returning nullptr is legal (key not present, link encodes null,
  //     etc.) — slow_path & help_thread treat null as a valid result.
  //   * Must NOT recurse back into protect() on this domain at this
  //     index (would clobber state[index]). Other domains / other
  //     indices are fine.
  //
  // Contract on `ctx`:
  //   * Must outlive every invocation of the thunk for this protect()
  //     call. Slow_path is synchronous from the caller's view (returns
  //     when help completes), so a stack-borne ctx is sound.
  template <auto LoadThunk, class Ctx>
  [[nodiscard]] LIBC_INLINE NodeT *
  protect(Ctx &ctx, uint32_t index, NodeT *parent) {
    static_assert(cpp::is_same_v<decltype(LoadThunk), NodeT *(*)(void *)>,
                  "LoadThunk must be NodeT *(*)(void *)");
    SlotT &my = my_slot_state();
    uint64_t prev_era =
        my.era[index].pair[0].load(cpp::MemoryOrder::ACQUIRE);
    uint32_t attempts = 16;
    do {
      NodeT *ptr = LoadThunk(static_cast<void *>(&ctx));
      uint64_t curr_era = current_era();
      if (curr_era == prev_era)
        return ptr;
      prev_era = do_update(curr_era, index);
    } while (--attempts != 0);

    return slow_path(LoadThunk, static_cast<void *>(&ctx), index, parent);
  }

  // Refresh this slot's era to current_era() so try_retire considers
  // it eligible against future retire batches. Drains any pending chain
  // attached to first[index] before publishing the era.
  //
  // PRECONDITION: the caller already holds the pointer being pinned
  // through an external mechanism — another reservation slot on this
  // thread, an external refcount, a thread-private lock, etc. Calling
  // anchor() on a slot whose pinned pointer is not externally held
  // races a concurrent retire and is a UAF in the caller (anchor()
  // publishes the new era after try_retire's eligibility check has
  // already excluded this slot, so the retiring batch may free the
  // pointer while the caller is still reading).
  //
  // Cheaper than protect()'s era-stability loop because it skips the
  // 16-attempt fast path and the load-thunk indirection; the caller
  // is asserting they already know the pointer is alive.
  LIBC_INLINE void anchor(uint32_t index) {
    (void)do_update(era_.load(cpp::MemoryOrder::ACQUIRE), index);
  }

  // Submit `node` for deferred reclamation. Buffers into the per-thread
  // batch; fires try_retire every Freq-th retire.
  LIBC_INLINE void retire(NodeT *node) {
    if (node == nullptr)
      return;
    CrystallineBatch &batch = my_batch();
    if (!batch.first) {
      batch.last = node; // implicit upcast NodeT* → CrystallineNode*
      node->refs.store(kCrystallineProtect1, cpp::MemoryOrder::RELAXED);
    } else {
      // The anchor (batch.last) carries the minimum birth_era across
      // the batch — lets try_retire filter slots by era efficiently.
      NodeT *last = as_node(batch.last);
      if (last->birth_era > node->birth_era)
        last->birth_era = node->birth_era;
      // Encode the anchor reference. The codec produces a value in
      // [1, 0x7FFFFFFF] — 0 is reserved as the unretired sentinel,
      // bit 31 is reserved as the RNODE tag for the anchor's own
      // self-reference (set in the try_retire close path below).
      node->batch_link.store(BatchLinkCodec<NodeT>::encode(batch.last),
                             cpp::MemoryOrder::SEQ_CST);
      node->batch_next = as_node(batch.first);
    }

    batch.first = node;
    batch.counter++;
    if (batch.counter % Freq == 0) {
      // Mark anchor as "I'm the refs-node, and here's my chain head".
      // Bit 31 set + bottom 31 bits encode the chain head node `info`.
      as_node(batch.last)
          ->batch_link.store(
              kCrystallineBatchLinkRnodeBit | BatchLinkCodec<NodeT>::encode(node),
              cpp::MemoryOrder::SEQ_CST);
      try_retire(batch);
    }
  }

  // Drop every reservation on the calling thread. Used at API boundaries
  // where the caller has finished all dereferences and wants to let
  // retiring threads claim the slots.
  LIBC_INLINE void clear_all() {
    SlotT &my = my_slot_state();
    CrystallineBatch &batch = my_batch();
    CrystallineNode *first[MaxIdx];
    for (uint32_t i = 0; i < MaxIdx; i++) {
      first[i] = my.first[i].list[0].exchange(
          crystalline_inv_ptr(), cpp::MemoryOrder::ACQ_REL);
    }
    for (uint32_t i = 0; i < MaxIdx; i++) {
      if (first[i] != crystalline_inv_ptr())
        traverse(&batch.list, first[i]);
    }
    free_list(batch.list);
    batch.list = nullptr;
    batch.list_count = 0;
  }

  // ---------- Introspection ----------

  LIBC_INLINE uint64_t current_era() {
    return era_.load(cpp::MemoryOrder::ACQUIRE);
  }

  LIBC_INLINE static constexpr uint32_t reservation_slots() {
    return MaxIdx;
  }

  // Pre-claim this thread's slot in the domain's pool. Idempotent;
  // subsequent calls on the same thread short-circuit on the cached
  // index in `ThreadScratchState::crystalline_slot_idx[domain_id]`.
  //
  // Use case: a thread that may take its first protect()/retire() call
  // on this domain in a context where allocation isn't safe (VEH filter,
  // signal handler, loader-lock context). Calling `warm_thread()` once
  // during normal thread bring-up forces the slot-pool demand-commit
  // off the fault path. No reservation is held — the slot is claimed
  // and pushed onto the active chain, but every `first[i].list[0]` /
  // era field stays at the constructor-init tombstone until the first
  // real `protect()` call.
  LIBC_INLINE void warm_thread() { (void)my_slot_idx(); }

private:
  // -------- Static helpers --------

  // Thunk used by the atomic-pointer overload of protect(). Runs the
  // single ACQUIRE load that paper Fig. 10's `protect(loc, idx, p)`
  // does inline. Lives as a static member so its address is a single
  // file-scope function-pointer constant per CrystallineDomain<>
  // instantiation — same compile-time constant whether published from
  // the atomic-pointer overload's slow_path fall-through or invoked
  // directly from a foreign helper thread.
  LIBC_INLINE static NodeT *atomic_load_thunk(void *ctx) {
    return static_cast<cpp::Atomic<NodeT *> *>(ctx)->load(
        cpp::MemoryOrder::ACQUIRE);
  }

  // Type-erased→typed downcast at the substrate boundary. The slot
  // pool, batch chains, and CrystallineStateT all carry the universal
  // `CrystallineNode *` pointer; field access (`birth_era`, `refs`,
  // `next`, `batch_link`, ...) lives in the derived class via
  // LIBC_CRYSTALLINE_NODE_FIELDS, so any access through a CrystallineNode
  // pointer must downcast first. Sound because each CrystallineDomain<
  // NodeT, FreeFn> instance only ever stores `NodeT*`s into the
  // type-erased slots — the dynamic type is always NodeT.
  LIBC_INLINE static NodeT *as_node(CrystallineNode *p) {
    return static_cast<NodeT *>(p);
  }

  // -------- Instance state --------

  alignas(64) cpp::Atomic<uint64_t> era_{0};
  alignas(64) cpp::Atomic<uint64_t> slow_counter_{0};
  uint32_t domain_id_ = 0;
  // Value-init (`{}` not just `;`) so the defaulted constexpr default
  // ctor zero-fills every scalar subobject of the POD descriptor.
  // Without this the defaulted ctor isn't constexpr-eligible (scalars
  // would be indeterminate) and clang demotes it to a runtime ctor —
  // tripping -Wglobal-constructors on the file-scope g_substrate_domain.
  CrystallineDomainDescriptor descriptor_{};

  // -------- Per-thread accessors --------

  LIBC_INLINE internal::ThreadScratchState *my_thread() {
    auto *ts = ::LIBC_NAMESPACE::internal::get_thread_scratch();
    if (LIBC_UNLIKELY(ts == nullptr))
      __builtin_trap(); // ThreadScratch OOM — unrecoverable.
    return ts;
  }

  // Lazy-claim a slot in this domain's pool on first use. Idempotent
  // after the first call within a thread's lifetime.
  LIBC_INLINE uint16_t my_slot_idx() {
    auto *ts = my_thread();
    uint16_t idx = ts->crystalline_slot_idx[domain_id_];
    if (LIBC_UNLIKELY(idx == kCrystallineSlotNullIndex)) {
      idx = pool_.claim_slot();
      if (LIBC_UNLIKELY(idx == kCrystallineSlotNullIndex))
        __builtin_trap(); // pool capacity exhausted — unrecoverable.
      ts->crystalline_slot_idx[domain_id_] = idx;
    }
    return idx;
  }

  LIBC_INLINE SlotT &my_slot_state() {
    return pool_.at(my_slot_idx());
  }

  LIBC_INLINE CrystallineBatch &my_batch() {
    return my_thread()->crystalline_batches[domain_id_];
  }

  // Index-keyed slot accessor for cross-thread walks.
  LIBC_INLINE SlotT &slot_state_of(uint16_t idx) {
    return pool_.at(idx);
  }

  // Refresh the per-thread snapshot of the active-slot chain if the pool's
  // chain_version has moved since the last refresh. Returns true when the
  // cache is usable (a subsequent walk should iterate
  // batch.cached_active_slots[0 .. cached_count)); returns false when the
  // active chain at refresh time exceeded kCrystallineSnapshotCapacity —
  // the caller must fall through to direct chain traversal.
  //
  // Two staleness modes are tolerated by every walker call site:
  //
  //   (a) A slot claimed AFTER the snapshot. era[j].pair[0] = 0 from
  //       reset_slot_fields, max_era_seen = 0; min_era >= 1 (era_ starts
  //       at 1 in init_registration). Per-index era_v < min_era and the
  //       E8 max_era_seen < min_era fast-skip both exclude it; missing
  //       the slot is safe.
  //
  //   (b) A slot RELEASED after the snapshot. release_slot_trampoline
  //       drains every first[j].list[0] to crystalline_inv_ptr() before
  //       the harris splice; the per-index first == invptr filter
  //       excludes the slot. max_era_seen may still carry a stale-high
  //       value from the prior owner, but the invptr filter fires before
  //       any era check.
  //
  // Lemma 5.2 / 5.3 bounds preserved: the cache shortens the eligibility
  // loop from |active chain| to |cached|, and |cached| <= |active chain|
  // by construction. The overflow path reverts to the paper's exact
  // loop bound.
  LIBC_INLINE bool refresh_active_snapshot(CrystallineBatch &batch) {
    uint64_t cv = pool_.chain_version();
    if (cv == batch.cached_chain_version && batch.cached_overflow == 0)
      return true;
    uint32_t count = 0;
    for (uint16_t idx = pool_.active_head(); idx != 0;
         idx = pool_.active_next(idx)) {
      if (count >= kCrystallineSnapshotCapacity) {
        batch.cached_overflow = 1;
        batch.cached_count = 0;
        batch.cached_chain_version = cv;
        return false;
      }
      batch.cached_active_slots[count++] = idx;
    }
    batch.cached_count = count;
    batch.cached_overflow = 0;
    batch.cached_chain_version = cv;
    return true;
  }

  // -----------------------------------------------------------------------
  // Reclamation helpers — ported from the reference, no algorithmic
  // changes.
  // -----------------------------------------------------------------------

  // Resolve the anchor ("refs") node for a given batch member.
  // Bit 31 set on `batch_link` means `node` IS its own anchor; otherwise
  // the bottom 31 bits decode through the codec to the anchor.
  LIBC_INLINE CrystallineNode *get_refs_node(CrystallineNode *node) {
    NodeT *n = as_node(node);
    uint32_t code = n->batch_link.load(cpp::MemoryOrder::ACQUIRE);
    if ((code & kCrystallineBatchLinkRnodeBit) != 0)
      return node;
    return BatchLinkCodec<NodeT>::decode(code & kCrystallineBatchLinkCodeMask);
  }

  // Walk a detached chain `next`, decrement each node's batch_link's
  // refs, and splice any fully-released anchors onto `*list`. Used by
  // do_update, slow_path, and clear_all to drain slot chains we
  // exchanged out.
  LIBC_INLINE void traverse(CrystallineNode **list, CrystallineNode *next) {
    while (true) {
      CrystallineNode *curr = next;
      if (!curr)
        break;
      if (crystalline_is_rnode(curr)) {
        // Terminal refs-node reached via the chain pointer's bit-0
        // tag: we've walked the full chain, decrement the anchor's
        // share and stop.
        CrystallineNode *refs_cn =
            reinterpret_cast<CrystallineNode *>(
                reinterpret_cast<uintptr_t>(curr) ^ 1U);
        NodeT *refs = as_node(refs_cn);
        if (refs->refs.fetch_sub(1, cpp::MemoryOrder::ACQ_REL) == 1) {
          refs->cn_next.store(as_node(*list), cpp::MemoryOrder::RELAXED);
          *list = refs_cn;
        }
        break;
      }
      NodeT *curr_node = as_node(curr);
      next = curr_node->cn_next.exchange(crystalline_inv_ptr<NodeT>(),
                                      cpp::MemoryOrder::ACQ_REL);
      // Non-anchor chain entry: decode batch_link to the anchor.
      // The encoded code never has bit 31 set in this path (bit 31
      // marks the anchor's self-reference, which is handled above
      // via the chain pointer's bit-0 tag — disjoint encoding).
      uint32_t code = curr_node->batch_link.load(cpp::MemoryOrder::RELAXED);
      CrystallineNode *refs_cn =
          BatchLinkCodec<NodeT>::decode(code & kCrystallineBatchLinkCodeMask);
      NodeT *refs = as_node(refs_cn);
      if (refs->refs.fetch_sub(1, cpp::MemoryOrder::ACQ_REL) == 1) {
        refs->cn_next.store(as_node(*list), cpp::MemoryOrder::RELAXED);
        *list = refs_cn;
      }
    }
  }

  // traverse() with a per-thread cache bound — if the free cache is
  // already at kCrystallineFreeCacheCap, flush it before walking again.
  LIBC_INLINE void traverse_cache(CrystallineBatch &batch,
                                  CrystallineNode *next) {
    if (next != nullptr) {
      if (batch.list_count == kCrystallineFreeCacheCap) {
        free_list(batch.list);
        batch.list = nullptr;
        batch.list_count = 0;
      }
      traverse(&batch.list, next);
      batch.list_count++;
    }
  }

  // Walk every refs-node in `list` and reclaim every node in its batch
  // via FreeFn. The list chains through each anchor's `next` field, and
  // each anchor's batch chains through `batch_next`.
  LIBC_INLINE void free_list(CrystallineNode *list) {
    while (list != nullptr) {
      NodeT *list_node = as_node(list);
      // The anchor's batch_link carries (RNODE_BIT | encode(chain_head))
      // — see retire's try_retire path. Mask off the RNODE bit; the
      // bottom 31 bits decode to the chain head. A RELAXED load
      // suffices — the anchor was exclusively claimed by the
      // refs->refs wrap-to-zero above.
      uint32_t code = list_node->batch_link.load(cpp::MemoryOrder::RELAXED);
      NodeT *start = as_node(
          BatchLinkCodec<NodeT>::decode(code & kCrystallineBatchLinkCodeMask));
      list = list_node->cn_next.load(cpp::MemoryOrder::RELAXED);
      do {
        NodeT *obj = start;
        start = obj->batch_next;
        FreeFn(obj);
      } while (start != nullptr);
    }
  }

  // -----------------------------------------------------------------------
  // do_update — era refresh and chain drain on the caller's slot.
  // -----------------------------------------------------------------------
  //
  // Called from the protect() fast-path loop whenever the observed era
  // advances. Detaches any pending chain on first[index], cache-traverses
  // it (drops refs on freed batches), then publishes the new
  // current_era into era[index].pair[0].
  LIBC_INLINE uint64_t do_update(uint64_t curr_era, uint32_t index) {
    SlotT &my = my_slot_state();
    CrystallineBatch &batch = my_batch();
    if (my.first[index].list[0].load(cpp::MemoryOrder::ACQUIRE) != nullptr) {
      CrystallineNode *first = my.first[index].list[0].exchange(
          crystalline_inv_ptr(), cpp::MemoryOrder::ACQ_REL);
      if (first != crystalline_inv_ptr())
        traverse_cache(batch, first);
      my.first[index].list[0].store(nullptr, cpp::MemoryOrder::SEQ_CST);
      curr_era = current_era();
    }
    my.era[index].pair[0].store(curr_era, cpp::MemoryOrder::SEQ_CST);
    // E8 mirror — keep max_era_seen tracking the largest era ever
    // published into this slot's era[] array so try_retire Phase A
    // can fast-skip stale slots with one RELAXED load. Owner-exclusive
    // write on the owner's own slot; no CAS needed.
    uint64_t prev_max = my.max_era_seen.load(cpp::MemoryOrder::RELAXED);
    if (curr_era > prev_max)
      my.max_era_seen.store(curr_era, cpp::MemoryOrder::RELAXED);
    return curr_era;
  }

  // -----------------------------------------------------------------------
  // slow_path — the wait-free fallback when the fast path's attempts
  // are exhausted. Publishes a help-request via `state[index].result =
  // {WFR_INVPTR64, seqno}` plus the (load_thunk, load_ctx) pair that
  // helpers will invoke to produce a value, waits for either a self-
  // observed era match or a helper-produced result, then reconciles
  // the returned (era, ptr) pair with any pending list and parent
  // handoff.
  //
  // Generalization vs. paper Fig. 13: the reference stores `obj`
  // (the atomic-pointer address) and helpers redo the load via
  // `obj->load()`. We publish a (thunk, ctx) pair so the helper's
  // load can be a multi-step computation (ART scan, encoded-link
  // decode). Atomic-pointer protect() feeds through the same path
  // via the `atomic_load_thunk` static member.
  // -----------------------------------------------------------------------
  LIBC_INLINE NodeT *slow_path(NodeT *(*load_thunk)(void *),
                               void *load_ctx, uint32_t index,
                               NodeT *node) {
    SlotT &my = my_slot_state();
    CrystallineBatch &batch = my_batch();

    // Compute the birth era for the parent-node reference we're
    // holding. If the parent has already been retired AND the parent
    // is NOT its own batch's anchor (RNODE bit clear), decode the
    // anchor and use its birth_era (the min across the retired batch)
    // instead of the parent's own — protects us from ABA where the
    // parent was reclaimed while we were mid-traversal.
    uint64_t birth_era = 0;
    CrystallineNode *parent = nullptr;
    if (node != nullptr) {
      parent = node; // implicit upcast NodeT* → CrystallineNode*
      birth_era = node->birth_era;
      uint32_t code = node->batch_link.load(cpp::MemoryOrder::ACQUIRE);
      if (code != 0 && (code & kCrystallineBatchLinkRnodeBit) == 0) {
        NodeT *info = as_node(BatchLinkCodec<NodeT>::decode(
            code & kCrystallineBatchLinkCodeMask));
        birth_era = info->birth_era;
      }
    }

    uint64_t prev_era =
        my.era[index].pair[0].load(cpp::MemoryOrder::ACQUIRE);
    // Publish (load_ctx, load_thunk) for helpers. Order: ctx RELEASE
    // first, thunk RELEASE last; helpers consume thunk ACQUIRE first
    // and on a non-null observation re-load ctx ACQUIRE. The thunk
    // pointer is the publication anchor — non-null thunk ⇒ ctx
    // happens-before-visible.
    my.state[index].load_ctx.store(load_ctx, cpp::MemoryOrder::RELEASE);
    my.state[index].load_thunk.store(
        reinterpret_cast<CrystallineNode *(*)(void *)>(load_thunk),
        cpp::MemoryOrder::RELEASE);
    my.state[index].parent.store(parent, cpp::MemoryOrder::RELEASE);
    my.state[index].birth_era.store(birth_era, cpp::MemoryOrder::RELEASE);
    uint64_t seqno =
        my.era[index].pair[1].load(cpp::MemoryOrder::ACQUIRE);

    CrystallineValuePair last_result;
    last_result.pair[0] = kCrystallineInvPtr64;
    last_result.pair[1] = seqno;
    my.state[index].result.full.store(last_result.full,
                                      cpp::MemoryOrder::RELEASE);

    // Bump the monotonic slow-path generation AFTER the state fields
    // above are published. Helpers gate on this counter and ACQUIRE-
    // load it before scanning state[index]; the ACQ_REL fetch_add is
    // the publication fence that pairs with their ACQUIRE load, so a
    // helper that observes gen ≥ this value is guaranteed to also see
    // load_ctx / load_thunk / parent / birth_era / result.full in their
    // freshly-published state. Counter is write-only (never decremented
    // at slow_path exit); per-thread last_helped_slow_gen drives the
    // gate. See help_read.
    slow_counter_.fetch_add(1, cpp::MemoryOrder::ACQ_REL);

    CrystallineValuePair old, value;
    uint64_t result_era, result_ptr, expseqno;
    CrystallineNode *first;
    do {
      NodeT *ptr = load_thunk ? load_thunk(load_ctx) : nullptr;
      uint64_t curr_era = current_era();
      if (curr_era == prev_era) {
        last_result.pair[0] = kCrystallineInvPtr64;
        last_result.pair[1] = seqno;
        value.pair[0] = 0;
        value.pair[1] = 0;
        if (my.state[index].result.full.compare_exchange_strong(
                last_result.full, value.full,
                cpp::MemoryOrder::ACQ_REL,
                cpp::MemoryOrder::ACQUIRE)) {
          my.era[index].pair[1].store(seqno + 2,
                                        cpp::MemoryOrder::RELEASE);
          my.first[index].pair[1].store(seqno + 2,
                                        cpp::MemoryOrder::RELEASE);
          return ptr;
        }
      }
      if (my.first[index].list[0].load(cpp::MemoryOrder::ACQUIRE) !=
          nullptr) {
        first = my.first[index].list[0].exchange(
            nullptr, cpp::MemoryOrder::ACQ_REL);
        if (my.first[index].pair[1].load(cpp::MemoryOrder::ACQUIRE) !=
            seqno)
          goto done;
        if (first != crystalline_inv_ptr())
          traverse_cache(batch, first);
        curr_era = current_era();
      }
      first = nullptr;
      old.pair[0] = prev_era;
      old.pair[1] = seqno;
      value.pair[0] = curr_era;
      value.pair[1] = seqno;
      my.era[index].full.compare_exchange_strong(
          old.full, value.full, cpp::MemoryOrder::SEQ_CST,
          cpp::MemoryOrder::ACQUIRE);
      // E8 mirror — `my` is the slot owner running its own slow_path,
      // so the bump is owner-exclusive. We bump unconditionally on
      // curr_era regardless of CAS outcome: a CAS-success publishes
      // curr_era; a CAS-failure means a helper raced and may have set
      // era to an even higher value, which the helpee re-publishes at
      // line ~853 below (also mirrored).
      {
        uint64_t prev_max = my.max_era_seen.load(cpp::MemoryOrder::RELAXED);
        if (curr_era > prev_max)
          my.max_era_seen.store(curr_era, cpp::MemoryOrder::RELAXED);
      }
      prev_era = curr_era;
      result_ptr =
          my.state[index].result.pair[0].load(cpp::MemoryOrder::ACQUIRE);
    } while (result_ptr == kCrystallineInvPtr64);

    // Empty-era seqno advance.
    expseqno = seqno;
    my.era[index].pair[1].compare_exchange_strong(
        expseqno, seqno + 1, cpp::MemoryOrder::ACQ_REL,
        cpp::MemoryOrder::RELAXED);
    value.list[0] = nullptr;
    value.pair[1] = seqno + 1;
    old.pair[1] =
        my.first[index].pair[1].load(cpp::MemoryOrder::ACQUIRE);
    old.list[0] =
        my.first[index].list[0].load(cpp::MemoryOrder::ACQUIRE);
    while (old.pair[1] == seqno) { // n iterations at most
      if (my.first[index].full.compare_exchange_weak(
              old.full, value.full, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::ACQUIRE)) {
        if (old.list[0] != crystalline_inv_ptr())
          first = old.list[0];
        break;
      }
    }

  done:
    seqno++;

    // Publish the produced era into this slot's visible era.
    my.era[index].pair[1].store(seqno + 1, cpp::MemoryOrder::RELEASE);
    result_era =
        my.state[index].result.pair[1].load(cpp::MemoryOrder::ACQUIRE);
    my.era[index].pair[0].store(result_era,
                                  cpp::MemoryOrder::RELEASE);
    // E8 mirror — owner-exclusive write on the owner's own slot. This
    // is also the recovery point that closes the brief window where a
    // helper at help_thread's WCAS site advanced their.era[index].pair[0]
    // beyond max_era_seen: as soon as the helpee returns through this
    // path it re-publishes the helper-set era, sweeping max_era_seen up.
    {
      uint64_t prev_max = my.max_era_seen.load(cpp::MemoryOrder::RELAXED);
      if (result_era > prev_max)
        my.max_era_seen.store(result_era, cpp::MemoryOrder::RELAXED);
    }

    // Check whether the produced pointer was retired while we were
    // in slow_path. If so, attach a refs-node reference onto our
    // first[index] chain so we retain a hold on the batch.
    my.first[index].pair[1].store(seqno + 1, cpp::MemoryOrder::RELEASE);
    result_ptr =
        my.state[index].result.pair[0].load(cpp::MemoryOrder::ACQUIRE) &
        0xFFFFFFFFFFFFFFFCULL;
    auto *ptr_node = reinterpret_cast<NodeT *>(result_ptr);
    if (result_ptr != 0 &&
        ptr_node->batch_link.load(cpp::MemoryOrder::ACQUIRE) != 0) {
      CrystallineNode *refs_cn = get_refs_node(ptr_node);
      NodeT *refs = as_node(refs_cn);
      refs->refs.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
      if (first != crystalline_inv_ptr())
        traverse_cache(batch, first);
      first = my.first[index].list[0].exchange(
          crystalline_rnode(refs_cn), cpp::MemoryOrder::ACQ_REL);
    }

    if (first != crystalline_inv_ptr())
      traverse_cache(batch, first);

    // Parent-handoff: if the parent node was retired while we were
    // slow-pathing, increment its anchor refs by PROTECT2, then CAS
    // every live thread's state[hr_num].parent field to claim back
    // the references they would have held. Net delta is added once.
    if (parent != nullptr &&
        as_node(parent)->batch_link.load(cpp::MemoryOrder::ACQUIRE) != 0) {
      NodeT *refs = as_node(get_refs_node(parent));
      refs->refs.fetch_add(kCrystallineProtect2,
                           cpp::MemoryOrder::ACQ_REL);
      uintptr_t adjs = static_cast<uintptr_t>(-kCrystallineProtect2);
      if (refresh_active_snapshot(batch)) {
        for (uint32_t k = 0; k < batch.cached_count; k++) {
          uint16_t i = batch.cached_active_slots[k];
          SlotT &their = slot_state_of(i);
          CrystallineNode *exp = parent;
          if (their.state[MaxIdx].parent.compare_exchange_strong(
                  exp, nullptr, cpp::MemoryOrder::ACQ_REL,
                  cpp::MemoryOrder::RELAXED)) {
            adjs++;
          }
        }
      } else {
        for (uint16_t i = pool_.active_head(); i != 0;
             i = pool_.active_next(i)) {
          SlotT &their = slot_state_of(i);
          CrystallineNode *exp = parent;
          if (their.state[MaxIdx].parent.compare_exchange_strong(
                  exp, nullptr, cpp::MemoryOrder::ACQ_REL,
                  cpp::MemoryOrder::RELAXED)) {
            adjs++;
          }
        }
      }
      refs->refs.fetch_add(adjs, cpp::MemoryOrder::ACQ_REL);
    }

    return reinterpret_cast<NodeT *>(result_ptr);
  }

  // -----------------------------------------------------------------------
  // help_thread — wait-free helping protocol for a stalled slow_path
  // on some other thread `target`. Produces a (ptr, era) into the
  // target's result word if a consistent era observation is possible.
  //
  // Loads via the helpee's published (load_thunk, load_ctx) pair —
  // see slow_path's matching publication.
  // -----------------------------------------------------------------------
  LIBC_INLINE void help_thread(uint16_t target_idx, uint32_t index,
                               uint16_t my_idx) {
    SlotT &their = slot_state_of(target_idx);
    SlotT &my = slot_state_of(my_idx);
    CrystallineBatch &my_b = my_batch();

    CrystallineValuePair last_result;
    last_result.full = their.state[index].result.full.load(
        cpp::MemoryOrder::ACQUIRE);
    if (last_result.pair[0] != kCrystallineInvPtr64)
      return;
    uint64_t birth_era =
        their.state[index].birth_era.load(cpp::MemoryOrder::ACQUIRE);
    CrystallineNode *parent =
        their.state[index].parent.load(cpp::MemoryOrder::ACQUIRE);
    if (parent != nullptr) {
      my.first[MaxIdx].list[0].store(
          nullptr, cpp::MemoryOrder::SEQ_CST);
      my.era[MaxIdx].pair[0].store(
          birth_era, cpp::MemoryOrder::SEQ_CST);
      // E8 mirror — helper writes its OWN scratch slot
      // (MaxIdx lives in `my`, the helper thread's slot),
      // so the bump is owner-exclusive.
      uint64_t prev_max = my.max_era_seen.load(cpp::MemoryOrder::RELAXED);
      if (birth_era > prev_max)
        my.max_era_seen.store(birth_era, cpp::MemoryOrder::RELAXED);
    }
    my.state[MaxIdx].parent.store(parent,
                                             cpp::MemoryOrder::SEQ_CST);
    // Consume helpee's (load_thunk, load_ctx). Order: thunk ACQUIRE
    // first; on non-null thunk, ctx ACQUIRE pairs with the helpee's
    // RELEASE writes.
    auto *thunk = reinterpret_cast<NodeT *(*)(void *)>(
        their.state[index].load_thunk.load(cpp::MemoryOrder::ACQUIRE));
    void *ctx = thunk ? their.state[index].load_ctx.load(
                            cpp::MemoryOrder::ACQUIRE)
                      : nullptr;
    uint64_t seqno =
        their.era[index].pair[1].load(cpp::MemoryOrder::ACQUIRE);
    if (last_result.pair[1] == seqno) {
      uint64_t prev_era = current_era();
      do {
        // Use our OWN slot MaxIdx+1 as the helper's
        // dereference workspace. do_update on our slot refreshes the
        // era publication without perturbing the target's state.
        prev_era = do_update_on(my, my_b, prev_era,
                                  MaxIdx + 1);
        NodeT *ptr = thunk ? thunk(ctx) : nullptr;
        uint64_t curr_era = current_era();
        if (curr_era == prev_era) {
          CrystallineValuePair value;
          value.pair[0] = reinterpret_cast<uint64_t>(ptr);
          value.pair[1] = curr_era;
          if (their.state[index].result.full.compare_exchange_strong(
                  last_result.full, value.full,
                  cpp::MemoryOrder::ACQ_REL,
                  cpp::MemoryOrder::ACQUIRE)) {
            // Empty-era transition on the target's seqno (best-
            // effort; another helper may have done it already).
            uint64_t expseqno = seqno;
            their.era[index].pair[1].compare_exchange_strong(
                expseqno, seqno + 1, cpp::MemoryOrder::ACQ_REL,
                cpp::MemoryOrder::RELAXED);
            // Clean up the pending list on the target's first[index].
            value.list[0] = nullptr;
            value.pair[1] = seqno + 1;
            CrystallineValuePair old_val;
            old_val.pair[1] = their.first[index].pair[1].load(
                cpp::MemoryOrder::ACQUIRE);
            old_val.list[0] = their.first[index].list[0].load(
                cpp::MemoryOrder::ACQUIRE);
            while (old_val.pair[1] == seqno) {
              if (their.first[index].full.compare_exchange_weak(
                      old_val.full, value.full,
                      cpp::MemoryOrder::ACQ_REL,
                      cpp::MemoryOrder::ACQUIRE)) {
                if (old_val.list[0] != crystalline_inv_ptr())
                  traverse_cache(my_b, old_val.list[0]);
                break;
              }
            }
            seqno++;
            // Set the real era on the target's era slot. E8 design
            // note: this is the one foreign-thread write to era[].pair[0]
            // in the substrate. It is intentionally NOT mirrored into
            // their.max_era_seen — the helpee re-publishes the same era
            // via slow_path's final my.era[index].pair[0].store at
            // line ~853, which DOES mirror, sweeping max_era_seen up
            // shortly after. The window is bounded by the helpee's
            // slow_path tail.
            value.pair[0] = curr_era;
            value.pair[1] = seqno + 1;
            old_val.pair[1] = their.era[index].pair[1].load(
                cpp::MemoryOrder::ACQUIRE);
            old_val.pair[0] = their.era[index].pair[0].load(
                cpp::MemoryOrder::ACQUIRE);
            while (old_val.pair[1] == seqno) { // 2 iterations at most
              if (their.era[index].full.compare_exchange_weak(
                      old_val.full, value.full,
                      cpp::MemoryOrder::ACQ_REL,
                      cpp::MemoryOrder::ACQUIRE)) {
                break;
              }
            }
            // Check whether the produced ptr is already retired; if
            // so, attach a refs-node reference onto the target's
            // first[index] chain so they retain the hold.
            uint64_t ptr_val = reinterpret_cast<uint64_t>(ptr) &
                               0xFFFFFFFFFFFFFFFCULL;
            auto *ptr_node = reinterpret_cast<NodeT *>(ptr_val);
            if (ptr_val != 0 &&
                ptr_node->batch_link.load(
                    cpp::MemoryOrder::ACQUIRE) != 0) {
              CrystallineNode *refs_cn = get_refs_node(ptr_node);
              NodeT *refs = as_node(refs_cn);
              refs->refs.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
              value.list[0] = crystalline_rnode(refs_cn);
              value.pair[1] = seqno + 1;
              old_val.pair[1] = their.first[index].pair[1].load(
                  cpp::MemoryOrder::ACQUIRE);
              old_val.list[0] = their.first[index].list[0].load(
                  cpp::MemoryOrder::ACQUIRE);
              while (old_val.pair[1] == seqno) {
                if (their.first[index].full.compare_exchange_weak(
                        old_val.full, value.full,
                        cpp::MemoryOrder::ACQ_REL,
                        cpp::MemoryOrder::ACQUIRE)) {
                  if (old_val.list[0] != crystalline_inv_ptr())
                    traverse_cache(my_b, old_val.list[0]);
                  goto done;
                }
              }
              // Already inserted by someone else — drop our extra ref.
              refs->refs.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
            } else {
              // Empty-list transition.
              uint64_t expseqno2 = seqno;
              their.first[index].pair[1].compare_exchange_strong(
                  expseqno2, seqno + 1, cpp::MemoryOrder::ACQ_REL,
                  cpp::MemoryOrder::RELAXED);
            }
          }
          break;
        }
        prev_era = curr_era;
      } while (last_result.full ==
               their.state[index].result.full.load(
                   cpp::MemoryOrder::ACQUIRE));
    done:
      if (my.era[MaxIdx + 1].pair[0].exchange(
              0, cpp::MemoryOrder::SEQ_CST) != 0) {
        CrystallineNode *first =
            my.first[MaxIdx + 1].list[0].exchange(
                crystalline_inv_ptr(), cpp::MemoryOrder::ACQ_REL);
        traverse_cache(my_b, first);
      }
    }
    // If the helpee handed us a parent reservation reference we no
    // longer own, release it.
    if (my.state[MaxIdx].parent.exchange(
            nullptr, cpp::MemoryOrder::SEQ_CST) != parent) {
      CrystallineNode *refs_cn = get_refs_node(parent);
      NodeT *refs = as_node(refs_cn);
      if (refs->refs.fetch_sub(1, cpp::MemoryOrder::ACQ_REL) == 1) {
        refs->cn_next.store(as_node(my_b.list), cpp::MemoryOrder::RELAXED);
        my_b.list = refs_cn;
      }
    }
    if (my.era[MaxIdx].pair[0].exchange(
            0, cpp::MemoryOrder::SEQ_CST) != 0) {
      CrystallineNode *first =
          my.first[MaxIdx].list[0].exchange(
              crystalline_inv_ptr(), cpp::MemoryOrder::ACQ_REL);
      traverse_cache(my_b, first);
    }
    free_list(my_b.list);
    my_b.list = nullptr;
    my_b.list_count = 0;
  }

  // help_read — scan every live slot in the pool for a stalled
  // slow_path (state[j].result.pair[0] == WFR_INVPTR64) and help it.
  //
  // Self-gating: `slow_counter_` is a monotonic generation (bumped once
  // per slow_path entry, never decremented). Each thread records the
  // largest gen it has already helped against in `ts->crystalline_
  // last_helped_slow_gen[domain_id_]`. The walk fires only when the
  // current counter differs from the recorded value, then the recorded
  // value advances BEFORE the walk so that a second back-to-back
  // help_read on the same thread without an intervening slow_path is a
  // no-op. last_helped is updated pre-walk so that even if the walk
  // finds no INVPTR64 (e.g. all stalled slow_paths happen to complete
  // between our gen load and the walk) the bookkeeping is correct:
  // subsequent slow_path bumps push the counter past last_helped and
  // re-enable the walk.
  LIBC_INLINE void help_read() {
    uint64_t gen = slow_counter_.load(cpp::MemoryOrder::ACQUIRE);
    auto *ts = my_thread();
    if (gen == ts->crystalline_last_helped_slow_gen[domain_id_])
      return;
    ts->crystalline_last_helped_slow_gen[domain_id_] = gen;
    uint16_t my_idx = my_slot_idx();
    CrystallineBatch &batch = my_batch();
    if (refresh_active_snapshot(batch)) {
      for (uint32_t k = 0; k < batch.cached_count; k++) {
        uint16_t target_idx = batch.cached_active_slots[k];
        SlotT &their = slot_state_of(target_idx);
        for (uint32_t j = 0; j < MaxIdx; j++) {
          uint64_t result_ptr = their.state[j].result.pair[0].load(
              cpp::MemoryOrder::ACQUIRE);
          if (result_ptr == kCrystallineInvPtr64) {
            help_thread(target_idx, j, my_idx);
          }
        }
      }
    } else {
      for (uint16_t target_idx = pool_.active_head(); target_idx != 0;
           target_idx = pool_.active_next(target_idx)) {
        SlotT &their = slot_state_of(target_idx);
        for (uint32_t j = 0; j < MaxIdx; j++) {
          uint64_t result_ptr = their.state[j].result.pair[0].load(
              cpp::MemoryOrder::ACQUIRE);
          if (result_ptr == kCrystallineInvPtr64) {
            help_thread(target_idx, j, my_idx);
          }
        }
      }
    }
  }

  // do_update variant for help_thread that operates on an explicit
  // slot/batch rather than the calling thread's own. Body identical
  // to do_update modulo the arguments.
  LIBC_INLINE uint64_t do_update_on(SlotT &my,
                                    CrystallineBatch &batch,
                                    uint64_t curr_era, uint32_t index) {
    if (my.first[index].list[0].load(cpp::MemoryOrder::ACQUIRE) != nullptr) {
      CrystallineNode *first = my.first[index].list[0].exchange(
          crystalline_inv_ptr(), cpp::MemoryOrder::ACQ_REL);
      if (first != crystalline_inv_ptr())
        traverse_cache(batch, first);
      my.first[index].list[0].store(nullptr, cpp::MemoryOrder::SEQ_CST);
      curr_era = current_era();
    }
    my.era[index].pair[0].store(curr_era, cpp::MemoryOrder::SEQ_CST);
    // E8 mirror — `my` here is help_thread's own slot (the helper writes
    // its scratch slot MaxIdx+1 via this path), so the write
    // remains owner-exclusive.
    uint64_t prev_max = my.max_era_seen.load(cpp::MemoryOrder::RELAXED);
    if (curr_era > prev_max)
      my.max_era_seen.store(curr_era, cpp::MemoryOrder::RELAXED);
    return curr_era;
  }

  // -----------------------------------------------------------------------
  // try_retire — publish the current batch across live-thread slots
  // using the modular-addend refs adjustment. Directly ports lines
  // 535-611 of the reference.
  // -----------------------------------------------------------------------
  LIBC_INLINE void try_retire(CrystallineBatch &batch) {
    // Help any stalled slow paths before walking the active chain.
    // help_read self-gates on the per-thread last_helped_slow_gen, so
    // a redundant outer gate would just duplicate that load.
    help_read();

    NodeT *curr = as_node(batch.first);
    NodeT *refs = as_node(batch.last);
    uint64_t min_era = refs->birth_era;

    // Phase A — walk every claimed pool slot and claim a batch node
    // for each eligible slot. Claim = write the slot's first word-pair
    // address into the node's `slot` field (first union's pointer alias
    // of `next`). Stops early if we run out of batch nodes.
    //
    // Two-layer fast-skip:
    //   * Outer: cached active-slot snapshot iterated when the pool's
    //     chain_version still matches the cache. Avoids the per-walker
    //     active-chain traversal on every retire close.
    //   * Inner: a single RELAXED max_era_seen load per slot rejects
    //     slots whose largest-reserved era is already < min_era — no
    //     per-index era[j] in the slot could pass the eligibility
    //     check, so the inner per-index loop is skipped entirely.
    NodeT *last = curr;
    if (refresh_active_snapshot(batch)) {
      for (uint32_t k = 0; k < batch.cached_count; k++) {
        uint16_t i = batch.cached_active_slots[k];
        SlotT &their = slot_state_of(i);
        if (their.max_era_seen.load(cpp::MemoryOrder::RELAXED) < min_era)
          continue;
        uint32_t j = 0;
        for (; j < MaxIdx; j++) {
          CrystallineNode *first = their.first[j].list[0].load(
              cpp::MemoryOrder::ACQUIRE);
          if (first == crystalline_inv_ptr())
            continue;
          if (their.first[j].pair[1].load(cpp::MemoryOrder::ACQUIRE) & 0x1U)
            continue; // in slow-path final transition
          uint64_t era_v =
              their.era[j].pair[0].load(cpp::MemoryOrder::ACQUIRE);
          if (era_v < min_era)
            continue;
          if (their.era[j].pair[1].load(cpp::MemoryOrder::ACQUIRE) & 0x1U)
            continue;
          if (last == refs) {
            return;
          }
          last->slot = &their.first[j];
          last = last->batch_next;
        }
        // Helper slots hr_num and hr_num+1 don't carry the seqno filter
        // — they're one-shot scratch used by the helping protocol.
        for (; j < MaxIdx + 2; j++) {
          CrystallineNode *first = their.first[j].list[0].load(
              cpp::MemoryOrder::ACQUIRE);
          if (first == crystalline_inv_ptr())
            continue;
          uint64_t era_v =
              their.era[j].pair[0].load(cpp::MemoryOrder::ACQUIRE);
          if (era_v < min_era)
            continue;
          if (last == refs) {
            return;
          }
          last->slot = &their.first[j];
          last = last->batch_next;
        }
      }
    } else {
      // Snapshot overflowed — fall back to direct chain traversal. E8
      // fast-skip still applies per slot.
      for (uint16_t i = pool_.active_head(); i != 0;
           i = pool_.active_next(i)) {
        SlotT &their = slot_state_of(i);
        if (their.max_era_seen.load(cpp::MemoryOrder::RELAXED) < min_era)
          continue;
        uint32_t j = 0;
        for (; j < MaxIdx; j++) {
          CrystallineNode *first = their.first[j].list[0].load(
              cpp::MemoryOrder::ACQUIRE);
          if (first == crystalline_inv_ptr())
            continue;
          if (their.first[j].pair[1].load(cpp::MemoryOrder::ACQUIRE) & 0x1U)
            continue;
          uint64_t era_v =
              their.era[j].pair[0].load(cpp::MemoryOrder::ACQUIRE);
          if (era_v < min_era)
            continue;
          if (their.era[j].pair[1].load(cpp::MemoryOrder::ACQUIRE) & 0x1U)
            continue;
          if (last == refs) {
            return;
          }
          last->slot = &their.first[j];
          last = last->batch_next;
        }
        for (; j < MaxIdx + 2; j++) {
          CrystallineNode *first = their.first[j].list[0].load(
              cpp::MemoryOrder::ACQUIRE);
          if (first == crystalline_inv_ptr())
            continue;
          uint64_t era_v =
              their.era[j].pair[0].load(cpp::MemoryOrder::ACQUIRE);
          if (era_v < min_era)
            continue;
          if (last == refs) {
            return;
          }
          last->slot = &their.first[j];
          last = last->batch_next;
        }
      }
    }

    // Phase B — push every claimed batch node onto its assigned slot's
    // first[].list[0]. adjs tracks how many publishes succeeded; the
    // anchor's refs is incremented by (adjs - PROTECT1) so that once
    // every publish is later drained by a reader (decrementing refs by
    // one each), the total wraps to zero and free_list fires.
    uintptr_t adjs = static_cast<uintptr_t>(-kCrystallineProtect1);
    for (; curr != last; curr = curr->batch_next) {
      CrystallineWordPair *slot_first = curr->slot;
      CrystallineWordPair *slot_era_field = slot_first + (MaxIdx + 2);
      curr->cn_next.store(nullptr, cpp::MemoryOrder::RELAXED);
      if (slot_first->list[0].load(cpp::MemoryOrder::ACQUIRE) ==
          crystalline_inv_ptr())
        continue;
      uint64_t era_v =
          slot_era_field->pair[0].load(cpp::MemoryOrder::ACQUIRE);
      if (era_v < min_era)
        continue;
      CrystallineNode *prev = slot_first->list[0].exchange(
          curr, cpp::MemoryOrder::ACQ_REL);
      if (prev != nullptr) {
        if (prev == crystalline_inv_ptr()) {
          // Transitioning — slot just moved into INVPTR state. Try to
          // roll back our install so the transition owner sees a clean
          // state.
          CrystallineNode *exp = curr;
          if (slot_first->list[0].compare_exchange_strong(
                  exp, crystalline_inv_ptr(),
                  cpp::MemoryOrder::ACQ_REL,
                  cpp::MemoryOrder::RELAXED)) {
            continue;
          }
        } else {
          // Slot had a pending chain; graft it onto our new head.
          // `curr->cn_next` is Atomic<NodeT*>, so the CAS's expected/
          // desired must be NodeT*; downcast `prev` from the
          // type-erased slot view.
          NodeT *exp = nullptr;
          NodeT *prev_node = as_node(prev);
          if (!curr->cn_next.compare_exchange_strong(
                  exp, prev_node, cpp::MemoryOrder::ACQ_REL,
                  cpp::MemoryOrder::RELAXED)) {
            // Lost the graft race — someone already linked curr.
            // Drain `prev` ourselves.
            CrystallineNode *list = nullptr;
            traverse(&list, prev);
            free_list(list);
          }
        }
      }
      adjs++;
    }
    // Roll the adjs into the anchor's refs. If it wraps to zero the
    // batch is reclaim-ready.
    if (refs->refs.fetch_add(adjs, cpp::MemoryOrder::ACQ_REL) ==
        static_cast<uintptr_t>(-adjs)) {
      refs->cn_next.store(nullptr, cpp::MemoryOrder::RELAXED);
      free_list(refs);
    }
    // Era advances on retire close — drives protect()'s convergence so
    // readers learn that a retirement happened and may need to drain.
    era_.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
    batch.first = nullptr;
    batch.counter = 0;
  }

  // -----------------------------------------------------------------------
  // Registry trampolines (static, called through CrystallineDomainDescriptor).
  // -----------------------------------------------------------------------

  LIBC_INLINE static void fork_reinit_trampoline(void *context) {
    auto *self = static_cast<CrystallineDomain *>(context);
    self->fork_reinit();
  }

  LIBC_INLINE static void fini_trampoline(void *context) {
    auto *self = static_cast<CrystallineDomain *>(context);
    self->fini();
  }

  // Drain this thread's pending retire batch into the domain's slot
  // chains before the per-thread arena (which owns the batch storage)
  // is released.
  LIBC_INLINE static void thread_flush_trampoline(void *context,
                                                  CrystallineBatch *batch) {
    if (batch == nullptr || batch->first == nullptr)
      return;
    auto *self = static_cast<CrystallineDomain *>(context);
    // Close the batch: mark anchor, then try_retire publishes. Same
    // encoding as the in-line retire close path: bit 31 set + bottom
    // 31 bits encode the chain head (batch->first).
    as_node(batch->last)
        ->batch_link.store(kCrystallineBatchLinkRnodeBit |
                               BatchLinkCodec<NodeT>::encode(batch->first),
                           cpp::MemoryOrder::SEQ_CST);
    self->try_retire(*batch);
  }

  // Release this thread's per-domain slot back to the pool. Called from
  // scratch_thread_cleanup AFTER thread_flush_trampoline so any pending
  // retires are already published into the slot chains the released
  // slot is leaving behind.
  //
  // Drain THIS slot's first[] arrays before splicing — peer threads
  // may have published batch nodes to first[j].list[0] while the
  // exiting thread was still on the active chain (during the flush
  // window). Without draining, reset_slot_fields zeroes those entries
  // and the publishing peers' refs counters never decrement → batch
  // leak. The drain exchanges first[j].list[0] to invptr (which also
  // signals to subsequent peer try_retire calls to skip publishing
  // here via the `prev == invptr` rollback path) and traverses each
  // popped chain to decrement its refs.
  //
  // Order matters: drain BEFORE release_slot. release_slot calls
  // active_splice (removes from active chain), then freelist_push
  // (rewrites the link). After active_splice succeeds, no new peer
  // publications can target us, so the only nodes we need to drain
  // are the ones already in first[] at the moment of drain.
  LIBC_INLINE static void release_slot_trampoline(void *context,
                                                  uint16_t slot_idx) {
    auto *self = static_cast<CrystallineDomain *>(context);
    self->drain_slot_first(slot_idx);
    self->pool_.release_slot(slot_idx);
  }

  // Eager per-thread slot claim. Forces `pool_.claim_slot()` to commit
  // its slot-pool VA on a benign code path so any later fault-context
  // protect()/retire() doesn't pay the demand-commit cost. Idempotent
  // — if the calling thread has already claimed a slot in this domain,
  // `my_slot_idx()` short-circuits on the cached TLS index.
  LIBC_INLINE static void warm_thread_trampoline(void *context) {
    auto *self = static_cast<CrystallineDomain *>(context);
    self->warm_thread();
  }

  // Drain a per-thread slot's first[] arrays via exchange(invptr) +
  // traverse. Called by release_slot_trampoline before splicing the
  // slot off the active chain, to ensure peer threads' published
  // batch nodes get their refs decremented (otherwise the peers'
  // batches leak).
  //
  // Uses the calling thread's CrystallineBatch (this is the exiting
  // thread's batch) to accumulate freed anchors. After the drain,
  // free_list runs the FreeFn destructor on every batch that hit
  // refs==0.
  LIBC_INLINE void drain_slot_first(uint16_t slot_idx) {
    SlotT &slot = pool_.at(slot_idx);
    CrystallineBatch &batch = my_batch();
    // Exchange first[] entries to invptr; capture old values.
    // Uses MaxIdx + 2 to cover the helper slots
    // (hr_num and hr_num+1) populated by the slow_path helping
    // protocol — those carry the same chain semantics and need
    // draining too.
    CrystallineNode *first[MaxIdx + 2];
    for (uint32_t i = 0; i < MaxIdx + 2; i++) {
      first[i] = slot.first[i].list[0].exchange(
          crystalline_inv_ptr(), cpp::MemoryOrder::ACQ_REL);
    }
    for (uint32_t i = 0; i < MaxIdx + 2; i++) {
      if (first[i] != crystalline_inv_ptr() && first[i] != nullptr)
        traverse(&batch.list, first[i]);
    }
    free_list(batch.list);
    batch.list = nullptr;
    batch.list_count = 0;
  }

  LIBC_INLINE void fork_reinit() {
    // Post-fork the child is single-threaded; every thread other than
    // the forking thread is gone. Reset the pool back to its initial
    // freelist-of-all-slots state, then clear the surviving thread's
    // slot index — the slot the survivor used pre-fork is now back on
    // the freelist, so leaving the surviving thread's TLS reference at
    // the old index would let the next claimer hand it out a SECOND
    // time, producing two "owners" of the same slot. Lazy-claim will
    // hand the survivor a fresh slot on its next reservation.
    //
    // Any pre-fork retires the surviving thread had buffered on its
    // CrystallineBatch are discarded — fork is a POSIX quiesce point.
    era_.store(1, cpp::MemoryOrder::RELAXED);
    slow_counter_.store(0, cpp::MemoryOrder::RELAXED);
    pool_.fork_reinit();

    auto *ts = ::LIBC_NAMESPACE::internal::get_thread_scratch();
    if (ts == nullptr)
      return;
    ts->crystalline_slot_idx[domain_id_] = kCrystallineSlotNullIndex;
    // Match the slow_counter_ reset above. Without this the surviving
    // thread's recorded gen would still reflect the pre-fork counter
    // value and help_read's gate would short-circuit forever on
    // post-fork slow_paths until the counter wrapped past the stale
    // value.
    ts->crystalline_last_helped_slow_gen[domain_id_] = 0;
    CrystallineBatch &batch = ts->crystalline_batches[domain_id_];
    batch.first = nullptr;
    batch.last = nullptr;
    batch.list = nullptr;
    batch.counter = 0;
    batch.list_count = 0;
    // Pool's chain_version_ has been reset to 0 by pool_.fork_reinit().
    // Mirror that here so the next walker call observes a mismatch and
    // refreshes the snapshot against the post-fork active chain (empty,
    // until the surviving thread lazy-claims a fresh slot).
    batch.cached_chain_version = 0;
    batch.cached_count = 0;
    batch.cached_overflow = 0;
  }

  LIBC_INLINE void fini() {
    // Best-effort at process shutdown. We can't reach every live
    // thread's batch from here — retirees they hold will leak. The
    // process is exiting; acceptable.
    pool_.fini();
  }

  // Pool is mutable because slot_state_of (called from const-y walks
  // via try_retire / help_read) needs `at()` access. The pool itself
  // is internally synchronized.
  CrystallineSlotPool<MaxIdx> pool_;
};

} // namespace concurrent
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_DOMAIN_H
