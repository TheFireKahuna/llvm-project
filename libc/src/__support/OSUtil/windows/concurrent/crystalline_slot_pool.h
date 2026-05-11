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
#include "src/__support/OSUtil/windows/memory/legacy/commit_region.h"
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

// Cache-line isolated chain head: `[gen:48 | head:16]` packing defends
// the Treiber stack against ABA on the head field even before the per-
// slot tag bump kicks in. Used for both freelist_ and active_head_.
//
// Gen is 48-bit so wrap-around takes ~2.8e14 head mutations — at a
// sustained 1 GHz mutation rate that is ~9 years, structurally
// unreachable. A 16-bit gen wrapped under realistic 16-thread
// teardown workloads (~75K head ops per `[E]` bench section) and
// produced active-chain self-loops via active_push reading
// `old.head == idx` post-wrap; the dump showed `slot N self-loop
// (next=N)` after roughly 7K successful CASes — gen had wrapped at
// least once during the run.
struct alignas(64) CrystallineFreelistLine {
  cpp::Atomic<uint64_t> head{0};
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
  // the substrate harris_unlink walker, then push onto freelist.
  //
  // Caller holds idx exclusively (claim/release ownership), so
  // capturing the slot's generation here is race-free against
  // freelist_push (only this thread's release path bumps it) — the
  // substrate walker uses it to bind the splice CAS path against
  // accidental cross-lifecycle reuse if any other path were to bump
  // gen mid-walk.
  LIBC_INLINE void release_slot(uint16_t idx) {
    // Tombstone fields BEFORE the splice. While the slot is still on
    // the active chain, peer walkers see inv_ptr in first[] and zero
    // in era/state — the existing algorithmic filters skip it the
    // same way they skip a never-claimed slot.
    reset_slot_fields(slots_[idx]);
    uint32_t my_gen =
        slots_[idx].generation.load(cpp::MemoryOrder::ACQUIRE);
    // Substrate walker drains all chain-shape races internally
    // (loops on Retry until SplicedReclaim / SplicedNoReclaim /
    // NotFound). Pool's DeadPolicy declares target_state_reclaims
    // ≡ true — caller always proceeds to freelist_push regardless
    // of whether the walker self-spliced or vacuously walked-to-end.
    SubstrateCtx ctx{*this};
    (void)linkage::harris_unlink<SubstrateCtx, PoolDeadPolicy>(ctx, idx,
                                                                my_gen);
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

  // Diagnostic-only getters — packed [gen:48|head:16] words. Used by
  // crystalline_slot_pool_stress to record observed values per op boundary
  // for race localization. Production code should not consume these.
  LIBC_INLINE uint64_t debug_active_head_packed() {
    return active_head_.head.load(cpp::MemoryOrder::ACQUIRE);
  }
  LIBC_INLINE uint64_t debug_freelist_head_packed() {
    return freelist_.head.load(cpp::MemoryOrder::ACQUIRE);
  }
  LIBC_INLINE uint32_t debug_committed_slots() {
    return committed_slots_.load(cpp::MemoryOrder::ACQUIRE);
  }

private:
  // ----- [gen:48|head:16] packers (64-bit head packing).
  static constexpr uint32_t kFlGenShift = 16;
  static constexpr uint64_t kFlIndexMask = (1ull << kFlGenShift) - 1;
  LIBC_INLINE static uint64_t fl_head(uint64_t packed) {
    return packed & kFlIndexMask;
  }
  LIBC_INLINE static uint64_t fl_gen(uint64_t packed) {
    return packed >> kFlGenShift;
  }
  LIBC_INLINE static uint64_t fl_pack(uint64_t gen, uint64_t head) {
    return (gen << kFlGenShift) | (head & kFlIndexMask);
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
      slot.era[j].pair[0].store(0, cpp::MemoryOrder::RELAXED);
      slot.era[j].pair[1].store(0, cpp::MemoryOrder::RELAXED);
      slot.state[j].result.pair[0].store(0, cpp::MemoryOrder::RELAXED);
      slot.state[j].result.pair[1].store(0, cpp::MemoryOrder::RELAXED);
      slot.state[j].load_thunk.store(nullptr, cpp::MemoryOrder::RELAXED);
      slot.state[j].load_ctx.store(nullptr, cpp::MemoryOrder::RELAXED);
      slot.state[j].parent.store(nullptr, cpp::MemoryOrder::RELAXED);
      slot.state[j].birth_era.store(0, cpp::MemoryOrder::RELAXED);
    }
  }

  LIBC_INLINE void init_slot_storage_at_origin(uint32_t hwm) {
    for (uint32_t i = 0; i < hwm; ++i)
      reset_slot_fields(slots_[i]);
  }

  // Treiber pop from freelist. Returns 0 only on pool exhaustion.
  LIBC_INLINE uint16_t freelist_pop() {
    for (;;) {
      uint64_t old = freelist_.head.load(cpp::MemoryOrder::ACQUIRE);
      uint16_t head = static_cast<uint16_t>(fl_head(old));
      if (head == kCrystallineSlotNullIndex) {
        if (!commit_more_slots())
          return kCrystallineSlotNullIndex;
        continue;
      }
      uint16_t next = slots_[head].link.load(cpp::MemoryOrder::ACQUIRE).next();
      uint64_t desired = fl_pack(fl_gen(old) + 1, next);
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
  //
  // generation bump is the substrate walker's slot-lifecycle ABA
  // defense: any concurrent harris_walk_attempt that captured this
  // slot's gen pre-push observes the bumped value at its target
  // re-check and returns NotFound rather than splicing on a slot
  // about to be reallocated. RELEASE ordering pairs with the
  // walker's ACQUIRE load + the freelist_.head ACQUIRE load on the
  // pop side, so the bumped gen happens-before any post-pop reuse.
  LIBC_INLINE void freelist_push(uint16_t idx) {
    slots_[idx].generation.fetch_add(1, cpp::MemoryOrder::RELEASE);
    for (;;) {
      uint64_t old = freelist_.head.load(cpp::MemoryOrder::ACQUIRE);
      linkage::link_store(slots_[idx].link,
                          static_cast<uint8_t>(SlotPoolState::FREE),
                          static_cast<uint16_t>(fl_head(old)));
      uint64_t desired = fl_pack(fl_gen(old) + 1, idx);
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
      uint64_t old = active_head_.head.load(cpp::MemoryOrder::ACQUIRE);
      cur = linkage::link_store_next_uncertify_known(
          slots_[idx].link, cur,
          static_cast<uint16_t>(fl_head(old)));
      uint64_t desired = fl_pack(fl_gen(old) + 1, idx);
      if (active_head_.head.compare_exchange_weak(old, desired,
                                                  cpp::MemoryOrder::RELEASE,
                                                  cpp::MemoryOrder::RELAXED))
        return;
    }
  }

  // Head splice via active_head_ CAS. Returns false if the chain head
  // is no longer `expected_head` (caller restarts). Loops on weak-CAS
  // spurious failures while the head still matches.
  //
  // Called both from active_push's CAS-publish loop and from the
  // substrate harris walker's SubstrateCtx::try_splice_head adapter.
  LIBC_INLINE bool try_splice_active_head(uint16_t expected_head,
                                            uint16_t new_top) {
    for (;;) {
      uint64_t old = active_head_.head.load(cpp::MemoryOrder::ACQUIRE);
      if (static_cast<uint16_t>(fl_head(old)) != expected_head)
        return false;
      uint64_t desired = fl_pack(fl_gen(old) + 1, new_top);
      if (active_head_.head.compare_exchange_weak(old, desired,
                                                  cpp::MemoryOrder::ACQ_REL,
                                                  cpp::MemoryOrder::RELAXED))
        return true;
    }
  }

  // Substrate harris walker's Ctx adapter — wraps `*this` so the
  // walker template can call into the pool's link/gen/head accessors
  // without indirection. All members LIBC_INLINE so the walker
  // compiles to identical code as the prior open-coded active_splice.
  //
  // Refers to the enclosing CrystallineSlotPool by reference; the
  // adapter is constructed transiently in release_slot's stack frame
  // (`SubstrateCtx ctx{*this};`) and lives only across one
  // harris_unlink call.
  struct SubstrateCtx {
    CrystallineSlotPool &pool;
    static constexpr uint16_t kNullIndex = kCrystallineSlotNullIndex;

    LIBC_INLINE cpp::Atomic<linkage::Link> &link_at(uint16_t idx) {
      return pool.slots_[idx].link;
    }
    LIBC_INLINE uint32_t load_gen(uint16_t idx) {
      return pool.slots_[idx].generation.load(cpp::MemoryOrder::ACQUIRE);
    }
    LIBC_INLINE uint16_t load_head() {
      return static_cast<uint16_t>(
          pool.fl_head(pool.active_head_.head.load(
              cpp::MemoryOrder::ACQUIRE)));
    }
    LIBC_INLINE bool try_splice_head(uint16_t expected_head,
                                      uint16_t new_top) {
      return pool.try_splice_active_head(expected_head, new_top);
    }
    // Pool's DeadPolicy declares no dead-intermediate states, so the
    // substrate walker never invokes this hook. Defined only to
    // satisfy the duck-typed interface; never called at runtime.
    LIBC_INLINE void on_dead_intermediate_reclaim(uint16_t /*idx*/,
                                                    uint8_t /*pre_state*/) {}
  };

  // Substrate harris walker's DeadPolicy. Pool's link-state lattice:
  //
  //   CLAIMED + CERT=0   on the live chain (live).
  //   CLAIMED + CERT=1   post-finalize, pre-freelist-push transient
  //                      (DEAD-INTERMEDIATE — walker opp-splices).
  //   CLAIMED + MARK=1   mid-release (mark-curr help branch handles).
  //   FREE    + CERT=1   on freelist (idle-on-chain — walker Retries).
  //
  // The dead-intermediate case is load-bearing for chain self-healing.
  // When two threads release adjacent slots concurrently, T1's target
  // case can publish prev.next = (slot T2 is mid-releasing). Without
  // dead-intermediate handling, that stale link survives T2's
  // freelist_push as a FREE-on-chain entry and walkers retry forever.
  // With dead-intermediate handling, walkers passing through the
  // post-finalize / pre-push window (CLAIMED+CERT=1) opp-splice the
  // slot using its still-chain-linkage `.next`, repairing the chain
  // before it transitions to FREE.
  //
  // target_state_reclaims ≡ true: caller of harris_unlink is the
  // slot's owner (release_slot) and always proceeds to freelist_push
  // regardless of walker outcome. on_dead_intermediate_reclaim is a
  // no-op — walker only repairs the chain; the slot's lifecycle owner
  // (release_slot caller) handles the eventual freelist_push.
  struct PoolDeadPolicy {
    static constexpr bool is_idle_on_chain(uint8_t state) {
      return state == static_cast<uint8_t>(SlotPoolState::FREE);
    }
    static constexpr bool is_dead_intermediate(linkage::Link l) {
      return l.state() == static_cast<uint8_t>(SlotPoolState::CLAIMED) &&
             l.is_certified();
    }
    static constexpr bool target_state_reclaims(uint8_t /*pre_mark_state*/) {
      return true;
    }
  };

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
      uint64_t old = freelist_.head.load(cpp::MemoryOrder::ACQUIRE);
      slots_[end - 1].link.store(
          linkage::Link::pack_certified(
              static_cast<uint8_t>(SlotPoolState::FREE), /*tag=*/0,
              static_cast<uint16_t>(fl_head(old))),
          cpp::MemoryOrder::RELAXED);
      uint64_t desired = fl_pack(fl_gen(old) + 1, cur);
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
