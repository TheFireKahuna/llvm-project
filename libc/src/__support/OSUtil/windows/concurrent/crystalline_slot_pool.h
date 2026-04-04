//===-- Crystalline slot pool — demand-committed reservation table -*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-CrystallineDomain pool of `CrystallineDomainSlot` records. Replaces
// the inline-in-ThreadScratchState `CrystallineThreadRegion::slots[]`
// model so the per-thread arena can be `page_free`'d on thread exit
// without faulting cross-thread Crystalline walks.
//
// Algorithmic correspondence
// --------------------------
// The reference paper (`crystalline_reference.hpp.inc:5`,
// `WFRTracker(int task_num, ...)`) allocates `WFRSlot* slots` as a
// `memalign(sizeof(WFRSlot) * task_num)` block at construction and
// never frees it; threads index by integer `tid` ∈ [0, task_num). This
// pool is the elastic-sized analogue: VA reserved up front for `1<<16`
// slot records (matches the substrate's 16-bit `next` field), pages
// demand-committed via `internal::CommitRegion`. The reference's
// `task_num` invariant becomes "every CLAIMED slot is on the active
// chain", observed by walkers via chain traversal — NOT via a watermark.
//
// Why chain traversal, not watermark iteration
// --------------------------------------------
// `lock_free_linkage.h` is built around a single 64-bit `linkage::Link`
// word that atomically encodes state + chain-linkage + ABA tag + reserved
// bits. Used correctly, every visibility transition is one Link CAS.
// The substrate's Safety Triad (T1 tag monotonicity, T2 IDLE-on-reachable
// retry, T3 never-freed pool memory) makes hangs structurally impossible
// when consumers traverse via `link.next` chains.
//
// Earlier drafts of this pool used a `committed_slots_` watermark and
// linear `[1, hwm)` walks, copying wait_slot.cpp's pool shape. wait_slot
// gets away with that because no wait_slot consumer iterates the
// watermark — its walks chase per-Futex SLLs and the parking-lot SLL
// via `link.next`. Crystalline walks DO want a flat enumeration, but
// the substrate-correct expression of that is "chain of CLAIMED slots".
// Active-chain traversal:
//
//   * Walk size scales with CURRENT active count, not historical peak.
//   * Every state transition is a single Link CAS (T1 covers ABA).
//   * No coordinating multi-atomic protocol — `committed_slots_` is
//     purely internal to commit_more_slots, never read by walkers.
//   * No "zero-fill in committed-but-uninit'd ranges" workaround —
//     slots become walker-visible only by being pushed onto the active
//     chain, which only happens after they're owner-initialised.
//   * Hangs are structurally impossible (HELP-NOT-WAIT on observed MARK).
//
// Slot lifecycle
// --------------
// claim_slot():    Treiber-pop from freelist, reset slot fields to
//                  constructor-init (matches reference's ctor body
//                  lines 134-144), then Treiber-push onto active chain.
//                  Both pushes/pops are single Link writes plus one CAS
//                  on the chain head.
// release_slot():  Harris splice off active chain (mark-then-help via
//                  link_cas_set_mark + link_finalize_after_splice),
//                  then Treiber-push onto freelist via `link_store`.
//                  Encountering another walker's MARK on the chain
//                  triggers HELP-NOT-WAIT (substrate convention) so a
//                  killed splicer cannot freeze progress.
// walker:          for (idx = active_head(); idx; idx = active_next(idx))
//                  ... — single `link.load(ACQUIRE)` per slot. State !=
//                  CLAIMED or MARK observed → walk terminates. Crystalline-W
//                  tolerates partial walks (try_retire's "try" semantics;
//                  help_read re-runs on next init_node), so terminating
//                  on stale snap is correctness-preserving.
//
// State machine on `slot.link`:
//   FREE     — on freelist, link.next is freelist linkage.
//   CLAIMED  — on active chain, link.next is active chain linkage.
// Transitions are owner-exclusive at write time (claim_slot owns the
// slot between freelist pop and active push; release_slot owns it
// between active splice and freelist push), so the substrate's
// "drop MARK by invariant" withers compose correctly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_SLOT_POOL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_SLOT_POOL_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_local_state.h"
#include "src/__support/OSUtil/windows/concurrent/lock_free_linkage.h"
#include "src/__support/OSUtil/windows/memory/commit_region.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace concurrent {

// Two-state lifecycle byte folded into linkage::Link's `state` field.
enum class SlotPoolState : uint8_t {
  FREE = 0,    // on freelist; link.next is freelist linkage
  CLAIMED = 1, // on active chain; link.next is active chain linkage
};

// Slot 0 is the null sentinel — both freelist and active-chain tail.
inline constexpr uint16_t kCrystallineSlotNullIndex = 0;

// Maximum slots per pool. Bounded structurally by linkage::Link's
// 16-bit `next` field. ~96 MB VA reservation per pool at sizeof=1472,
// demand-committed; an unused pool only touches its first-page commit.
inline constexpr uint32_t kCrystallineSlotPoolCapacity = 1u << 16;

// Slots claimed-and-linked per commit_more_slots call. 8 slots = ~12 KB
// = ~3 OS pages — keeps each commit a multi-page chunk so page-rounding
// in CommitRegion::ensure_committed doesn't oscillate.
inline constexpr uint32_t kCrystallineSlotsPerCommitStep = 8;

// Total VA bytes reserved per pool.
inline constexpr size_t kCrystallineSlotPoolReserveBytes =
    static_cast<size_t>(kCrystallineSlotPoolCapacity) *
    sizeof(CrystallineDomainSlot);

// Cache-line isolated chain head: `[gen:16 | head:16]` packing defends
// the Treiber stack against ABA on the head field even before the per-
// slot tag bump kicks in. Used for both freelist_ and active_head_.
struct alignas(64) CrystallineFreelistLine {
  cpp::Atomic<uint32_t> head{0};
};
static_assert(sizeof(CrystallineFreelistLine) == 64,
              "CrystallineFreelistLine must occupy exactly one 64-byte "
              "cache line");
static_assert(alignof(CrystallineFreelistLine) == 64,
              "CrystallineFreelistLine must be cache-line aligned");

// CrystallineSlotPool — one instance per CrystallineDomain<>.
//
// Trivially constructible (constinit-safe). NOT usable until `init()`
// has run once, single-threaded, before any thread calls claim/release/
// walker accessors.
class CrystallineSlotPool {
public:
  LIBC_INLINE constexpr CrystallineSlotPool() = default;

  // One-shot bring-up. Reserves the full slot VA, commits the first
  // chunk of slots, links them into the freelist, sentinels slot 0.
  // Returns false on VA reservation or first-commit failure.
  [[nodiscard]] LIBC_INLINE bool init() {
    if (!slot_region_.init(kCrystallineSlotPoolReserveBytes,
                           kCrystallineSlotsPerCommitStep *
                               sizeof(CrystallineDomainSlot)))
      return false;
    slots_ = slot_region_.as<CrystallineDomainSlot>();
    init_slot_storage_at_origin(kCrystallineSlotsPerCommitStep);
    committed_slots_.store(kCrystallineSlotsPerCommitStep,
                           cpp::MemoryOrder::RELEASE);
    // Splice [1, kCrystallineSlotsPerCommitStep) onto the freelist.
    // Slot 0 is the sentinel (never claimed; freelist tail).
    for (uint32_t i = 1; i < kCrystallineSlotsPerCommitStep - 1; ++i)
      slots_[i].link.store(linkage::Link::pack_certified(
                               static_cast<uint8_t>(SlotPoolState::FREE),
                               /*tag=*/0,
                               static_cast<uint16_t>(i + 1)),
                           cpp::MemoryOrder::RELAXED);
    slots_[kCrystallineSlotsPerCommitStep - 1].link.store(
        linkage::Link::pack_certified(
            static_cast<uint8_t>(SlotPoolState::FREE),
            /*tag=*/0, kCrystallineSlotNullIndex),
        cpp::MemoryOrder::RELAXED);
    freelist_.head.store(fl_pack(/*gen=*/0, /*head=*/1),
                         cpp::MemoryOrder::RELAXED);
    active_head_.head.store(
        fl_pack(/*gen=*/0, kCrystallineSlotNullIndex),
        cpp::MemoryOrder::RELAXED);
    return true;
  }

  // Shutdown. Single-threaded — caller guarantees no concurrent
  // claim/release/walks.
  LIBC_INLINE void fini() {
    slot_region_.destroy();
    slots_ = nullptr;
    committed_slots_.store(0, cpp::MemoryOrder::RELAXED);
    freelist_.head.store(0, cpp::MemoryOrder::RELAXED);
    active_head_.head.store(0, cpp::MemoryOrder::RELAXED);
  }

  // Post-fork relink. Single-threaded post-fork. Every committed slot
  // goes back onto the freelist; the active chain is empty (post-fork
  // surviving thread's slot index is cleared by
  // CrystallineDomain::fork_reinit, so it lazy-claims fresh).
  LIBC_INLINE void fork_reinit() {
    if (slots_ == nullptr)
      return;
    uint32_t hwm = committed_slots_.load(cpp::MemoryOrder::RELAXED);
    init_slot_storage_at_origin(hwm);
    active_head_.head.store(
        fl_pack(/*gen=*/0, kCrystallineSlotNullIndex),
        cpp::MemoryOrder::RELAXED);
    if (hwm <= 1) {
      freelist_.head.store(fl_pack(/*gen=*/0, kCrystallineSlotNullIndex),
                           cpp::MemoryOrder::RELAXED);
      return;
    }
    for (uint32_t i = 1; i < hwm - 1; ++i)
      slots_[i].link.store(linkage::Link::pack_certified(
                               static_cast<uint8_t>(SlotPoolState::FREE),
                               /*tag=*/0,
                               static_cast<uint16_t>(i + 1)),
                           cpp::MemoryOrder::RELAXED);
    slots_[hwm - 1].link.store(linkage::Link::pack_certified(
                                   static_cast<uint8_t>(SlotPoolState::FREE),
                                   /*tag=*/0, kCrystallineSlotNullIndex),
                               cpp::MemoryOrder::RELAXED);
    freelist_.head.store(fl_pack(/*gen=*/0, /*head=*/1),
                         cpp::MemoryOrder::RELAXED);
  }

  // Claim a slot: pop from freelist, then push onto active chain.
  // Returns kCrystallineSlotNullIndex only on pool capacity exhaustion;
  // caller traps.
  LIBC_INLINE uint16_t claim_slot() {
    uint16_t idx = freelist_pop();
    if (LIBC_UNLIKELY(idx == kCrystallineSlotNullIndex))
      return kCrystallineSlotNullIndex;
    // Slot is owner-exclusive between pop and active-push: link.state
    // is FREE+CERT (from freelist_push's link_store), link.next is the
    // freelist's old next-pointer (loaded but not modified during pop).
    // Reset algorithmic fields to constructor-init tombstone (matches
    // reference's ctor body — first[].list[0] = inv_ptr, all else 0).
    reset_slot_fields(slots_[idx]);
    active_push(idx);
    return idx;
  }

  // Release a slot: tombstone its fields, splice off active chain via
  // mark-then-help, then push onto freelist.
  LIBC_INLINE void release_slot(uint16_t idx) {
    // Tombstone fields BEFORE the splice. While the slot is still on
    // the active chain, peer walkers see inv_ptr in first[] and zero
    // in epoch/state — the existing algorithmic filters skip it the
    // same way they skip a never-claimed slot.
    reset_slot_fields(slots_[idx]);
    // Splice. Returns false on chain-shape changes (concurrent
    // walker mark-then-help, prev's state transition, etc.) — retry.
    while (!active_splice(idx))
      ;
    freelist_push(idx);
  }

  // Walker entry. Returns first slot on the active chain or
  // kCrystallineSlotNullIndex if empty.
  LIBC_INLINE uint16_t active_head() {
    return static_cast<uint16_t>(
        fl_head(active_head_.head.load(cpp::MemoryOrder::ACQUIRE)));
  }

  // Walker step. Returns the next slot in the active chain after
  // `idx`, or kCrystallineSlotNullIndex if `idx` reached the end OR
  // its snap is stale (state != CLAIMED, MARK observed). Crystalline-W
  // tolerates partial walks — see try_retire's "try" semantics in the
  // reference paper.
  LIBC_INLINE uint16_t active_next(uint16_t idx) {
    linkage::Link link = slots_[idx].link.load(cpp::MemoryOrder::ACQUIRE);
    if (link.state() != static_cast<uint8_t>(SlotPoolState::CLAIMED) ||
        link.is_marked())
      return kCrystallineSlotNullIndex;
    return link.next();
  }

  LIBC_INLINE CrystallineDomainSlot &at(uint16_t idx) { return slots_[idx]; }

private:
  // ----- [gen:16|head:16] packers, copied from wait_slot.cpp:74-78 -----
  static constexpr uint32_t kFlGenShift = 16;
  static constexpr uint32_t kFlIndexMask = (1u << kFlGenShift) - 1;
  LIBC_INLINE static uint32_t fl_head(uint32_t packed) {
    return packed & kFlIndexMask;
  }
  LIBC_INLINE static uint32_t fl_gen(uint32_t packed) {
    return packed >> kFlGenShift;
  }
  LIBC_INLINE static uint32_t fl_pack(uint32_t gen, uint32_t head) {
    return ((gen & 0xFFFFu) << kFlGenShift) | (head & kFlIndexMask);
  }

  // Reset every algorithmically-meaningful slot field to its
  // constructor-init value. Mirrors WFRTracker's ctor body
  // (crystalline_reference.hpp.inc:134-144). The slot's `link` is NOT
  // touched here — caller handles link transitions for the freelist /
  // active chain.
  LIBC_INLINE static void reset_slot_fields(CrystallineDomainSlot &slot) {
    for (uint32_t j = 0; j < kCrystallineSlotCount; ++j) {
      slot.first[j].list[0].store(crystalline_inv_ptr(),
                                  cpp::MemoryOrder::RELAXED);
      slot.first[j].pair[1].store(0, cpp::MemoryOrder::RELAXED);
      slot.epoch[j].pair[0].store(0, cpp::MemoryOrder::RELAXED);
      slot.epoch[j].pair[1].store(0, cpp::MemoryOrder::RELAXED);
      slot.state[j].result.pair[0].store(0, cpp::MemoryOrder::RELAXED);
      slot.state[j].result.pair[1].store(0, cpp::MemoryOrder::RELAXED);
      slot.state[j].pointer.store(0, cpp::MemoryOrder::RELAXED);
      slot.state[j].parent.store(nullptr, cpp::MemoryOrder::RELAXED);
      slot.state[j].epoch.store(0, cpp::MemoryOrder::RELAXED);
    }
  }

  LIBC_INLINE void init_slot_storage_at_origin(uint32_t hwm) {
    for (uint32_t i = 0; i < hwm; ++i)
      reset_slot_fields(slots_[i]);
  }

  // Treiber pop from freelist. Returns 0 only on pool exhaustion.
  LIBC_INLINE uint16_t freelist_pop() {
    for (;;) {
      uint32_t old = freelist_.head.load(cpp::MemoryOrder::ACQUIRE);
      uint16_t head = static_cast<uint16_t>(fl_head(old));
      if (head == kCrystallineSlotNullIndex) {
        if (!commit_more_slots())
          return kCrystallineSlotNullIndex;
        continue;
      }
      uint16_t next = slots_[head].link.load(cpp::MemoryOrder::ACQUIRE).next();
      uint32_t desired = fl_pack(fl_gen(old) + 1, next);
      if (freelist_.head.compare_exchange_weak(old, desired,
                                               cpp::MemoryOrder::ACQ_REL,
                                               cpp::MemoryOrder::ACQUIRE))
        return head;
    }
  }

  // Treiber push onto freelist. Caller guarantees the slot is off any
  // chain (active chain splice succeeded). `link_store` writes
  // state=FREE, next=fl_head, sets CERT, drops MARK + ALERT_FIRED, bumps
  // tag — all via the substrate's `rewrite_certified` wither.
  LIBC_INLINE void freelist_push(uint16_t idx) {
    for (;;) {
      uint32_t old = freelist_.head.load(cpp::MemoryOrder::ACQUIRE);
      linkage::link_store(slots_[idx].link,
                          static_cast<uint8_t>(SlotPoolState::FREE),
                          static_cast<uint16_t>(fl_head(old)));
      uint32_t desired = fl_pack(fl_gen(old) + 1, idx);
      if (freelist_.head.compare_exchange_weak(old, desired,
                                               cpp::MemoryOrder::RELEASE,
                                               cpp::MemoryOrder::RELAXED))
        return;
    }
  }

  // Push onto active chain. Caller guarantees the slot is off the
  // freelist (freelist_pop succeeded), so we are owner-exclusive on
  // slot.link until the chain-head CAS succeeds.
  //
  // Two link writes (state, next) since the substrate has no compound
  // wither for "set state + set next + drop CERT" in a single bump.
  // Owner-exclusive — non-atomic stores are sound; tag bumps twice,
  // 32-bit per-link wraparound is astronomical (T1 retains its ABA
  // defense headroom).
  LIBC_INLINE void active_push(uint16_t idx) {
    // 1. Transition state FREE → CLAIMED. Preserve next + CERT (still
    //    on freelist's logical chain at this point — CERT is the
    //    "off any live walker chain" signal, which we maintain until
    //    we publish on the active chain via the head CAS).
    linkage::Link cur = slots_[idx].link.load(cpp::MemoryOrder::RELAXED);
    cur = linkage::link_store_state_known(
        slots_[idx].link, cur,
        static_cast<uint8_t>(SlotPoolState::CLAIMED));
    // 2. CAS-loop on active_head_.head: each iteration updates our
    //    link.next to the current head value (drops CERT + MARK +
    //    ALERT_FIRED — we are about to commit on-chain), then CAS
    //    publishes us as the new head.
    for (;;) {
      uint32_t old = active_head_.head.load(cpp::MemoryOrder::ACQUIRE);
      cur = linkage::link_store_next_uncertify_known(
          slots_[idx].link, cur,
          static_cast<uint16_t>(fl_head(old)));
      uint32_t desired = fl_pack(fl_gen(old) + 1, idx);
      if (active_head_.head.compare_exchange_weak(old, desired,
                                                  cpp::MemoryOrder::RELEASE,
                                                  cpp::MemoryOrder::RELAXED))
        return;
    }
  }

  // Harris splice off active chain. Returns true on success (target
  // detached, CERT published) or "target was not on chain" (vacuously
  // succeeded). Returns false on chain-shape race (caller retries).
  //
  // Mark-then-help walker discipline copied from
  // futex_utils.h:harris_walk_attempt — encountering another walker's
  // MARK on the chain triggers HELP-NOT-WAIT (we complete the splice
  // ourselves) so a killed splicer cannot freeze progress for any
  // other thread.
  LIBC_INLINE bool active_splice(uint16_t target) {
    // Iterate from head, tracking prev + prev_link snap for the
    // mid-splice CAS expected-value.
    uint16_t prev = kCrystallineSlotNullIndex;
    linkage::Link prev_link;
    uint16_t curr = static_cast<uint16_t>(
        fl_head(active_head_.head.load(cpp::MemoryOrder::ACQUIRE)));

    while (curr != kCrystallineSlotNullIndex) {
      auto &cs = slots_[curr];
      linkage::Link c_link = cs.link.load(cpp::MemoryOrder::ACQUIRE);

      // HELP-NOT-WAIT on observed MARK: another splicer is mid-flight
      // on `curr`. Complete its splice ourselves, then restart.
      if (c_link.is_marked()) {
        uint16_t c_next_marked = c_link.next();
        bool spliced = (prev == kCrystallineSlotNullIndex)
                           ? try_splice_active_head(curr, c_next_marked)
                           : try_splice_at_pred(prev, prev_link,
                                                  c_next_marked);
        if (spliced)
          linkage::link_finalize_after_splice(
              cs.link,
              linkage::MarkedLinkSnap::from_observed_marked(c_link));
        else
          linkage::link_clear_mark(cs.link);
        return false; // Restart from head.
      }

      // Stale snap — slot was concurrently released by another path
      // (shouldn't happen for our own target since we own it, but
      // could happen for an intermediate). Restart.
      uint8_t c_state = c_link.state();
      if (c_state != static_cast<uint8_t>(SlotPoolState::CLAIMED))
        return false;

      if (curr == target) {
        // Found target. Mark target.link to freeze target.next while
        // we CAS the parent — closes the stale c_next race where a
        // concurrent op-splice of target's successor could advance
        // target.next between our capture and the parent CAS.
        if (!linkage::link_cas_set_mark(cs.link, c_link))
          return false; // Concurrent mutation; retry.

        linkage::MarkedLinkSnap target_marked =
            linkage::link_pack_after_mark(c_link);
        uint16_t my_next = c_link.next();

        bool spliced = (prev == kCrystallineSlotNullIndex)
                           ? try_splice_active_head(target, my_next)
                           : try_splice_at_pred(prev, prev_link, my_next);
        if (spliced) {
          // Atomic CERT publish + MARK clear on target.link.
          linkage::link_finalize_after_splice(cs.link, target_marked);
          return true;
        }
        // Splice failed: parent's link or chain head changed
        // concurrently. Release the mark; retry.
        linkage::link_clear_mark(cs.link);
        return false;
      }

      // Advance.
      prev = curr;
      prev_link = c_link;
      curr = c_link.next();
    }

    // Walked to end without finding target. Means another actor
    // already removed it (shouldn't happen for our own target unless
    // a concurrent helper-then-splice path is active; we accept it as
    // already-spliced and proceed to freelist push).
    return true;
  }

  // Head splice via active_head_ CAS. Returns false if the chain head
  // is no longer `expected_head` (caller restarts). Loops on weak-CAS
  // spurious failures while the head still matches.
  LIBC_INLINE bool try_splice_active_head(uint16_t expected_head,
                                            uint16_t new_top) {
    for (;;) {
      uint32_t old = active_head_.head.load(cpp::MemoryOrder::ACQUIRE);
      if (static_cast<uint16_t>(fl_head(old)) != expected_head)
        return false;
      uint32_t desired = fl_pack(fl_gen(old) + 1, new_top);
      if (active_head_.head.compare_exchange_weak(old, desired,
                                                  cpp::MemoryOrder::ACQ_REL,
                                                  cpp::MemoryOrder::RELAXED))
        return true;
    }
  }

  // Mid splice via pred.link strong CAS: rewrite pred.next, drop
  // CERT/MARK/ALERT, preserve state. Any concurrent mutation to
  // pred.link (state transition, walker mark/finalize, opp-splice on
  // pred's predecessor) fails the CAS — caller restarts.
  LIBC_INLINE bool try_splice_at_pred(uint16_t pred,
                                        linkage::Link expected_pred_link,
                                        uint16_t new_next) {
    linkage::Link desired = expected_pred_link.with_next_uncertify(new_next);
    return slots_[pred].link.compare_exchange_strong(
        expected_pred_link, desired, cpp::MemoryOrder::ACQ_REL,
        cpp::MemoryOrder::ACQUIRE);
  }

  // Grow the pool by one chunk. `committed_slots_` is purely internal
  // coordination — walkers iterate the active chain via Link traversal,
  // never read this watermark.
  //
  // Phase order:
  //   1. CAS-claim a chunk via committed_slots_ — only one thread
  //      commits a given range.
  //   2. ensure_committed page-commits the corresponding VA. Failure
  //      is unrecoverable (kernel commit-charge OOM, not RAM); trap.
  //   3. Init slots in the chunk.
  //   4. Splice the chunk's chain onto the freelist head.
  // The final freelist splice's RELEASE CAS makes every step 1-3 store
  // happens-before any subsequent freelist_pop's ACQUIRE load.
  [[nodiscard]] LIBC_INLINE bool commit_more_slots() {
    uint32_t cur = committed_slots_.load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(cur >= kCrystallineSlotPoolCapacity))
      return false;
    uint32_t end = cur + kCrystallineSlotsPerCommitStep;
    if (end > kCrystallineSlotPoolCapacity)
      end = kCrystallineSlotPoolCapacity;

    // Phase 1: claim the chunk. Loser returns true so the caller's
    // claim_slot loop re-checks the freelist (winner's chunk may
    // already be linked).
    if (!committed_slots_.compare_exchange_strong(cur, end,
                                                  cpp::MemoryOrder::ACQ_REL,
                                                  cpp::MemoryOrder::ACQUIRE))
      return true;

    // Phase 2: commit pages. No bail path — page_commit only fails on
    // kernel commit-charge / quota limits, not RAM exhaustion. Under
    // any realistic workload this never fires; under the OOM scenario
    // where it could, the process is dying anyway.
    if (LIBC_UNLIKELY(!slot_region_.ensure_committed(
            static_cast<size_t>(end) * sizeof(CrystallineDomainSlot))))
      __builtin_trap();

    // Phase 3: stamp constructor-init field values + link tombstone
    // across the newly-committed range.
    for (uint32_t i = cur; i < end; ++i)
      reset_slot_fields(slots_[i]);
    for (uint32_t i = cur; i < end - 1; ++i)
      slots_[i].link.store(linkage::Link::pack_certified(
                               static_cast<uint8_t>(SlotPoolState::FREE),
                               /*tag=*/0,
                               static_cast<uint16_t>(i + 1)),
                           cpp::MemoryOrder::RELAXED);

    // Phase 4: splice tail of new chain onto current freelist head.
    // CAS-loop because freelist_.head can change concurrently — same
    // Treiber discipline as wait_slot.cpp:274-282.
    for (;;) {
      uint32_t old = freelist_.head.load(cpp::MemoryOrder::ACQUIRE);
      slots_[end - 1].link.store(
          linkage::Link::pack_certified(
              static_cast<uint8_t>(SlotPoolState::FREE), /*tag=*/0,
              static_cast<uint16_t>(fl_head(old))),
          cpp::MemoryOrder::RELAXED);
      uint32_t desired = fl_pack(fl_gen(old) + 1, cur);
      if (freelist_.head.compare_exchange_weak(old, desired,
                                               cpp::MemoryOrder::RELEASE,
                                               cpp::MemoryOrder::RELAXED))
        return true;
    }
  }

  internal::CommitRegion slot_region_{};
  CrystallineDomainSlot *slots_ = nullptr;
  // Internal coordinator for commit_more_slots — never read by walkers.
  cpp::Atomic<uint32_t> committed_slots_{0};
  CrystallineFreelistLine freelist_{};    // FREE slots
  CrystallineFreelistLine active_head_{}; // CLAIMED slots (walker entry)
};

} // namespace concurrent
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_SLOT_POOL_H
