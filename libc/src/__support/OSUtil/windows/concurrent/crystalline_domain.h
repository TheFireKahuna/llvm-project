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
// claimed yet". The first read()/reserve_slot() call on a new thread for
// this domain lazy-claims a slot (Treiber-pop, lock-free); thread exit
// releases it (Treiber-push). Per-thread retire batches (CrystallineBatch)
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
// The API mirrors the reference 1:1 (no Handle/Guard wrapper; callers
// manage reservation indices and pass the parent node themselves).
// Methods:
//   * init_node(NodeT*)                                — stamp birth_epoch,
//     called by the user after allocating a NodeT and before publishing
//     it to a lock-free data structure. Replaces the reference's alloc().
//   * read(atomic<NodeT*>&, index, NodeT* parent)       — hot-path read.
//     attempts=16 fast path, then slow_path helping.
//   * reserve_slot(NodeT* ptr, index, NodeT* parent)    — declare a
//     reservation without reading; for amortized traversals where the
//     same slot gets reused.
//   * retire(NodeT*)                                    — submit for
//     deferred reclamation.
//   * clear_all()                                       — drop every
//     reservation on the current thread (for API boundaries).
//   * current_epoch()                                   — introspection.
//
// NodeT contract
// --------------
// User types inherit from CrystallineNode. The base class provides the
// intrusive metadata fields (next / batch_link / refs / batch_next /
// birth_epoch). Zero-initialization is a valid "never retired" state
// (batch_link == nullptr is the retired-check discriminator). User
// allocation is outside the primitive — init_node stamps the runtime
// state but doesn't allocate. FreeFn (template parameter) is invoked
// once per node when the batch's refcount wraps to zero, and receives
// the full NodeT* for the user-side reclaim.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_DOMAIN_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_DOMAIN_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/type_traits.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain_registry.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_local_state.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_slot_pool.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace concurrent {

// -------------------------------------------------------------------------
// CrystallineNode — intrusive base for every retirable user type.
// -------------------------------------------------------------------------
//
// 1:1 port of WFRTracker.hpp's `struct WFRInfo`. The two type-punned
// unions encode the node's current role in the algorithm:
//
//   First union:
//     next        (inserted state)  — atomic chain link in a slot's list
//     slot        (prepare state)   — target CrystallineWordPair* for a
//                                      try_retire slot assignment
//     birth_epoch (anchor / refs node) — epoch stamped by init_node
//
//   Second union:
//     refs        (anchor)           — modular-addend refcount on the
//                                      batch anchor node
//     batch_next  (non-anchor)       — chain link for walking a batch's
//                                      nodes in retire order
//
// `batch_link` is an atomic pointer to the anchor node, OR a tagged
// self-reference indicating THIS node is its batch's anchor (low-bit
// tag via WFR_RNODE / WFR_IS_RNODE below). `batch_link == nullptr` is
// the retired-check discriminator: untouched nodes have nullptr, retired
// nodes have either the anchor or the self-tag.
//
// All fields are owned by the runtime; user code must never read or
// write them. Zero-initialization is the valid "freshly allocated, not
// yet retired" state — init_node() stamps birth_epoch and batch_link=0
// at publication time.
struct CrystallineNode {
  union {
    // Inserted state: link to the next node in a slot's retirement chain.
    cpp::Atomic<CrystallineNode *> next;
    // Preparation state: target slot's first-word-pair during try_retire's
    // slot-selection loop. A raw (non-atomic) pointer — the node isn't yet
    // visible to any reader during this phase, so the store is plain.
    CrystallineWordPair *slot;
    // Anchor-node (refs-node) role: birth epoch stamped at init_node time,
    // used by subsequent retire() calls to track the batch's min-epoch.
    uint64_t birth_epoch;
  };

  // Anchor pointer (or WFR_IS_RNODE-tagged self-reference when this node
  // IS the anchor). Zero in fresh nodes; set by retire() at batch close.
  cpp::Atomic<CrystallineNode *> batch_link;

  union {
    // Anchor role: batch-level reference count. Modular-addend
    // arithmetic — wraps to zero when the last holder releases.
    cpp::Atomic<uintptr_t> refs;
    // Non-anchor role: chain pointer across the batch's nodes in
    // retire order (from batches.first backwards).
    CrystallineNode *batch_next;
  };
};

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

// Tag bit 0 on `batch_link` distinguishes "this node is the batch's
// anchor" from "here is a plain pointer to the anchor". Reference
// WFR_RNODE / WFR_IS_RNODE.
LIBC_INLINE CrystallineNode *crystalline_rnode(CrystallineNode *n) {
  return reinterpret_cast<CrystallineNode *>(
      reinterpret_cast<uintptr_t>(n) ^ 1U);
}
LIBC_INLINE bool crystalline_is_rnode(CrystallineNode *n) {
  return (reinterpret_cast<uintptr_t>(n) & 1U) != 0;
}

// Cap on the free-list cache held per-thread during a traverse walk
// (reference's MAX_WFRC). Bounds memory the reaper carries between
// explicit free_list() flushes. Reference default = 12.
inline constexpr uint32_t kCrystallineFreeCacheCap = 12;

// -------------------------------------------------------------------------
// CrystallineDomain<NodeT, FreeFn, Freq>
// -------------------------------------------------------------------------
//
// NodeT — user retirable type, inherits CrystallineNode.
// FreeFn — `void (*)(NodeT*)` invoked once per node on final reclaim.
// Freq   — retire counter threshold for try_retire triggering. Matches
//          the reference's `emptyFreq` parameter. Default 128.
template <typename NodeT, auto FreeFn, uint32_t Freq = 128>
class CrystallineDomain {
  static_assert(cpp::is_base_of_v<CrystallineNode, NodeT>,
                "NodeT must derive from CrystallineNode");
  static_assert(Freq > 0, "Freq must be positive");

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
    descriptor_.name = "crystalline_domain";
    if (LIBC_UNLIKELY(!pool_.init()))
      __builtin_trap();
    registry_push(&descriptor_);
    domain_id_ = descriptor_.domain_id;
    // epoch starts at 1 (reference uses 0 as sentinel / "uninitialized")
    epoch_.store(1, cpp::MemoryOrder::RELEASE);
    slow_counter_.store(0, cpp::MemoryOrder::RELEASE);
  }

  CrystallineDomain(const CrystallineDomain &) = delete;
  CrystallineDomain &operator=(const CrystallineDomain &) = delete;

  // -----------------------------------------------------------------------
  // Public API — 1:1 with WFRTracker's surface (minus `tid` — implicit).
  // -----------------------------------------------------------------------

  // Stamp birth_epoch on a freshly allocated node. Replaces the
  // reference's alloc() — the user handles allocation, then calls this
  // to prepare the node for use in the lock-free data structure.
  // Amortized global-epoch bump every Freq-th call; precedes a
  // help_read() so stalled readers make progress.
  LIBC_INLINE void init_node(NodeT *node) {
    CrystallineBatch &batch = my_batch();
    batch.alloc_counter++;
    if (batch.alloc_counter % Freq == 0) {
      help_read();
      epoch_.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
    }
    auto *cn = static_cast<CrystallineNode *>(node);
    cn->birth_epoch = current_epoch();
    cn->batch_link.store(nullptr, cpp::MemoryOrder::RELAXED);
  }

  // Fast-path read. Attempts up to 16 epoch-matches on the caller's
  // existing reservation; falls through to slow_path helping if the
  // global epoch drifts. `parent` is the node whose atomic field is
  // being dereferenced (used in slow_path for parent-handoff accounting).
  LIBC_INLINE NodeT *read(cpp::Atomic<NodeT *> &obj, uint32_t index,
                          NodeT *parent) {
    CrystallineDomainSlot &my = my_slot_state();
    uint64_t prev_epoch =
        my.epoch[index].pair[0].load(cpp::MemoryOrder::ACQUIRE);
    uint32_t attempts = 16;
    do {
      NodeT *ptr = obj.load(cpp::MemoryOrder::ACQUIRE);
      uint64_t curr_epoch = current_epoch();
      if (curr_epoch == prev_epoch)
        return ptr;
      prev_epoch = do_update(curr_epoch, index);
    } while (--attempts != 0);

    return slow_path(&obj, index, parent);
  }

  // Declare a reservation without producing a pointer value. The fast
  // path matches read() up to the "return ptr" line; on falling through
  // to slow_path, passes a null `obj` so the protocol only advances the
  // epoch/seqno without loading.
  LIBC_INLINE void reserve_slot(NodeT * /*ptr_unused*/, uint32_t index,
                                NodeT *parent) {
    CrystallineDomainSlot &my = my_slot_state();
    uint64_t prev_epoch =
        my.epoch[index].pair[0].load(cpp::MemoryOrder::ACQUIRE);
    uint32_t attempts = 16;
    do {
      uint64_t curr_epoch = current_epoch();
      if (curr_epoch == prev_epoch)
        return;
      prev_epoch = do_update(curr_epoch, index);
    } while (--attempts != 0);

    (void)slow_path(nullptr, index, parent);
  }

  // Submit `node` for deferred reclamation. Buffers into the per-thread
  // batch; fires try_retire every Freq-th retire.
  LIBC_INLINE void retire(NodeT *node) {
    if (node == nullptr)
      return;
    auto *info = static_cast<CrystallineNode *>(node);
    CrystallineBatch &batch = my_batch();
    if (!batch.first) {
      batch.last = info;
      info->refs.store(kCrystallineProtect1, cpp::MemoryOrder::RELAXED);
    } else {
      // The anchor (batch.last) carries the minimum birth_epoch across
      // the batch — lets try_retire filter slots by epoch efficiently.
      if (batch.last->birth_epoch > info->birth_epoch)
        batch.last->birth_epoch = info->birth_epoch;
      info->batch_link.store(batch.last, cpp::MemoryOrder::SEQ_CST);
      info->batch_next = batch.first;
    }

    batch.first = info;
    batch.counter++;
    if (batch.counter % Freq == 0) {
      // Mark anchor as "I'm the refs-node, and here's my chain head".
      batch.last->batch_link.store(crystalline_rnode(info),
                                   cpp::MemoryOrder::SEQ_CST);
      try_retire(batch);
    }
  }

  // Drop every reservation on the calling thread. Used at API boundaries
  // where the caller has finished all dereferences and wants to let
  // retiring threads claim the slots.
  LIBC_INLINE void clear_all() {
    CrystallineDomainSlot &my = my_slot_state();
    CrystallineBatch &batch = my_batch();
    CrystallineNode *first[kCrystallineHrNum];
    for (uint32_t i = 0; i < kCrystallineHrNum; i++) {
      first[i] = my.first[i].list[0].exchange(
          crystalline_inv_ptr(), cpp::MemoryOrder::ACQ_REL);
    }
    for (uint32_t i = 0; i < kCrystallineHrNum; i++) {
      if (first[i] != crystalline_inv_ptr())
        traverse(&batch.list, first[i]);
    }
    free_list(batch.list);
    batch.list = nullptr;
    batch.list_count = 0;
  }

  // ---------- Introspection ----------

  LIBC_INLINE uint64_t current_epoch() {
    return epoch_.load(cpp::MemoryOrder::ACQUIRE);
  }

  LIBC_INLINE static constexpr uint32_t reservation_slots() {
    return kCrystallineHrNum;
  }

private:
  // -------- Instance state --------

  alignas(64) cpp::Atomic<uint64_t> epoch_{0};
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

  LIBC_INLINE CrystallineDomainSlot &my_slot_state() {
    return pool_.at(my_slot_idx());
  }

  LIBC_INLINE CrystallineBatch &my_batch() {
    return my_thread()->crystalline_batches[domain_id_];
  }

  // Index-keyed slot accessor for cross-thread walks.
  LIBC_INLINE CrystallineDomainSlot &slot_state_of(uint16_t idx) {
    return pool_.at(idx);
  }

  // -----------------------------------------------------------------------
  // Reclamation helpers — ported from the reference, no algorithmic
  // changes.
  // -----------------------------------------------------------------------

  // Resolve the anchor ("refs") node for a given batch member.
  LIBC_INLINE CrystallineNode *get_refs_node(CrystallineNode *node) {
    CrystallineNode *r =
        node->batch_link.load(cpp::MemoryOrder::ACQUIRE);
    if (crystalline_is_rnode(r))
      r = node;
    return r;
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
        // Terminal refs-node: we've walked the full chain, decrement
        // the anchor's share and stop.
        CrystallineNode *refs =
            reinterpret_cast<CrystallineNode *>(
                reinterpret_cast<uintptr_t>(curr) ^ 1U);
        if (refs->refs.fetch_sub(1, cpp::MemoryOrder::ACQ_REL) == 1) {
          refs->next.store(*list, cpp::MemoryOrder::RELAXED);
          *list = refs;
        }
        break;
      }
      next =
          curr->next.exchange(crystalline_inv_ptr(),
                              cpp::MemoryOrder::ACQ_REL);
      CrystallineNode *refs =
          curr->batch_link.load(cpp::MemoryOrder::RELAXED);
      if (refs->refs.fetch_sub(1, cpp::MemoryOrder::ACQ_REL) == 1) {
        refs->next.store(*list, cpp::MemoryOrder::RELAXED);
        *list = refs;
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
      // The reference stores `WFR_RNODE(node)` in the anchor's batch_link
      // at batch close (retire's try_retire path), so untagging gives
      // the chain head. A RELAXED load suffices — the anchor was
      // exclusively claimed by the refs->refs wrap-to-zero above.
      CrystallineNode *tagged =
          list->batch_link.load(cpp::MemoryOrder::RELAXED);
      CrystallineNode *start = reinterpret_cast<CrystallineNode *>(
          reinterpret_cast<uintptr_t>(tagged) ^ 1U);
      list = list->next.load(cpp::MemoryOrder::RELAXED);
      do {
        auto *obj = static_cast<NodeT *>(start);
        start = start->batch_next;
        FreeFn(obj);
      } while (start != nullptr);
    }
  }

  // -----------------------------------------------------------------------
  // do_update — epoch refresh and chain drain on the caller's slot.
  // -----------------------------------------------------------------------
  //
  // Called from the read() / reserve_slot() fast-path loop whenever the
  // observed epoch advances. Detaches any pending chain on first[index],
  // cache-traverses it (drops refs on freed batches), then publishes the
  // new current_epoch into epoch[index].pair[0].
  LIBC_INLINE uint64_t do_update(uint64_t curr_epoch, uint32_t index) {
    CrystallineDomainSlot &my = my_slot_state();
    CrystallineBatch &batch = my_batch();
    if (my.first[index].list[0].load(cpp::MemoryOrder::ACQUIRE) != nullptr) {
      CrystallineNode *first = my.first[index].list[0].exchange(
          crystalline_inv_ptr(), cpp::MemoryOrder::ACQ_REL);
      if (first != crystalline_inv_ptr())
        traverse_cache(batch, first);
      my.first[index].list[0].store(nullptr, cpp::MemoryOrder::SEQ_CST);
      curr_epoch = current_epoch();
    }
    my.epoch[index].pair[0].store(curr_epoch, cpp::MemoryOrder::SEQ_CST);
    return curr_epoch;
  }

  // -----------------------------------------------------------------------
  // slow_path — the wait-free fallback when the fast path's attempts
  // are exhausted. Publishes a help-request via `state[index].result =
  // {WFR_INVPTR64, seqno}`, waits for either a self-observed epoch
  // match or a helper-produced result, then reconciles the returned
  // (epoch, ptr) pair with any pending list and parent-handoff.
  // -----------------------------------------------------------------------
  LIBC_INLINE NodeT *slow_path(cpp::Atomic<NodeT *> *obj, uint32_t index,
                               NodeT *node) {
    CrystallineDomainSlot &my = my_slot_state();
    CrystallineBatch &batch = my_batch();

    // Compute the birth epoch for the parent-node reference we're
    // holding. If the parent has already been retired (its batch_link
    // points at a non-rnode anchor), we use the anchor's birth_epoch
    // (the min across the retired batch) instead of the parent's own
    // — protects us from ABA where the parent was reclaimed while we
    // were mid-traversal.
    uint64_t birth_epoch = 0;
    CrystallineNode *parent = nullptr;
    if (node != nullptr) {
      parent = static_cast<CrystallineNode *>(node);
      birth_epoch = parent->birth_epoch;
      CrystallineNode *info =
          parent->batch_link.load(cpp::MemoryOrder::ACQUIRE);
      if (info != nullptr && !crystalline_is_rnode(info))
        birth_epoch = info->birth_epoch;
    }

    uint64_t prev_epoch =
        my.epoch[index].pair[0].load(cpp::MemoryOrder::ACQUIRE);
    slow_counter_.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
    my.state[index].pointer.store(reinterpret_cast<uint64_t>(obj),
                                  cpp::MemoryOrder::RELEASE);
    my.state[index].parent.store(parent, cpp::MemoryOrder::RELEASE);
    my.state[index].epoch.store(birth_epoch, cpp::MemoryOrder::RELEASE);
    uint64_t seqno =
        my.epoch[index].pair[1].load(cpp::MemoryOrder::ACQUIRE);

    CrystallineValuePair last_result;
    last_result.pair[0] = kCrystallineInvPtr64;
    last_result.pair[1] = seqno;
    my.state[index].result.full.store(last_result.full,
                                      cpp::MemoryOrder::RELEASE);

    CrystallineValuePair old, value;
    uint64_t result_epoch, result_ptr, expseqno;
    CrystallineNode *first;
    do {
      NodeT *ptr = obj ? obj->load(cpp::MemoryOrder::ACQUIRE) : nullptr;
      uint64_t curr_epoch = current_epoch();
      if (curr_epoch == prev_epoch) {
        last_result.pair[0] = kCrystallineInvPtr64;
        last_result.pair[1] = seqno;
        value.pair[0] = 0;
        value.pair[1] = 0;
        if (my.state[index].result.full.compare_exchange_strong(
                last_result.full, value.full,
                cpp::MemoryOrder::ACQ_REL,
                cpp::MemoryOrder::ACQUIRE)) {
          my.epoch[index].pair[1].store(seqno + 2,
                                        cpp::MemoryOrder::RELEASE);
          my.first[index].pair[1].store(seqno + 2,
                                        cpp::MemoryOrder::RELEASE);
          slow_counter_.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
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
        curr_epoch = current_epoch();
      }
      first = nullptr;
      old.pair[0] = prev_epoch;
      old.pair[1] = seqno;
      value.pair[0] = curr_epoch;
      value.pair[1] = seqno;
      my.epoch[index].full.compare_exchange_strong(
          old.full, value.full, cpp::MemoryOrder::SEQ_CST,
          cpp::MemoryOrder::ACQUIRE);
      prev_epoch = curr_epoch;
      result_ptr =
          my.state[index].result.pair[0].load(cpp::MemoryOrder::ACQUIRE);
    } while (result_ptr == kCrystallineInvPtr64);

    // Empty-epoch seqno advance.
    expseqno = seqno;
    my.epoch[index].pair[1].compare_exchange_strong(
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

    // Publish the produced epoch into this slot's visible epoch.
    my.epoch[index].pair[1].store(seqno + 1, cpp::MemoryOrder::RELEASE);
    result_epoch =
        my.state[index].result.pair[1].load(cpp::MemoryOrder::ACQUIRE);
    my.epoch[index].pair[0].store(result_epoch,
                                  cpp::MemoryOrder::RELEASE);

    // Check whether the produced pointer was retired while we were
    // in slow_path. If so, attach a refs-node reference onto our
    // first[index] chain so we retain a hold on the batch.
    my.first[index].pair[1].store(seqno + 1, cpp::MemoryOrder::RELEASE);
    result_ptr =
        my.state[index].result.pair[0].load(cpp::MemoryOrder::ACQUIRE) &
        0xFFFFFFFFFFFFFFFCULL;
    auto *ptr_node = reinterpret_cast<CrystallineNode *>(result_ptr);
    if (result_ptr != 0 &&
        ptr_node->batch_link.load(cpp::MemoryOrder::ACQUIRE) != nullptr) {
      CrystallineNode *refs = get_refs_node(ptr_node);
      refs->refs.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
      if (first != crystalline_inv_ptr())
        traverse_cache(batch, first);
      first = my.first[index].list[0].exchange(
          crystalline_rnode(refs), cpp::MemoryOrder::ACQ_REL);
    }
    slow_counter_.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);

    if (first != crystalline_inv_ptr())
      traverse_cache(batch, first);

    // Parent-handoff: if the parent node was retired while we were
    // slow-pathing, increment its anchor refs by PROTECT2, then CAS
    // every live thread's state[hr_num].parent field to claim back
    // the references they would have held. Net delta is added once.
    if (parent != nullptr &&
        parent->batch_link.load(cpp::MemoryOrder::ACQUIRE) != nullptr) {
      CrystallineNode *refs = get_refs_node(parent);
      refs->refs.fetch_add(kCrystallineProtect2,
                           cpp::MemoryOrder::ACQ_REL);
      uintptr_t adjs = static_cast<uintptr_t>(-kCrystallineProtect2);
      for (uint16_t i = pool_.active_head(); i != 0;
           i = pool_.active_next(i)) {
        CrystallineDomainSlot &their = slot_state_of(i);
        CrystallineNode *exp = parent;
        if (their.state[kCrystallineHrNum].parent.compare_exchange_strong(
                exp, nullptr, cpp::MemoryOrder::ACQ_REL,
                cpp::MemoryOrder::RELAXED)) {
          adjs++;
        }
      }
      refs->refs.fetch_add(adjs, cpp::MemoryOrder::ACQ_REL);
    }

    return reinterpret_cast<NodeT *>(result_ptr);
  }

  // -----------------------------------------------------------------------
  // help_thread — wait-free helping protocol for a stalled slow_path
  // on some other thread `target`. Produces a (ptr, epoch) into the
  // target's result word if a consistent epoch observation is possible.
  // -----------------------------------------------------------------------
  LIBC_INLINE void help_thread(uint16_t target_idx, uint32_t index,
                               uint16_t my_idx) {
    CrystallineDomainSlot &their = slot_state_of(target_idx);
    CrystallineDomainSlot &my = slot_state_of(my_idx);
    CrystallineBatch &my_b = my_batch();

    CrystallineValuePair last_result;
    last_result.full = their.state[index].result.full.load(
        cpp::MemoryOrder::ACQUIRE);
    if (last_result.pair[0] != kCrystallineInvPtr64)
      return;
    uint64_t birth_epoch =
        their.state[index].epoch.load(cpp::MemoryOrder::ACQUIRE);
    CrystallineNode *parent =
        their.state[index].parent.load(cpp::MemoryOrder::ACQUIRE);
    if (parent != nullptr) {
      my.first[kCrystallineHrNum].list[0].store(
          nullptr, cpp::MemoryOrder::SEQ_CST);
      my.epoch[kCrystallineHrNum].pair[0].store(
          birth_epoch, cpp::MemoryOrder::SEQ_CST);
    }
    my.state[kCrystallineHrNum].parent.store(parent,
                                             cpp::MemoryOrder::SEQ_CST);
    auto *obj = reinterpret_cast<cpp::Atomic<NodeT *> *>(
        their.state[index].pointer.load(cpp::MemoryOrder::ACQUIRE));
    uint64_t seqno =
        their.epoch[index].pair[1].load(cpp::MemoryOrder::ACQUIRE);
    if (last_result.pair[1] == seqno) {
      uint64_t prev_epoch = current_epoch();
      do {
        // Use our OWN slot kCrystallineHrNum+1 as the helper's
        // dereference workspace. do_update on our slot refreshes the
        // epoch publication without perturbing the target's state.
        prev_epoch = do_update_on(my, my_b, prev_epoch,
                                  kCrystallineHrNum + 1);
        NodeT *ptr =
            obj ? obj->load(cpp::MemoryOrder::ACQUIRE) : nullptr;
        uint64_t curr_epoch = current_epoch();
        if (curr_epoch == prev_epoch) {
          CrystallineValuePair value;
          value.pair[0] = reinterpret_cast<uint64_t>(ptr);
          value.pair[1] = curr_epoch;
          if (their.state[index].result.full.compare_exchange_strong(
                  last_result.full, value.full,
                  cpp::MemoryOrder::ACQ_REL,
                  cpp::MemoryOrder::ACQUIRE)) {
            // Empty-epoch transition on the target's seqno (best-
            // effort; another helper may have done it already).
            uint64_t expseqno = seqno;
            their.epoch[index].pair[1].compare_exchange_strong(
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
            // Set the real epoch on the target's epoch slot.
            value.pair[0] = curr_epoch;
            value.pair[1] = seqno + 1;
            old_val.pair[1] = their.epoch[index].pair[1].load(
                cpp::MemoryOrder::ACQUIRE);
            old_val.pair[0] = their.epoch[index].pair[0].load(
                cpp::MemoryOrder::ACQUIRE);
            while (old_val.pair[1] == seqno) { // 2 iterations at most
              if (their.epoch[index].full.compare_exchange_weak(
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
            auto *ptr_node = reinterpret_cast<CrystallineNode *>(ptr_val);
            if (ptr_val != 0 &&
                ptr_node->batch_link.load(
                    cpp::MemoryOrder::ACQUIRE) != nullptr) {
              CrystallineNode *refs = get_refs_node(ptr_node);
              refs->refs.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
              value.list[0] = crystalline_rnode(refs);
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
        prev_epoch = curr_epoch;
      } while (last_result.full ==
               their.state[index].result.full.load(
                   cpp::MemoryOrder::ACQUIRE));
    done:
      if (my.epoch[kCrystallineHrNum + 1].pair[0].exchange(
              0, cpp::MemoryOrder::SEQ_CST) != 0) {
        CrystallineNode *first =
            my.first[kCrystallineHrNum + 1].list[0].exchange(
                crystalline_inv_ptr(), cpp::MemoryOrder::ACQ_REL);
        traverse_cache(my_b, first);
      }
    }
    // If the helpee handed us a parent reservation reference we no
    // longer own, release it.
    if (my.state[kCrystallineHrNum].parent.exchange(
            nullptr, cpp::MemoryOrder::SEQ_CST) != parent) {
      CrystallineNode *refs = get_refs_node(parent);
      if (refs->refs.fetch_sub(1, cpp::MemoryOrder::ACQ_REL) == 1) {
        refs->next.store(my_b.list, cpp::MemoryOrder::RELAXED);
        my_b.list = refs;
      }
    }
    if (my.epoch[kCrystallineHrNum].pair[0].exchange(
            0, cpp::MemoryOrder::SEQ_CST) != 0) {
      CrystallineNode *first =
          my.first[kCrystallineHrNum].list[0].exchange(
              crystalline_inv_ptr(), cpp::MemoryOrder::ACQ_REL);
      traverse_cache(my_b, first);
    }
    free_list(my_b.list);
    my_b.list = nullptr;
    my_b.list_count = 0;
  }

  // help_read — scan every live slot in the pool for a stalled
  // slow_path (state[j].result.pair[0] == WFR_INVPTR64) and help it.
  LIBC_INLINE void help_read() {
    if (slow_counter_.load(cpp::MemoryOrder::ACQUIRE) == 0)
      return;
    uint16_t my_idx = my_slot_idx();
    for (uint16_t target_idx = pool_.active_head(); target_idx != 0;
         target_idx = pool_.active_next(target_idx)) {
      CrystallineDomainSlot &their = slot_state_of(target_idx);
      for (uint32_t j = 0; j < kCrystallineHrNum; j++) {
        uint64_t result_ptr = their.state[j].result.pair[0].load(
            cpp::MemoryOrder::ACQUIRE);
        if (result_ptr == kCrystallineInvPtr64) {
          help_thread(target_idx, j, my_idx);
        }
      }
    }
  }

  // do_update variant for help_thread that operates on an explicit
  // slot/batch rather than the calling thread's own. Body identical
  // to do_update modulo the arguments.
  LIBC_INLINE uint64_t do_update_on(CrystallineDomainSlot &my,
                                    CrystallineBatch &batch,
                                    uint64_t curr_epoch, uint32_t index) {
    if (my.first[index].list[0].load(cpp::MemoryOrder::ACQUIRE) != nullptr) {
      CrystallineNode *first = my.first[index].list[0].exchange(
          crystalline_inv_ptr(), cpp::MemoryOrder::ACQ_REL);
      if (first != crystalline_inv_ptr())
        traverse_cache(batch, first);
      my.first[index].list[0].store(nullptr, cpp::MemoryOrder::SEQ_CST);
      curr_epoch = current_epoch();
    }
    my.epoch[index].pair[0].store(curr_epoch, cpp::MemoryOrder::SEQ_CST);
    return curr_epoch;
  }

  // -----------------------------------------------------------------------
  // try_retire — publish the current batch across live-thread slots
  // using the modular-addend refs adjustment. Directly ports lines
  // 535-611 of the reference.
  // -----------------------------------------------------------------------
  LIBC_INLINE void try_retire(CrystallineBatch &batch) {
    CrystallineNode *curr = batch.first;
    CrystallineNode *refs = batch.last;
    uint64_t min_epoch = refs->birth_epoch;

    // Phase A — walk every claimed pool slot and claim a batch node
    // for each eligible slot. Claim = write the slot's first word-pair
    // address into the node's `slot` field (first union's pointer alias
    // of `next`). Stops early if we run out of batch nodes.
    CrystallineNode *last = curr;
    for (uint16_t i = pool_.active_head(); i != 0;
         i = pool_.active_next(i)) {
      CrystallineDomainSlot &their = slot_state_of(i);
      uint32_t j = 0;
      for (; j < kCrystallineHrNum; j++) {
        CrystallineNode *first = their.first[j].list[0].load(
            cpp::MemoryOrder::ACQUIRE);
        if (first == crystalline_inv_ptr())
          continue;
        if (their.first[j].pair[1].load(cpp::MemoryOrder::ACQUIRE) & 0x1U)
          continue; // in slow-path final transition
        uint64_t eepoch =
            their.epoch[j].pair[0].load(cpp::MemoryOrder::ACQUIRE);
        if (eepoch < min_epoch)
          continue;
        if (their.epoch[j].pair[1].load(cpp::MemoryOrder::ACQUIRE) & 0x1U)
          continue;
        if (last == refs) {
          return;
        }
        last->slot = &their.first[j];
        last = last->batch_next;
      }
      // Helper slots hr_num and hr_num+1 don't carry the seqno filter —
      // they're one-shot scratch used by the helping protocol.
      for (; j < kCrystallineHrNum + 2; j++) {
        CrystallineNode *first = their.first[j].list[0].load(
            cpp::MemoryOrder::ACQUIRE);
        if (first == crystalline_inv_ptr())
          continue;
        uint64_t eepoch =
            their.epoch[j].pair[0].load(cpp::MemoryOrder::ACQUIRE);
        if (eepoch < min_epoch)
          continue;
        if (last == refs) {
          return;
        }
        last->slot = &their.first[j];
        last = last->batch_next;
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
      CrystallineWordPair *slot_epoch = slot_first + kCrystallineSlotCount;
      curr->next.store(nullptr, cpp::MemoryOrder::RELAXED);
      if (slot_first->list[0].load(cpp::MemoryOrder::ACQUIRE) ==
          crystalline_inv_ptr())
        continue;
      uint64_t eepoch =
          slot_epoch->pair[0].load(cpp::MemoryOrder::ACQUIRE);
      if (eepoch < min_epoch)
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
          CrystallineNode *exp = nullptr;
          if (!curr->next.compare_exchange_strong(
                  exp, prev, cpp::MemoryOrder::ACQ_REL,
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
      refs->next.store(nullptr, cpp::MemoryOrder::RELAXED);
      free_list(refs);
    }
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
    // Close the batch: mark anchor, then try_retire publishes.
    batch->last->batch_link.store(crystalline_rnode(batch->first),
                                  cpp::MemoryOrder::SEQ_CST);
    self->try_retire(*batch);
  }

  // Release this thread's per-domain slot back to the pool. Called from
  // scratch_thread_cleanup AFTER thread_flush_trampoline so any pending
  // retires are already published into the slot chains the released
  // slot is leaving behind.
  LIBC_INLINE static void release_slot_trampoline(void *context,
                                                  uint16_t slot_idx) {
    auto *self = static_cast<CrystallineDomain *>(context);
    self->pool_.release_slot(slot_idx);
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
    epoch_.store(1, cpp::MemoryOrder::RELAXED);
    slow_counter_.store(0, cpp::MemoryOrder::RELAXED);
    pool_.fork_reinit();

    auto *ts = ::LIBC_NAMESPACE::internal::get_thread_scratch();
    if (ts == nullptr)
      return;
    ts->crystalline_slot_idx[domain_id_] = kCrystallineSlotNullIndex;
    CrystallineBatch &batch = ts->crystalline_batches[domain_id_];
    batch.first = nullptr;
    batch.last = nullptr;
    batch.list = nullptr;
    batch.counter = 0;
    batch.list_count = 0;
    batch.alloc_counter = 0;
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
  CrystallineSlotPool pool_;
};

} // namespace concurrent
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_DOMAIN_H
