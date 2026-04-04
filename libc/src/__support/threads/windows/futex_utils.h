//===--- Futex for Windows with CAS-64 Treiber wait stack -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// 8-byte Futex: union { Atomic<u64> combined_; struct { Atomic<u32> stack_;
// Atomic<FutexWordType> value_; }; }. Little-endian layout.
//
//   value_    — caller-visible 32-bit futex word; direct LOCK'd RMWs.
//   stack_    — Treiber wait stack head, [gen:16 | top:16]. CAS-32 pop.
//   combined_ — 64-bit view for atomic value-check + push: one CAS-64 both
//               verifies value hasn't changed AND publishes the new head,
//               so push has no Dekker re-check and no lost-wakeup window.
//
// Wake fold — one pre-mark CAS publishes everything; detach is best-effort:
//   (B') link_cas_snap WAITING/IN_KERNEL → SIGNALED_*_ORPHAN. Single CAS
//        carries wake signal + wake kind + cleanup responsibility + tag
//        bump, closing both the harris walker's mid-splice race and the
//        old separate-wake_word race window.
//   (A') stack_ CAS detach. Loss leaves slot ORPHAN; waiter self-splices
//        via self_splice_if_orphan, keeping unlock O(1).
//   (U)  On detach win: link_cas_state_certify atomically upgrades
//        ORPHAN → CLEAN and publishes LINK_CERT_BIT. Waiter then sees
//        CLEAN+CERT (skip splice) or ORPHAN+CERT=0 (splice; harris's
//        splice-success path publishes CERT before clear_slot_owned
//        consumes it). LINK_CERT_BIT is the structural off-chain
//        certificate that gates clear_slot_owned's IDLE write — see
//        wait_slot.h for the full bit-level protocol.
//
// LIFO wake order — cache-warm, matches Linux qspinlock / parking_lot.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_UTILS_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_UTILS_H

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/optional.h"
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/OSUtil/windows/nt/shared_user_data.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/threads/windows/futex_word.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/threads/windows/wait_slot.h"
#include "src/__support/time/abs_timeout.h"

namespace LIBC_NAMESPACE_DECL {

// Clear owner-side linkage on an off-chain slot. Terminal state is
// IDLE+CERT=1+MARK=0 with tag bumped, reached via one of two paths:
//
//   Fast path (CERT=1 && MARK=0)  plain RELEASE store of IDLE+CERT=1.
//                                 No racer can interleave; saves one
//                                 LOCK'd RMW per wait completion.
//   Slow path (CERT=0 || MARK=1)  CAS-loop. A racing walker fold or
//                                 upstream certify CAS could otherwise
//                                 clobber our CERT publish.
//
// Ordering (load-bearing):
//   (0) gen++ RELEASE  — single barrier; remaining stores RELAXED.
//   (1) TLS gen refresh — owner's fast-path reuse compare matches.
//   (2) subsystem→None BEFORE (3) so racing thread-exit cleanup,
//       which dispatches on subsystem, skips us.
//   (3) IDLE+CERT publish.
//   (4) wait_address cleared (invalidation gate for racing wakers).
//
// filter_fn / filter_arg are NOT cleared here. Wakers only read them
// from chain-resident slots (state == WAITING || IN_KERNEL — see
// pop_and_signal_one and signal_first_match_after); an IDLE slot's
// filter is dormant. Phase 2 setup unconditionally rewrites both
// fields on every wait entry (HasPredicate=true: caller's pred/arg;
// HasPredicate=false: nullptr/0), so the slot's filter at chain-
// publish time always reflects the current wait. The redundant
// clear-then-rewrite saved nothing and cost two RELAXED stores per
// wait completion.
LIBC_INLINE void clear_slot_owned(wait_slot::WaitSlot &slot, uint32_t my_idx) {
  // Capture fetch_add's return so refresh_tls_slot_generation reuses
  // the new gen directly — avoids a second ACQUIRE on the gen atomic.
  uint32_t new_gen =
      slot.generation.fetch_add(1, cpp::MemoryOrder::RELEASE) + 1;
  wait_slot::refresh_tls_slot_generation(my_idx, new_gen);
  slot.subsystem.store(wait_slot::SubsystemKind::None,
                        cpp::MemoryOrder::RELAXED);

  linkage::Link link = slot.link.load(cpp::MemoryOrder::ACQUIRE);
  if (LIBC_LIKELY(link.is_certified() && !link.is_marked())) {
    // Owner-exclusive off-chain (no walker mid-fold, no upstream CERT
    // pending): plain RELEASE store of IDLE+CERT=1 is race-free.
    slot.link.store(link.with_state_certified(wait_slot::IDLE),
                     cpp::MemoryOrder::RELEASE);
  } else {
    // CAS-loop covers the [t0, t1] window where a racing walker fold
    // or upstream certify CAS would otherwise clobber our CERT publish.
    for (;;) {
      linkage::Link old = slot.link.load(cpp::MemoryOrder::ACQUIRE);
      linkage::Link desired =
          old.with_state_certified(wait_slot::IDLE);
      if (slot.link.compare_exchange_weak(old, desired,
                                            cpp::MemoryOrder::ACQ_REL,
                                            cpp::MemoryOrder::ACQUIRE))
        break;
    }
  }

  slot.wait_address.store(0, cpp::MemoryOrder::RELAXED);
}

// Load-bearing invariants every waker/waiter site must preserve.
//
//  I1. LINK_CERT_BIT (slot.link bit 49) ⇒ slot is provably off-chain.
//      Every detach-prover sets CERT atomically with detach (waker
//      upgrade, walker splice, stack-steal, reclaim, freelist push,
//      bail re-publish); push commit clears it. clear_slot_owned
//      gates the IDLE write on CERT=1; get_slot_index gates TLS
//      reuse on CERT=1. SIGNALED_*_CLEAN ⇒ CERT=1 by construction.
//      Full bit protocol in wait_slot.h.
//  I2. Wakers pre-mark via link_cas_snap — single atomic carrying
//      wake + wake-kind + cleanup-responsibility + tag bump. Stale
//      walker CAS fails on tag/state mismatch.
//  I4. Wakers capture slot.thread_id AND owner_handle_packed(slot)
//      TOGETHER pre-detach. Late owner-handle capture binds a
//      recycled slot's new owner while captured tid names the old →
//      wrong-TID alert. Enforced by WakeCommitToken /
//      capture_wake_target.
//  I5. Owners exit Phase 4 via exactly one of:
//        (a) state_is_signaled(st) → decode_signaled_ret;
//        (b) cancel_or_absorb(...) → unified CAS + decode-on-race;
//        (c) reloop (spurious)     → refresh entry_epoch.
//
// Stack-resident alert ring for broadcast paths (drain_waiters,
// notify_all, notify_all_chunked). A 128-entry CompactTarget+HANDLE
// pair is 2 KB on stack — fits L1 alongside the stolen-slot lines —
// and replaces the prior ScratchAlloc-on-ThreadScratch arena. Mid-walk
// flushes fire alert_multiple_if_live_compact when the ring fills,
// reset to 0, and continue stealing. No allocation, no failure path,
// no per-slot fallback: at small N a single end-of-walk flush wakes
// everyone in one syscall; at large N each fill amortizes the syscall
// while letting the kernel start delivering alerts before we finish
// the walk. WAITING-only broadcasts never accumulate (Phase 2.5 cache
// spin catches them), so the ring stays at 0 and no syscall fires.
inline constexpr uint32_t kAlertBatch = 128;

// Drain a broadcast walk's accumulated CompactTargets via one
// alert_multiple_if_live_compact call. No-op when the ring is empty.
// Resets `scratch_used` so the caller can keep walking. Used by
// drain_waiters, notify_all, and notify_all_chunked at every cap-fill
// and at end-of-walk / end-of-chunk.
LIBC_INLINE void
flush_alert_batch(wait_slot::CompactTarget *tgt_buf, HANDLE *out_buf,
                  uint32_t &scratch_used,
                  PS_ALERT_THREAD_EXTENDED_PARAMETER *ab_ctx) {
  if (scratch_used == 0)
    return;
  (void)wait_slot::alert_multiple_if_live_compact(
      tgt_buf, scratch_used, out_buf, ab_ctx, 1);
  scratch_used = 0;
}

class Futex {
  // LE: offset 0 = stack_ [gen:16 | top:16], offset 4 = value_.
  // CAS-64 on combined_ acts on both; native RMW on value_ leaves
  // stack_ alone; CAS-32 on stack_ leaves value_ alone.
  union {
    cpp::Atomic<uint64_t> combined_;
    struct {
      cpp::Atomic<uint32_t> stack_;
      cpp::Atomic<FutexWordType> value_;
    };
  };

  static constexpr uint32_t STACK_GEN_SHIFT = 16;
  static constexpr uint32_t STACK_INDEX_MASK = 0xFFFFu;
  static constexpr uint16_t STACK_NULL = static_cast<uint16_t>(wait_slot::NULL_INDEX);

  LIBC_INLINE static constexpr uint16_t stack_top(uint32_t s) {
    return static_cast<uint16_t>(s & STACK_INDEX_MASK);
  }
  LIBC_INLINE static constexpr uint16_t stack_gen(uint32_t s) {
    return static_cast<uint16_t>(s >> STACK_GEN_SHIFT);
  }
  LIBC_INLINE static constexpr uint32_t stack_pack(uint16_t gen, uint16_t top) {
    return (static_cast<uint32_t>(gen) << STACK_GEN_SHIFT) | top;
  }

  // combined_ word: [value:32 upper | stack:32 lower]
  LIBC_INLINE static constexpr uint32_t combined_value(uint64_t c) {
    return static_cast<uint32_t>(c >> 32);
  }
  LIBC_INLINE static constexpr uint32_t combined_stack(uint64_t c) {
    return static_cast<uint32_t>(c);
  }
  LIBC_INLINE static constexpr uint64_t combined_pack(uint32_t val, uint32_t stk) {
    return (static_cast<uint64_t>(val) << 32) | stk;
  }

public:
  using Timeout = internal::AbsTimeout;

  // Unified waiter descriptor.
  //   Classic    — pred=nullptr, arg=expected. Exit: v != arg.
  //   Predicated — pred=user fn, arg=user arg. Exit: pred(v, arg).
  //                The same fn is installed on slot.filter_fn so the
  //                waker agrees. Caller notify contract: every
  //                FALSE→TRUE flip MUST pair with notify_*; no
  //                eventual re-check fallback (load-bearing).
  // wait_impl's HasPredicate template axis compile-time-prunes the
  // pred branch on the classic path — satisfied() folds to a plain
  // `v != arg` compare with no function-pointer indirection.
  struct WaitCondition {
    FutexValueType arg;
    wait_slot::PredicateFn pred; // nullptr = classic v != arg

    LIBC_INLINE bool satisfied(FutexValueType v) const {
      return pred ? pred(v, arg) : (v != arg);
    }

    LIBC_INLINE static constexpr WaitCondition eq(FutexValueType expected) {
      return WaitCondition{expected, nullptr};
    }
    LIBC_INLINE static constexpr WaitCondition
    on_predicate(wait_slot::PredicateFn pred, uint32_t arg) {
      return WaitCondition{arg, pred};
    }
  };

  // Tri-state return from handoff_one.
  //   Empty     — no waiter woken. Caller MUST store unlock_val AND
  //               pop_and_signal_one (Dekker catch for any waiter
  //               pushed after handoff_one's scan).
  //   Completed — handoff_one already stored unlock_val AND notified
  //               the target. Caller MUST NOT store/pop. Covers both
  //               IN_KERNEL wake and WAITING self-committed fallback.
  //   Handoff   — ownership transferred via HANDOFF on a WAITING
  //               target; value stays at locked sentinel. Caller
  //               MUST NOT store/pop.
  // Completed exists so handoff_one owns the store-BEFORE-wake order
  // for IN_KERNEL waiters — inverting it is the hang where the waker
  // is preempted between alert and store, the waiter re-CAS's stale
  // LOCKED, and re-parks (livelock).
  enum class UnlockOutcome : uint8_t {
    Empty = 0,
    Completed = 1,
    Handoff = 2,
  };

  LIBC_INLINE constexpr Futex(FutexValueType value)
      : combined_(combined_pack(value,
                                stack_pack(0, STACK_NULL))) {}

  // Recover the Futex* a slot is parked on, gated on subsystem tag.
  // Returns nullptr iff subsystem != Futex (slot on a parking-lot
  // SLL, IDLE, or being torn down). The ACQUIRE on subsystem pairs
  // with Phase 2's wait_address-BEFORE-subsystem publish.
  //
  // ONLY blessed cast site for a SubsystemKind::Futex-tagged
  // wait_address. The harris_unlink_futex_trampoline at file end is
  // the one other cast and is type-safe by registration discipline.
  // Any new reinterpret_cast<Futex*>(slot.wait_address) is a layering
  // bug — route through this accessor.
  LIBC_INLINE static Futex *
  try_from_owner_slot(wait_slot::WaitSlot &slot) {
    if (slot.subsystem.load(cpp::MemoryOrder::ACQUIRE) !=
        wait_slot::SubsystemKind::Futex)
      return nullptr;
    return reinterpret_cast<Futex *>(
        slot.wait_address.load(cpp::MemoryOrder::RELAXED));
  }

  LIBC_INLINE FutexValueType get_value(
      cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) const {
    return const_cast<Futex *>(this)->value_.load(ord);
  }

  // Standard atomics — direct passthrough to value_; stack_ untouched.

  LIBC_INLINE FutexValueType
  load(cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return value_.load(ord);
  }

  LIBC_INLINE void
  store(FutexValueType val,
        cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    value_.store(val, ord);
  }

  LIBC_INLINE FutexValueType
  exchange(FutexValueType val,
           cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return value_.exchange(val, ord);
  }

  LIBC_INLINE FutexValueType
  fetch_add(FutexValueType delta,
            cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return value_.fetch_add(delta, ord);
  }

  LIBC_INLINE FutexValueType
  fetch_sub(FutexValueType delta,
            cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return value_.fetch_sub(delta, ord);
  }

  LIBC_INLINE FutexValueType
  fetch_or(FutexValueType mask,
           cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return value_.fetch_or(mask, ord);
  }

  LIBC_INLINE FutexValueType
  fetch_and(FutexValueType mask,
            cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return value_.fetch_and(mask, ord);
  }

  LIBC_INLINE bool
  compare_exchange_weak(FutexValueType &expected, FutexValueType desired,
                        cpp::MemoryOrder success_ord,
                        cpp::MemoryOrder failure_ord) {
    return value_.compare_exchange_weak(expected, desired,
                                        success_ord, failure_ord);
  }

  LIBC_INLINE bool
  compare_exchange_weak(FutexValueType &expected, FutexValueType desired,
                        cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return compare_exchange_weak(expected, desired, ord,
                                 ord == cpp::MemoryOrder::RELEASE
                                     ? cpp::MemoryOrder::RELAXED
                                     : ord);
  }

  LIBC_INLINE bool
  compare_exchange_strong(FutexValueType &expected, FutexValueType desired,
                          cpp::MemoryOrder success_ord,
                          cpp::MemoryOrder failure_ord) {
    return value_.compare_exchange_strong(expected, desired,
                                          success_ord, failure_ord);
  }

  LIBC_INLINE bool
  compare_exchange_strong(FutexValueType &expected, FutexValueType desired,
                          cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return compare_exchange_strong(expected, desired, ord,
                                   ord == cpp::MemoryOrder::RELEASE
                                       ? cpp::MemoryOrder::RELAXED
                                       : ord);
  }

  LIBC_INLINE operator FutexValueType() {
    return load(cpp::MemoryOrder::SEQ_CST);
  }

  // DELETED on purpose. `futex = v;` emits a plain RELEASE store; a
  // subsequent notify_*() then deadlocks on x86 because StoreLoad
  // reorders the MOV past the notify's ACQUIRE load of stack_. Use
  // store_and_notify(v) / store_and_notify_all(v) (both SEQ_CST), or
  // an explicit RMW (LOCK-prefixed, safe to follow with notify).
  Futex &operator=(FutexValueType value) = delete;

  LIBC_INLINE bool operator==(FutexValueType rhs) {
    return load(cpp::MemoryOrder::SEQ_CST) == rhs;
  }

  LIBC_INLINE bool operator!=(FutexValueType rhs) { return !(*this == rhs); }

  // First-time init of opaque storage (pthread_mutex_t, sem_t, etc.)
  // where stack_ may carry garbage. No drain — no legitimate waiters
  // can exist yet.
  LIBC_INLINE void init(FutexValueType val) {
    combined_.store(combined_pack(val, stack_pack(0, STACK_NULL)),
                    cpp::MemoryOrder::RELAXED);
  }

  // Drain stale waiters first (so they observe wait_address==0 and
  // bail with EINVAL instead of operating on recycled storage), then
  // atomically zero combined_. Drain is a no-op on empty stacks.
  // Atomic store mandatory: a non-atomic 64-bit write could tear
  // against a stale waiter's CAS-64.
  LIBC_INLINE void reset(FutexValueType val = 0) {
    drain_waiters();
    combined_.store(combined_pack(val, stack_pack(0, STACK_NULL)),
                    cpp::MemoryOrder::RELEASE);
  }

  // Fork child path: zero combined_ WITHOUT draining. Stale waiter
  // slots name parent-process threads, and Windows TIDs are system-
  // wide — drain_waiters would alert the parent. The child has no
  // threads to wake; lazy cleanup via the pop-side wait_address check.
  LIBC_INLINE void reset_for_fork(FutexValueType val = 0) {
    combined_.store(combined_pack(val, stack_pack(0, STACK_NULL)),
                    cpp::MemoryOrder::RELEASE);
  }

  // Drain all waiters from the embedded Treiber stack, clearing
  // wait_address on each so they wake and bail with EINVAL instead of
  // operating on recycled storage. Used by mutex destroy.
  LIBC_INLINE void drain_waiters() {
    // Empty short-circuit common during destroy of an uncontended
    // futex.
    if (stack_top(stack_.load(cpp::MemoryOrder::ACQUIRE)) == STACK_NULL)
      return;

    // 128-entry stack ring. CompactTarget (8B vs BatchTarget's 24B)
    // suffices because the stack-steal CAS is itself the state-
    // transition authority — alert-time TID mismatch proves the owner
    // already woke independently. AutoBoost correlation = wait-key
    // address (`this` matches slot.wait_address set in Phase 2);
    // reused on every flush_alert_batch.
    wait_slot::CompactTarget tgt_buf[kAlertBatch];
    HANDLE out_buf[kAlertBatch];
    uint32_t scratch_used = 0;
    PS_ALERT_THREAD_EXTENDED_PARAMETER ab_ctx{};
    ab_ctx.Pointer = this;

    // Steal the entire stack atomically. The hoisted empty-check
    // above was a hint; the in-loop check below handles a concurrent
    // drainer between hint load and steal CAS.
    uint32_t old_stk;
    for (;;) {
      old_stk = stack_.load(cpp::MemoryOrder::ACQUIRE);
      if (stack_top(old_stk) == STACK_NULL)
        return;
      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1, STACK_NULL);
      if (stack_.compare_exchange_weak(old_stk, new_stk,
                                        cpp::MemoryOrder::ACQ_REL,
                                        cpp::MemoryOrder::RELAXED))
        break;
    }

    // Software-pipelined walk: DEPTH in-flight ACQUIRE loads overlap
    // cold-line DRAM latency across iterations instead of serializing
    // behind each LOCK XCHG. Depth=4 covers ~200–250 ns DRAM miss at
    // ~50–80 ns per slot.
    uint32_t curr = stack_top(old_stk);
    static constexpr uint32_t PIPELINE_DEPTH = 4;
    struct PipeEntry {
      uint32_t idx;
      linkage::Link snap;
    };
    PipeEntry pipe[PIPELINE_DEPTH];
    uint32_t pipe_head = 0;
    uint32_t pipe_fill = 0;

    auto pipe_push_one = [&]() -> bool {
      if (curr == wait_slot::NULL_INDEX)
        return false;
      uint32_t tail = (pipe_head + pipe_fill) % PIPELINE_DEPTH;
      pipe[tail].idx = curr;
      pipe[tail].snap =
          wait_slot::get_slot(curr).link.load(cpp::MemoryOrder::ACQUIRE);
      curr = linkage::link_next(pipe[tail].snap);
      ++pipe_fill;
      return true;
    };

    while (pipe_fill < PIPELINE_DEPTH && pipe_push_one())
      ;

    while (pipe_fill > 0) {
      uint32_t slot_idx = pipe[pipe_head].idx;
      linkage::Link slot_snap = pipe[pipe_head].snap;
      pipe_head = (pipe_head + 1) % PIPELINE_DEPTH;
      --pipe_fill;

      auto &slot = wait_slot::get_slot(slot_idx);
      // wait_address=0 BEFORE the state CAS so the waiter's
      // invalidation check trips. wait_address=0 ALSO short-circuits
      // slot_cleanup's thread-exit dispatch (see wait_slot.cpp:277)
      // before it reads subsystem — so we don't need to clear
      // subsystem here on the !ours path. The `ours` branch below
      // clears subsystem alongside the wake commit, mirroring
      // commit_pop_wake's "clear-on-detach-win" pattern.
      slot.wait_address.store(0, cpp::MemoryOrder::RELAXED);

      // TID before the state CAS — CompactTarget skips owner_ref
      // capture; alert-time TID mismatch suffices. Capture pre-CAS
      // (I4 contract) so a recycled slot post-CAS can't bind a wrong
      // tid.
      uint32_t tid = slot.thread_id.load(cpp::MemoryOrder::RELAXED);
      bool ours = false;
      uint8_t old = linkage::link_cas_state_detached<
          wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(slot.link, slot_snap, ours);
      if (!ours) {
        // Owner / unrelated waker handled the slot independently
        // (typically: pre-mark + alert preceded our steal, owner
        // ran clear_slot_owned). Only TIMED_OUT obliges us to
        // reclaim — owner's harris would walk the post-steal NULL.
        // SIGNALED_*_ORPHAN ⇒ slot off-chain by our steal but our
        // failed CAS didn't publish CERT; follow up with certify CAS.
        // SIGNALED_*_CLEAN and IDLE already carry CERT=1 from their 
        // committing publisher.
        if (old == wait_slot::TIMED_OUT) {
          wait_slot::reclaim_slot(
              wait_slot::ReclaimAuthority::after_stack_steal(slot_idx));
        } else if (old == wait_slot::SIGNALED_ORPHAN) {
          (void)linkage::link_cas_state_certify<
              wait_slot::SIGNALED_ORPHAN, wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(
              slot.link);
        } else if (old == wait_slot::SIGNALED_HANDOFF_ORPHAN) {
          (void)linkage::link_cas_state_certify<
              wait_slot::SIGNALED_HANDOFF_ORPHAN,
              wait_slot::SIGNALED_HANDOFF_CLEAN, wait_slot::WaitSlotStateTraits>(slot.link);
        }
        pipe_push_one();
        continue;
      }
      // ours == true: our CAS won the wake commit. Clear subsystem
      // alongside the wake (matches commit_pop_wake's pattern); thread-
      // exit cleanup before owner runs clear_slot_owned dispatches a
      // harmless harris-walk-to-NULL on the post-steal stack.
      slot.subsystem.store(wait_slot::SubsystemKind::None,
                           cpp::MemoryOrder::RELAXED);
      if (old == wait_slot::IN_KERNEL) {
        tgt_buf[scratch_used++] = {tid, slot_idx};
        if (scratch_used == kAlertBatch)
          flush_alert_batch(tgt_buf, out_buf, scratch_used, &ab_ctx);
      } else if (old == wait_slot::WAITING) {
        // Phase 2.5 cache spin observes our state CAS — no alert.
      } else if (old == wait_slot::TIMED_OUT) {
        wait_slot::reclaim_slot(
            wait_slot::ReclaimAuthority::after_stack_steal(slot_idx));
      }
      // SIGNALED_*: concurrent pop/handoff already pre-marked
      // (benign — same wake + off-stack to same owner).

      pipe_push_one();
    }

    flush_alert_batch(tgt_buf, out_buf, scratch_used, &ab_ctx);
  }

  // ===== Walker race protocol =====
  //
  // Actors: W (harris walker), P (pop_and_signal_one / handoff_one),
  // A (notify_all / drain_waiters / notify_all_chunked), T (Phase-2
  // push), R (reclaim_slot direct freelist push).
  //
  //   T   head-only CAS-64; never rewrites mid-stack next.
  //   W   full 64-bit prev.link re-verify per hop catches any
  //       tag/state/next mutation; mark-then-help on observed MARK
  //       so a dead splicer cannot block progress.
  //   P   link_cas_snap → SIGNALED_* BEFORE the stack_ detach. The
  //       pre-mark bumps tag + state so any W mid-splice expecting
  //       the old snap fails. Detach success → link_cas_state_certify
  //       atomically upgrades ORPHAN → CLEAN AND publishes CERT.
  //       Detach loss → slot lingers SIGNALED_ORPHAN+CERT=0; walkers
  //       splice it like TIMED_OUT (publishing CERT atomically with
  //       splice success) but DO NOT reclaim (owner manages).
  //   A   stack-steal empties head (W's head path sees NULL); per-
  //       slot CAS-with-snap fails any in-flight W mid-splice;
  //       TIMED_OUT in the stolen list → gen bump caught by W.
  //   R   direct freelist push. Safe under the state+tag fold: any W
  //       capturing this link pre-reclaim fails its mid-splice CAS
  //       on tag/state mismatch; any W reading post-reclaim observes
  //       IDLE on a reachable chain → Retry. Pool memory is statically
  //       allocated and never freed, so stale derefs read valid memory.
  //
  // Reclaim is single-actor per lifecycle gen: TIMED_OUT ⇒
  // harris_unlink returns true (caller reclaims); SIGNALED_* ⇒
  // returns false (owner reclaims via clear_slot_owned). reclaim_slot
  // bumps target.generation EAGERLY (pre-push); W captures gen at
  // entry and re-checks before any CAS. ABA on the 32-bit per-slot
  // tag is astronomical (per-slot wraps require >2^32 link writes
  // within one walk's lifetime — physically impossible at any
  // realistic clock rate).
  //
  // Termination: each retry re-reads head; progress bounded by
  // concurrent wake activity (≤ thread count).

  // Lock-free find+splice on the Treiber stack. Callers: Phase 1.75
  // stale-self reclaim, thread-exit trampoline, Phase 4 self-splice.
  //
  // Splices dead intermediates opportunistically:
  //   TIMED_OUT  — owner self-cancel; walker reclaims.
  //   SIGNALED_* — popper pre-mark + lost detach (orphan); owner
  //                reclaims on wake.
  // IDLE on a reachable chain is the legitimate stale-snapshot signal
  // (stack-steal + reclaim's IDLE+CERT=1 write left .next pointing
  // into stolen successors); walker retries — NEVER assert.
  //
  // Splice-success atomically publishes LINK_CERT_BIT on the spliced
  // slot, satisfying clear_slot_owned's precondition for any owner
  // reaching its epilogue concurrently.
  //
  // `expected_gen` — caller's gen capture. Mismatch ⇒ slot was
  // reclaimed and possibly reallocated; MUST NOT splice (would steal
  // a live slot from an unrelated waiter). Return false immediately.
  //
  // Return true iff caller should reclaim_slot(target). The target-
  // splicing CAS is the single authority for reclaim rights — at
  // most one actor per lifecycle-gen sees true.
  //
  // Structured per-attempt outcome. int8_t so the outer dispatcher
  // can return `r == SplicedReclaim` directly as a bool.
  //   Retry            — race; re-walk from head.
  //   NotFound         — walked to NULL (or target gen advanced).
  //   SplicedNoReclaim — target SIGNALED_*: spliced; owner reclaims.
  //   SplicedReclaim   — target TIMED_OUT:  spliced; we reclaim.
  enum class WalkResult : int8_t {
    Retry = -1,
    NotFound = 0,
    SplicedNoReclaim = 1,
    SplicedReclaim = 2,
  };

  // Non-blocking, non-allocating, no syscalls.
  LIBC_INLINE bool harris_unlink(uint16_t target, uint32_t expected_gen) {
    if (target == wait_slot::NULL_INDEX)
      return false;

    auto &target_slot = wait_slot::get_slot(target);
    // Bind target's generation to the caller's expected gen. Any
    // subsequent reclaim bumps the slot's gen; the per-attempt
    // re-check inside harris_walk_attempt catches it before any
    // splice CAS.
    if (target_slot.generation.load(cpp::MemoryOrder::ACQUIRE) !=
        expected_gen)
      return false;
    const uint32_t target_gen_entry = expected_gen;

    // No hazard-window bracketing: reclaim_slot pushes directly to
    // the freelist; the state+tag fold (every link write bumps tag,
    // mid-splice CAS validates the full link including tag) ensures
    // a stale walker snapshot fails CAS, and IDLE-on-reachable-chain
    // is the documented Retry signal. Pool memory is statically
    // allocated and never freed — stale derefs read valid memory.
    for (;;) {
      WalkResult r =
          harris_walk_attempt(target, target_gen_entry, target_slot);
      if (r == WalkResult::Retry)
        continue;
      return r == WalkResult::SplicedReclaim;
    }
  }

private:
  // Splice helpers used by harris_walk_attempt. Each returns the
  // splice's success bool directly — no mutable flag the caller can
  // forget to update.

  // Head splice via combined_ CAS. Returns false if stack_top is no
  // longer expected_head (caller restarts); loops on weak-CAS
  // spurious failures while it still matches.
  LIBC_INLINE bool try_splice_head(uint16_t expected_head,
                                    uint16_t new_top) {
    for (;;) {
      uint64_t old_c = combined_.load(cpp::MemoryOrder::ACQUIRE);
      uint32_t old_stk = combined_stack(old_c);
      if (stack_top(old_stk) != expected_head)
        return false;
      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1, new_top);
      uint64_t desired = combined_pack(combined_value(old_c), new_stk);
      if (combined_.compare_exchange_weak(old_c, desired,
                                           cpp::MemoryOrder::ACQ_REL,
                                           cpp::MemoryOrder::RELAXED))
        return true;
    }
  }

  // Mid splice via pred.link strong CAS: rewrite pred.next, preserving
  // pred's state. Any concurrent mutation to pred.link (state
  // transition, walker mark/finalize, opp-splice on pred's predecessor)
  // fails the CAS — caller restarts.
  LIBC_INLINE bool try_splice_at_pred(uint16_t pred,
                                        linkage::Link expected_pred_link,
                                        uint16_t new_next) {
    // with_next_uncertify drops MARK/CERT in desired; the strong CAS
    // validates expected as the full word, so any non-zero MARK/CERT
    // in actual would have failed it anyway — preservation would be
    // a no-op in the only legal call pattern.
    linkage::Link desired = expected_pred_link.with_next_uncertify(new_next);
    auto &ps = wait_slot::get_slot(pred);
    return ps.link.compare_exchange_strong(expected_pred_link, desired,
                                            cpp::MemoryOrder::ACQ_REL,
                                            cpp::MemoryOrder::ACQUIRE);
  }

  // Dead-prev mid-splice. Distinct from try_splice_at_pred: no
  // captured snap of gp.link, so re-loads gp.link fresh and requires
  // it to still point at prev with a live state and unmarked.
  LIBC_INLINE bool try_splice_dead_prev_mid(uint16_t gp, uint16_t prev,
                                              uint16_t p_next) {
    auto &gs = wait_slot::get_slot(gp);
    linkage::Link expected_link = gs.link.load(cpp::MemoryOrder::ACQUIRE);
    uint8_t gs_state = expected_link.state();
    if (expected_link.next() != prev || expected_link.is_marked() ||
        (gs_state != wait_slot::WAITING &&
         gs_state != wait_slot::IN_KERNEL))
      return false;
    return gs.link.compare_exchange_strong(
        expected_link, expected_link.with_next_uncertify(p_next),
        cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE);
  }

  // One head-anchored walk attempt. Caller (harris_unlink) loops
  // until a non-Retry result; each attempt does only walk + race
  // dispatch.
  LIBC_INLINE WalkResult
  harris_walk_attempt(uint16_t target, uint32_t target_gen_entry,
                       wait_slot::WaitSlot &target_slot) {
    // If target was reclaimed between caller's bind and now, we're
    // done — someone else already unlinked it.
    if (target_slot.generation.load(cpp::MemoryOrder::ACQUIRE) !=
        target_gen_entry)
      return WalkResult::NotFound;

    uint16_t gp = wait_slot::NULL_INDEX;   // grandparent (prev of prev)
    uint16_t prev = wait_slot::NULL_INDEX; // virtual head sentinel
    uint16_t curr = stack_top(
        combined_stack(combined_.load(cpp::MemoryOrder::ACQUIRE)));

    while (curr != wait_slot::NULL_INDEX) {
        // One atomic re-read of prev.link serves three purposes:
        //   (1) reachability check prev.next == curr,
        //   (2) expected-value for the mid-splice CAS (closes the
        //       "prev popped, CAS succeeds on detached slot" race),
        //   (3) prev.state dispatch — live (WAITING/IN_KERNEL) or
        //       dead (TIMED_OUT / SIGNALED_* → dead-prev splice).
        uint16_t observed;
        linkage::Link prev_link;
        if (prev == wait_slot::NULL_INDEX) {
          observed = stack_top(
              combined_stack(combined_.load(cpp::MemoryOrder::ACQUIRE)));
        } else {
          prev_link = wait_slot::get_slot(prev).link.load(
              cpp::MemoryOrder::ACQUIRE);
          // Marked: another walker is mid-splicing prev. Rather than
          // wait for the marker to finalize (which deadlocks the
          // walk if the marker died between parent CAS and finalize),
          // help complete the splice ourselves. Strong CAS at both
          // the parent rewrite and the finalize serializes the
          // original-marker vs helper races: at most one winner per
          // CAS, losers bail harmlessly.
          if (prev_link.is_marked()) {
            // Finalize ONLY on our own parent-CAS success — that is
            // the proof of off-chain. On parent-CAS failure we don't
            // know if the splice succeeded (marker did it / will do
            // it) or if the chain shape no longer admits the splice
            // (e.g. parent transitioned dead between mark and help):
            // clear MARK to release the freeze so the next walk
            // iteration can re-evaluate from scratch. link_clear_mark
            // is idempotent and exits early on MARK=0 observed
            // (covers the marker-already-finalized branch).
            uint16_t p_next = prev_link.next();
            bool spliced =
                (gp == wait_slot::NULL_INDEX)
                    ? try_splice_head(prev, p_next)
                    : try_splice_dead_prev_mid(gp, prev, p_next);
            if (spliced) {
              linkage::link_finalize_after_splice(
                  wait_slot::get_slot(prev).link,
                  linkage::MarkedLinkSnap::from_observed_marked(prev_link));
            } else {
              linkage::link_clear_mark(wait_slot::get_slot(prev).link);
            }
            return WalkResult::Retry;
          }
          observed = prev_link.next();
          uint8_t prev_state = prev_link.state();

          // IDLE on a reachable chain = stale-snapshot from a stolen-
          // list reclaim (see walker race table above). Retry, NEVER
          // assert.
          if (prev_state == wait_slot::IDLE)
            return WalkResult::Retry;

          // Dead-prev splice (TIMED_OUT or SIGNALED_*). Splice via
          // gp.link (or combined_ if prev was head); resume with
          // prev := gp, curr unchanged.
          if (prev_state == wait_slot::TIMED_OUT ||
              wait_slot::state_is_signaled(prev_state)) {
            bool p_timed_out = (prev_state == wait_slot::TIMED_OUT);

            // Mark prev.link to freeze prev.next while we CAS the
            // parent — closes the stale-p_next race where a
            // concurrent op-splice of prev's successor advances
            // prev.next between our capture and the parent CAS.
            auto &ps = wait_slot::get_slot(prev);
            if (!linkage::link_cas_set_mark(ps.link, prev_link))
              return WalkResult::Retry;
            // MarkedLinkSnap is typed — only link_pack_after_mark
            // produces one — so the finalize call's "input must be
            // marked" precondition is checked at the type level.
            // Required so the strong-CAS bail-on-fail in finalize
            // can't certify a re-pushed slot in a new lifecycle.
            linkage::MarkedLinkSnap prev_marked_snap =
                linkage::link_pack_after_mark(prev_link);
            // p_next is stable post-mark: only link_clear_mark or
            // clear_slot_owned can modify prev.link's next, and both
            // are post-off-chain. Waker state transitions preserve
            // both mark and next.
            uint16_t p_next = prev_link.next();

            if (!(gp == wait_slot::NULL_INDEX
                      ? try_splice_head(prev, p_next)
                      : try_splice_dead_prev_mid(gp, prev, p_next))) {
              // Release the mark so other walkers can make progress.
              linkage::link_clear_mark(ps.link);
              return WalkResult::Retry;
            }
            // Splice ok: atomically publish CERT and clear MARK via
            // single strong CAS bail-on-fail (any racing writer takes
            // over the terminal CERT publish; retrying could certify
            // a re-pushed slot in a new lifecycle).
            linkage::link_finalize_after_splice(
                wait_slot::get_slot(prev).link, prev_marked_snap);
            // Reclaim only TIMED_OUT — SIGNALED is owner-managed.
            if (p_timed_out)
              wait_slot::reclaim_slot(
                  wait_slot::ReclaimAuthority::after_walker_splice(prev));
            // Rewind: gp's own predecessor isn't tracked, so drop gp
            // to NULL. Bounded — dead-set shrinks by one per splice.
            prev = gp;
            gp = wait_slot::NULL_INDEX;
            continue;
          }
          // prev live (WAITING / IN_KERNEL).
        }
        if (observed != curr)
          return WalkResult::Retry;

        // One load covers curr's next AND state (folded in link).
        auto &cs = wait_slot::get_slot(curr);
        linkage::Link c_link = cs.link.load(cpp::MemoryOrder::ACQUIRE);
        // Marked: another walker mid-splice on curr. Help complete
        // (mark-then-help; see LINK_MARK_BIT doc in wait_slot.h).
        // Parent for curr is `prev` (or head if prev == NULL).
        // Same parent-CAS-success-gates-finalize discipline as the
        // prev-marked branch above.
        if (c_link.is_marked()) {
          uint16_t c_next_marked = c_link.next();
          bool spliced =
              (prev == wait_slot::NULL_INDEX)
                  ? try_splice_head(curr, c_next_marked)
                  : try_splice_at_pred(prev, prev_link, c_next_marked);
          if (spliced) {
            linkage::link_finalize_after_splice(
                cs.link,
                linkage::MarkedLinkSnap::from_observed_marked(c_link));
          } else {
            linkage::link_clear_mark(cs.link);
          }
          return WalkResult::Retry;
        }
        uint16_t c_next = c_link.next();
        uint8_t c_state = c_link.state();
        // IDLE on chain = stale snapshot from stolen-list reclaim;
        // retry, NEVER assert.
        if (c_state == wait_slot::IDLE)
          return WalkResult::Retry;
        bool c_timed_out = (c_state == wait_slot::TIMED_OUT);
        bool c_orphan = wait_slot::state_is_signaled(c_state);
        bool c_dead = c_timed_out || c_orphan;

        if (curr == target) {
          // Target reclaimed by another actor between bind and now.
          if (target_slot.generation.load(cpp::MemoryOrder::ACQUIRE) !=
              target_gen_entry)
            return WalkResult::NotFound;

          // Mark target.link to freeze target.next during parent CAS
          // (closes the stale-c_next race).
          if (!linkage::link_cas_set_mark(cs.link, c_link))
            return WalkResult::Retry;
          linkage::MarkedLinkSnap target_marked_snap =
              linkage::link_pack_after_mark(c_link);
          // Mid-splice CAS uses prev_link as expected; any concurrent
          // mutation to prev fails on tag mismatch.
          if (prev == wait_slot::NULL_INDEX
                  ? try_splice_head(target, c_next)
                  : try_splice_at_pred(prev, prev_link, c_next)) {
            linkage::link_finalize_after_splice(cs.link,
                                                   target_marked_snap);
            // Only TIMED_OUT is walker-reclaimable (owner moved on);
            // SIGNALED_* stays owner-managed.
            return c_timed_out ? WalkResult::SplicedReclaim
                                : WalkResult::SplicedNoReclaim;
          }
          linkage::link_clear_mark(cs.link);
          return WalkResult::Retry;
        }

        // Opp-splice dead intermediates — bounds ghost lifetime by
        // folding cleanup into every walker. Self-reclaim is safe:
        // reclaim_slot direct-pushes to freelist; the state+tag fold
        // ensures any later walker that observes the freelist-pushed
        // slot either trips the IDLE-on-chain retry or fails its
        // mid-splice CAS via tag mismatch.
        if (c_dead) {
          if (!linkage::link_cas_set_mark(cs.link, c_link))
            return WalkResult::Retry;
          linkage::MarkedLinkSnap opp_marked_snap =
              linkage::link_pack_after_mark(c_link);
          if (prev == wait_slot::NULL_INDEX
                  ? try_splice_head(curr, c_next)
                  : try_splice_at_pred(prev, prev_link, c_next)) {
            linkage::link_finalize_after_splice(cs.link, opp_marked_snap);
            if (c_timed_out)
              wait_slot::reclaim_slot(
                  wait_slot::ReclaimAuthority::after_walker_splice(curr));
            curr = c_next;
            continue;
          }
          linkage::link_clear_mark(cs.link);
          return WalkResult::Retry;
        }

        // Advance. gp = old prev so the next iteration can splice prev
        // via gp.link if prev transitions dead. No gp_link snap — the
        // dead-prev splice re-reads gs.link fresh.
        gp = prev;
        prev = curr;
        curr = c_next;
      }

      // Walked to NULL — target removed by another actor or never on
      // chain in this attempt's snapshot.
      return WalkResult::NotFound;
    }

public:
  // Self-splice after IN_KERNEL → TIMED_OUT. Bounds ghost
  // accumulation — timed-out slots leave the stack before wait
  // returns, so wakers never touch slots whose owner moved on.
  // slot.generation is stable across state transitions (only
  // link.tag moves), so my_gen captured here identifies "our"
  // allocation; concurrent reclaim bumps gen, harris_unlink returns
  // false — no double reclaim.
  // Skip clear_tls_slot when nested — only primaries have a TLS ref.
  LIBC_INLINE void inline_unlink_timed_out_slot(uint32_t my_idx,
                                                 bool nested) {
    auto &slot = wait_slot::get_slot(my_idx);
    uint32_t my_gen = slot.generation.load(cpp::MemoryOrder::ACQUIRE);
    if (harris_unlink(static_cast<uint16_t>(my_idx), my_gen))
      wait_slot::reclaim_slot(
          wait_slot::ReclaimAuthority::after_walker_splice(my_idx));
    if (!nested)
      wait_slot::clear_tls_slot();
  }

  // Lock-free: any waiters in the embedded stack?
  LIBC_INLINE bool has_waiters() const {
    uint32_t s = const_cast<Futex *>(this)->stack_.load(
        cpp::MemoryOrder::ACQUIRE);
    return stack_top(s) != STACK_NULL;
  }

  // Drain an in-flight wake alert on a losing cancel / unexpected
  // wake. The waker's pre-mark committed an unconditional alert.
  // Drain miss (waker preempted pre-syscall or killed post-publish)
  // sets expect_late_alert so the next STATUS_ALERTED + IN_KERNEL
  // classifies stale — closes the residual spurious-EINTR window.
  LIBC_INLINE void drain_waker_alert(PVOID tid_ptr) {
    static constexpr LARGE_INTEGER DRAIN_TIMEOUT = {.QuadPart = -1};
    NTSTATUS drain_status =
        ::NtWaitForAlertByThreadId(tid_ptr, &DRAIN_TIMEOUT);
    if (drain_status != STATUS_ALERTED)
      wait_slot::mark_expect_late_alert();
  }

  // Decode SIGNALED_* state to waiter return: HANDOFF ⇒ 1, else 0.
  // Non-SIGNALED returns 0 (callers that pre-read state).
  LIBC_INLINE static long decode_signaled_ret(uint8_t st) {
    return wait_slot::state_has_handoff(st) ? 1 : 0;
  }

  // Self-splice on the orphan wake path. Returns with slot.link
  // provably off-chain AND CERT=1 published.
  //
  // Structural bound on termination: harris_unlink uses mark-then-
  // help (see LINK_MARK_BIT doc in wait_slot.h), so every observed
  // MARK=1 either belongs to a live walker we race harmlessly, or
  // gets help-completed by us. No spin-on-external-thread-liveness
  // remains in the protocol. On return from harris_unlink, target
  // is either:
  //   (a) spliced by us (self.link CERT published by our finalize), or
  //   (b) spliced by a concurrent walker / waker (their commit path
  //       publishes CERT), or
  //   (c) walked-to-NULL — already off-chain when we entered, no
  //       publisher is committed because none was needed.
  //
  // Owner publishes CERT unconditionally after harris_unlink returns:
  // case (a)/(b) make this a redundant idempotent fetch_or; case (c)
  // is the structural close that would otherwise hang waiting on a
  // publisher that never existed (e.g., walker died post-detach,
  // pre-finalize).
  //
  // Owner authority for the CERT publish: in SIGNALED_*_ORPHAN, only
  // the owner can transition the slot back to a chain-resident state
  // (Phase 2 push requires owner-exclusive IDLE → WAITING transition
  // through clear_slot_owned). Therefore once off-chain, the slot
  // stays off-chain until owner reuses it — owner is the canonical
  // off-chain witness.
  LIBC_INLINE void
  self_splice_if_orphan(wait_slot::WaitSlot &slot, uint32_t my_idx) {
    linkage::Link snap = slot.link.load(cpp::MemoryOrder::ACQUIRE);
    if (!wait_slot::state_is_orphan(snap.state()) || snap.is_certified())
      return;

    // harris_unlink (with mark-then-help on observed MARK) always
    // terminates. Return value ignored: we proceed identically on
    // splice-success, splice-by-other, and walked-to-NULL — all
    // three imply off-chain, and the unconditional CERT publish
    // below closes any case where no publisher was committed.
    uint32_t my_gen = slot.generation.load(cpp::MemoryOrder::ACQUIRE);
    (void)harris_unlink(static_cast<uint16_t>(my_idx), my_gen);

    // Idempotent CERT publish via fetch_or. Preserves all other
    // fields (state, next, MARK, ALERT_FIRED), no tag bump (CERT is
    // bit-set semantics, not a state transition observers respond
    // to). If a walker / waker / our own finalize already published
    // CERT, this is a benign repeat-OR. If MARK=1 still set (e.g.,
    // walker died post-parent-CAS, pre-finalize, AND no helper has
    // run yet), MARK persists — clear_slot_owned's slow path takes
    // care of clearing it when writing IDLE+CERT.
    linkage::link_field_set_cert(slot.link, cpp::MemoryOrder::RELEASE);
  }

  // Drain a stale top slot (slot.wait_address != this). Shared by
  // pop_and_signal_one and handoff_one; always resolves or advances
  // top; caller `continue`s.
  //
  // Waker-side splice fallback (harris_unlink on detach loss) is
  // load-bearing here, UNLIKE the non-stale path — a stale waiter's
  // self_splice_if_orphan targets the OTHER futex (encoded in its
  // pre-cleared wait_address) and cannot remove itself from our stack.
  LIBC_INLINE void drain_stale_top(wait_slot::WaitSlot &slot,
                                    linkage::Link slot_snap, uint16_t top,
                                    uint8_t st, uint32_t old_stk,
                                    uint32_t new_stk) {
    if (st == wait_slot::WAITING || st == wait_slot::IN_KERNEL) {
      // Pre-mark ORPHAN — single-atomic wake publish.
      if (!linkage::link_cas_snap<wait_slot::SIGNALED_ORPHAN, wait_slot::WaitSlotStateTraits>(
              slot.link, slot_snap))
        return; // pre-mark raced; caller retries with fresh snap.
      // Invalidate wait_address so the waiter's post-wake check
      // trips; subsystem→None for thread-exit dispatch.
      slot.wait_address.store(0, cpp::MemoryOrder::RELAXED);
      slot.subsystem.store(wait_slot::SubsystemKind::None,
                            cpp::MemoryOrder::RELAXED);
      // I4: capture tid AND owner_ref together, post-pre-mark and
      // pre-detach, via the typed token. The unconditional alert
      // below is the only consumer.
      WakeCommitToken token = capture_wake_target(slot);
      bool detached = stack_.compare_exchange_strong(
          old_stk, new_stk, cpp::MemoryOrder::ACQ_REL,
          cpp::MemoryOrder::RELAXED);
      if (!detached) {
        uint32_t slot_gen =
            slot.generation.load(cpp::MemoryOrder::ACQUIRE);
        // harris_unlink walks the chain with mark-then-help on
        // observed MARK; splice success publishes CERT atomically.
        (void)harris_unlink(top, slot_gen);
      }
      // Terminal certify CAS, idempotent on CERT and best-effort on
      // state. By harris_unlink's correctness, "neither detached
      // nor unlinked" ⇒ another actor detached and certified us;
      // their atomic publish makes the state-mismatch failure here
      // harmless.
      (void)linkage::link_cas_state_certify<
          wait_slot::SIGNALED_ORPHAN, wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(
          slot.link);
      // Pre-mark committed the wake unconditionally; WAITING catches
      // via Phase 2.5 cache spin, IN_KERNEL needs the syscall.
      if (st == wait_slot::IN_KERNEL)
        wait_slot::alert_one_if_live(token.owner_packed(), token.tid(), slot);
      return;
    }
    if (st == wait_slot::TIMED_OUT) {
      // Dead-and-stale: safe to reclaim (no live TLS ref).
      if (stack_.compare_exchange_strong(
              old_stk, new_stk, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::RELAXED))
        wait_slot::reclaim_slot(
            wait_slot::ReclaimAuthority::after_stack_pop(top));
      return;
    }
    if (wait_slot::state_is_signaled(st)) {
      // Orphan from a mid-flight popper — atomic detach + certify.
      (void)help_detach_signaled(slot, st, old_stk, new_stk);
      return;
    }
    // IDLE / unknown — caller's `continue` reloads fresh.
  }

  // Self-cancel Phase-4 via link_cas IN_KERNEL → TIMED_OUT.
  //   Cancel wins: slot unlinked, return cancel_ret (caller's reason:
  //                -ETIMEDOUT, -EINTR, or 0 for spurious-pred-fail).
  //   Waker wins:  pre-mark raced; read state, decode (0/1 for
  //                handoff), drain the unconditional alert.
  LIBC_INLINE long cancel_or_absorb(wait_slot::WaitSlot &slot,
                                     uint32_t my_idx, bool nested,
                                     PVOID tid_ptr, long cancel_ret) {
    if (linkage::link_cas_state<wait_slot::IN_KERNEL,
                                    wait_slot::TIMED_OUT, wait_slot::WaitSlotStateTraits>(slot.link)) {
      inline_unlink_timed_out_slot(my_idx, nested);
      return cancel_ret;
    }
    // Waker raced — link_cas_snap(IN_KERNEL → SIGNALED_*) won.
    uint8_t st = linkage::link_load_state(slot.link);
    drain_waker_alert(tid_ptr);
    return decode_signaled_ret(st);
  }

  // ===== In-kernel re-park (HasPredicate, CLEAN-only) =====
  //
  // Phase 4 thundering-herd absorption: waker published CLEAN but
  // pred is still FALSE (winner flipped value_; losers don't
  // satisfy). Re-install the already-owned slot in place rather than
  // paying Phase 0-3 re-entry:
  //   (1) link.state → WAITING (tag bump, next preserved).
  //   (2) re-publish subsystem=Futex (waker cleared it).
  //       wait_address / filter_fn survive from Phase 2 install.
  //   (3) CAS-64 push; bail Satisfied on pred flip.
  //   (4) Phase 2.5 cache spin + Phase 3 CAS → IN_KERNEL.
  //
  // CLEAN only because CLEAN ⇒ waker's detach CAS committed ⇒ slot
  // off-chain. ORPHAN bails to top-level absorb in
  // handle_pred_signaled_wake (re-pushing a still-linked slot would
  // cycle the chain). Tag monotonicity across (1)-(5) kills any
  // stale-snap publish from a prior wake wave.
  enum class ReparkOutcome : uint8_t {
    Installed,   // slot IN_KERNEL on the stack; resume Phase 4 wait.
    Satisfied,   // cond.satisfied in CAS-push; caller clears + 0.
    Signaled,    // waker raced Phase 2.5/3; caller decodes.
    Invalidated, // slot.wait_address == 0; caller returns -EINVAL.
  };

  // Tagged union for inkernel_repark. signaled_ret() is meaningful
  // only when kind() == Signaled; named factories enforce that
  // construction commits to a (kind, payload) pair, eliminating the
  // earlier out-param's stale-read footgun.
  class ReparkResult {
  public:
    LIBC_INLINE constexpr ReparkOutcome kind() const { return kind_; }

    // Meaningful only when kind() == Signaled; deterministic 0 on
    // other arms. Switch on kind() first.
    LIBC_INLINE constexpr long signaled_ret() const { return signaled_ret_; }

    LIBC_INLINE static constexpr ReparkResult installed() {
      return ReparkResult{ReparkOutcome::Installed, 0};
    }
    LIBC_INLINE static constexpr ReparkResult satisfied() {
      return ReparkResult{ReparkOutcome::Satisfied, 0};
    }
    LIBC_INLINE static constexpr ReparkResult signaled(long ret) {
      return ReparkResult{ReparkOutcome::Signaled, ret};
    }
    LIBC_INLINE static constexpr ReparkResult invalidated() {
      return ReparkResult{ReparkOutcome::Invalidated, 0};
    }

  private:
    LIBC_INLINE constexpr ReparkResult(ReparkOutcome k, long r)
        : kind_(k), signaled_ret_(r) {}
    ReparkOutcome kind_;
    long signaled_ret_;
  };

  LIBC_INLINE ReparkResult
  inkernel_repark(wait_slot::WaitSlot &slot, uint32_t my_idx,
                  WaitCondition cond, FutexValueType caller_unsat_val) {
    // Invalidation gate first — futex was destroyed between Phase-3
    // commit and this wake.
    if (slot.wait_address.load(cpp::MemoryOrder::RELAXED) == 0)
      return ReparkResult::invalidated();

    // Caller gates on CLEAN-only ⇒ off-chain. Tag bump on WAITING
    // re-entry; next is preserved (push CAS overwrites at commit).
    //
    // Owner-exclusive optimization: load slot.link once and thread
    // the local Link through state set + push loop. See Phase 2 push
    // for the same pattern.
    linkage::Link slot_link = linkage::link_store_state_known(
        slot.link, slot.link.load(cpp::MemoryOrder::RELAXED),
        wait_slot::WAITING);
    // Waker cleared subsystem on pre-mark; re-publish for thread-
    // exit cleanup / stale-reclaim dispatch on a future cancel.
    slot.subsystem.store(wait_slot::SubsystemKind::Futex,
                         cpp::MemoryOrder::RELAXED);

    // CAS-64 push — same shape as Phase 2 (bail re-publishes CERT;
    // push uses _uncertify).
    //
    // First-iteration satisfied() elision: caller_unsat_val is the
    // value handle_pred_signaled_wake just evaluated UNSAT. Pure
    // predicate ⇒ same value ⇒ same UNSAT, so skip the indirect call
    // on iteration 1 only. Value-change or CAS-retry re-evaluates.
    bool first = true;
    for (;;) {
      uint64_t old = combined_.load(cpp::MemoryOrder::RELAXED);
      uint32_t val = combined_value(old);
      bool pred_sat;
      if (first && val == caller_unsat_val) {
        pred_sat = false;
      } else {
        pred_sat = cond.satisfied(val);
      }
      first = false;
      if (pred_sat) {
        // Bail without push — re-publish CERT after the prior
        // _uncertify write so caller's clear_slot_owned passes.
        linkage::link_field_set_cert(slot.link,
                                        cpp::MemoryOrder::RELAXED);
        return ReparkResult::satisfied();
      }
      uint32_t old_stk = combined_stack(old);
      slot_link = linkage::link_store_next_uncertify_known(
          slot.link, slot_link, stack_top(old_stk));
      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1,
                                     static_cast<uint16_t>(my_idx));
      uint64_t desired = combined_pack(val, new_stk);
      if (combined_.compare_exchange_weak(old, desired,
                                           cpp::MemoryOrder::ACQ_REL,
                                           cpp::MemoryOrder::RELAXED))
        break;
    }

    // Phase 2.5 cache spin. Pre-mark snap was WAITING here
    // (Phase 2.5 spinners are in WAITING state) → ALERT_FIRED is
    // 0 by structural invariant; the bit check is a no-op but
    // kept uniform with the rule shared by every SIGNALED-
    // observation site.
    if (spin_wait::spin_on_link_state(&slot.link, wait_slot::WAITING)) {
      linkage::Link snap = slot.link.load(cpp::MemoryOrder::ACQUIRE);
      uint8_t st = snap.state();
      if (wait_slot::state_is_signaled(st)) {
        if (snap.is_alert_fired())
          wait_slot::mark_expect_late_alert();
        long decoded_ret = decode_signaled_ret(st);
        return (slot.wait_address.load(cpp::MemoryOrder::RELAXED) == 0)
                 ? ReparkResult::invalidated()
                                         : ReparkResult::signaled(decoded_ret);
      }
    }

    // Phase 3 CAS WAITING → IN_KERNEL. CAS-fail snap state is
    // SIGNALED via WAITING-pre-mark (CAS expected WAITING) →
    // ALERT_FIRED 0 by invariant; no-op on the bit but the rule
    // stays uniform.
    if (!linkage::link_cas_state<wait_slot::WAITING,
                                     wait_slot::IN_KERNEL, wait_slot::WaitSlotStateTraits>(slot.link)) {
      linkage::Link snap = slot.link.load(cpp::MemoryOrder::ACQUIRE);
      uint8_t st = snap.state();
      if (wait_slot::state_is_signaled(st) && snap.is_alert_fired())
        wait_slot::mark_expect_late_alert();
      long decoded_ret = decode_signaled_ret(st);
      return (slot.wait_address.load(cpp::MemoryOrder::RELAXED) == 0)
                 ? ReparkResult::invalidated()
                                       : ReparkResult::signaled(decoded_ret);
    }

    return ReparkResult::installed();
  }

  // Shared Phase-4 dispatcher for "SIGNALED_* observed + predicate
  // path". HasPredicate=true only; classic path inlines
  // `ret = signaled_ret; break`.
  // Contract: caller observed state_is_signaled(signaled_state) on
  // slot.link and decoded signaled_ret. Post-wake callers MUST have
  // drained any pending waker alert first.
  //   Continue   — re-installed IN_KERNEL; caller `continue`s.
  //   ReturnZero — Satisfied during re-push; slot cleared.
  //   Break      — break Phase 4 loop with ret = break_ret(). Covers
  //                HANDOFF, pred-satisfied at entry, invalidation,
  //                ORPHAN wake, post-install timeout, racing wake
  //                during re-install.
  enum class PhaseFourAction : uint8_t {
    Break,
    Continue,
    ReturnZero,
  };

  // Tagged union — same rationale as ReparkResult; break_ret() is
  // meaningful only when action() == Break.
  class PhaseFourActionResult {
  public:
    LIBC_INLINE constexpr PhaseFourAction action() const { return action_; }

    // Meaningful only when action() == Break; deterministic 0 on
    // Continue/ReturnZero (don't read).
    LIBC_INLINE constexpr long break_ret() const { return break_ret_; }

    LIBC_INLINE static constexpr PhaseFourActionResult continue_loop() {
      return PhaseFourActionResult{PhaseFourAction::Continue, 0};
    }
    LIBC_INLINE static constexpr PhaseFourActionResult return_zero() {
      return PhaseFourActionResult{PhaseFourAction::ReturnZero, 0};
    }
    LIBC_INLINE static constexpr PhaseFourActionResult break_with(long ret) {
      return PhaseFourActionResult{PhaseFourAction::Break, ret};
    }

  private:
    LIBC_INLINE constexpr PhaseFourActionResult(PhaseFourAction a, long r)
        : action_(a), break_ret_(r) {}
    PhaseFourAction action_;
    long break_ret_;
  };

  LIBC_INLINE PhaseFourActionResult
  handle_pred_signaled_wake(wait_slot::WaitSlot &slot, uint32_t my_idx,
                             bool nested, WaitCondition cond,
                             uint8_t signaled_state, long signaled_ret,
                             cpp::optional<Timeout> timeout,
                             LARGE_INTEGER &nt_timeout,
                             LARGE_INTEGER *&nt_timeout_ptr,
                             PVOID tid_ptr) {
    bool invalidated =
        (slot.wait_address.load(cpp::MemoryOrder::RELAXED) == 0);
    bool orphan = wait_slot::state_is_orphan(signaled_state);
    // Re-park requires ALL: non-HANDOFF wake, not invalidated, CLEAN
    // (not ORPHAN), pred still false. ORPHAN bails because the slot
    // is still linked on-chain (waker's detach lost) — re-pushing
    // would cycle the chain; let the loop epilogue's
    // self_splice_if_orphan publish CERT and top-level absorb
    // re-enter Phase 0-3 fresh.
    // cur_val passes to inkernel_repark for first-iteration
    // satisfied() elision (pure pred + same value ⇒ same UNSAT).
    FutexValueType cur_val = value_.load(cpp::MemoryOrder::ACQUIRE);
    if (signaled_ret != 0 || invalidated || orphan ||
        cond.satisfied(cur_val)) {
      return PhaseFourActionResult::break_with(
          invalidated ? -EINVAL : signaled_ret);
    }

    ReparkResult rr = inkernel_repark(slot, my_idx, cond, cur_val);
    switch (rr.kind()) {
    case ReparkOutcome::Installed: {
      nt_timeout_ptr = timeout_to_nt(timeout, nt_timeout);
      if (nt_timeout_ptr && nt_timeout_ptr->QuadPart == -1)
        return PhaseFourActionResult::break_with(
            cancel_or_absorb(slot, my_idx, nested, tid_ptr, -ETIMEDOUT));
      return PhaseFourActionResult::continue_loop();
    }
    case ReparkOutcome::Satisfied:
      clear_slot_owned(slot, my_idx);
      return PhaseFourActionResult::return_zero();
    case ReparkOutcome::Invalidated:
      return PhaseFourActionResult::break_with(-EINVAL);
    case ReparkOutcome::Signaled:
      // Fall to normal epilogue (self_splice_if_orphan +
      // clear_slot_owned at Phase 4 loop exit).
      return PhaseFourActionResult::break_with(rr.signaled_ret());
    }
    // Unreachable; -Wswitch-enum catches additions. Defensive only.
    return PhaseFourActionResult::break_with(0);
  }

  // --- Timeout conversion ---

  LIBC_INLINE LARGE_INTEGER *
  timeout_to_nt(cpp::optional<Timeout> timeout, LARGE_INTEGER &storage) {
    if (!timeout)
      return nullptr;

    constexpr long long EPOCH_DIFF_HNS = 116444736000000000LL;
    const timespec &ts = timeout->get_timespec();
    long long target_hns =
        static_cast<long long>(ts.tv_sec) * 10000000LL + ts.tv_nsec / 100;

    if (timeout->is_realtime()) {
      storage.QuadPart = target_hns + EPOCH_DIFF_HNS;
      return &storage;
    }

    ULONGLONG now_hns;
    ::RtlQueryUnbiasedInterruptTime(&now_hns);
    long long relative_hns = target_hns - static_cast<long long>(now_hns);
    if (relative_hns <= 0) {
      storage.QuadPart = -1;
      return &storage;
    }
    storage.QuadPart = -relative_hns;
    return &storage;
  }

  // ===== Wait =====
  //
  // Phase map — fall through on "still need to park", exit early on
  // "done":
  //
  //   Phase 1    HW-monitored value spin (UMWAIT/MWAITX)
  //   Phase 1.5  timeout already expired?
  //   Phase 1.75 owner-side stale-slot reclaim (TIMED_OUT TLS slot)
  //   Phase 2    CAS-64 push onto Treiber stack (atomic value+push)
  //   Phase 2.5  pre-kernel cache spin on slot.link state byte
  //   Phase 3    CAS state WAITING → IN_KERNEL
  //   Phase 4    NtWait + wake/timeout/stale-alert/signal loop
  //
  // Early-exit returns:
  //   Phase 1     value changed                       0
  //   Phase 1.5   timeout expired                     -ETIMEDOUT
  //   Phase 2     pool exhausted                      -ENOMEM
  //   Phase 2     CAS-64 cond.satisfied(val)          0
  //   Phase 2.5   SIGNALED observed                   HANDOFF-decoded
  //   Phase 3     CAS-fail self-commit ORPHAN         HANDOFF-decoded
  //   Phase 4     SIGNALED observed                   HANDOFF-decoded
  //   Phase 4     STATUS_TIMEOUT                      cancel_or_absorb(-ETIMEDOUT)
  //   Phase 4     stale + pred moved off              cancel_or_absorb(0)
  //   Phase 4     stale + timeout expired             cancel_or_absorb(-ETIMEDOUT)
  //   Phase 4     signal [Interruptible]              cancel_or_absorb(-EINTR)
  //   Phase 4     signal [!Interruptible] + timeout   cancel_or_absorb(-ETIMEDOUT)
  //   Phase 4     signal [!Interruptible]             reloop
  //
  // cancel_or_absorb decodes HANDOFF on waker race, so a late wake
  // that lost the cancel CAS still delivers its handoff.

  // VEH-nested release: secondary slots that landed back in IDLE by
  // return go to the freelist here. Skipped on -ETIMEDOUT/-EINTR
  // because those paths leave the slot TIMED_OUT for self-reclaim.
  LIBC_INLINE static long wait_epilogue(bool nested, uint32_t my_idx,
                                        long ret) {
    if (nested && ret != -ETIMEDOUT && ret != -EINTR &&
        linkage::link_load_state(
            wait_slot::get_slot(my_idx).link,
            cpp::MemoryOrder::RELAXED) == wait_slot::IDLE)
      wait_slot::release_secondary(my_idx);
    return ret;
  }

  // Interruptible: APC/signal wakes return -EINTR; default absorbs
  // them as spurious. `if constexpr` branches eliminate from the
  // default path with zero codegen impact.
  template <bool Interruptible = false>
  LIBC_INLINE long wait(FutexValueType expected,
                        cpp::optional<Timeout> timeout = cpp::nullopt,
                        bool is_shared = false) {
    return wait_impl<Interruptible, /*HasPredicate=*/false>(
        WaitCondition::eq(expected), timeout, is_shared);
  }

  // Predicate-based stop condition: pred(v, arg) == TRUE exits wait.
  // Same fn doubles as waker-side filter — pop/walker skip waiters
  // whose pred is FALSE on current value_.
  //
  //   Return guarantee: ret == 0 ⇒ pred held at some point during
  //   the wait (Phase 4 re-park + top-level absorb never return 0
  //   on pred-FALSE spurious wake).
  //
  //   Thundering-herd: Phase 1 hw-spin re-arms on non-pred-flipping
  //   writes; filter suppresses wasted alerts; in-kernel re-park
  //   keeps losers out of Phase-0 round-trip.
  //
  // Caller contract (LOAD-BEARING): every FALSE→TRUE flip of
  // pred(value_, arg) MUST be paired with notify_one / notify_all /
  // unlock_notify. No "eventual re-check" fallback — violating it
  // strands parked waiters. Same contract as classical wait() (pred
  // = `v != expected`), generalised.
  //
  //   e.g. bitset wait — pred = `(v & mask) != 0`; setter does
  //        fetch_or(bits); notify_one().
  //   e.g. robust mutex — pred = `(v & TID_MASK) == 0 || (v &
  //        OWNER_DIED) != 0`; unlock stores TID=0 + notify, and
  //        death-detect CAS sets OWNER_DIED + notify.
  //
  // handoff_one and notify_all IGNORE the predicate — HANDOFF_BIT
  // is unconditional transfer; broadcast opted into fan-out.
  template <bool Interruptible = false>
  LIBC_INLINE long
  wait_on_predicate(wait_slot::PredicateFn pred, uint32_t arg,
                    cpp::optional<Timeout> timeout = cpp::nullopt,
                    bool is_shared = false) {
    return wait_impl<Interruptible, /*HasPredicate=*/true>(
        WaitCondition::on_predicate(pred, arg), timeout, is_shared);
  }

private:
  // Unified body for wait() and wait_on_predicate(). HasPredicate
  // selects classic `v != arg` vs caller-supplied PredicateFn;
  // `if constexpr` pruning keeps classic byte-equivalent to a
  // non-templated version (no fn-pointer indirection, no filter
  // stores, no Phase-4 pred-retry branches).
  //
  // wait_impl wraps wait_one_cycle with a predicate-aware top-level
  // absorb: if wait_one_cycle returned 0 but pred is still FALSE
  // (rare; possible when in-kernel re-park exits via invalidation
  // race), re-enter. Loop compiles out for HasPredicate=false.
  template <bool Interruptible, bool HasPredicate>
  LIBC_INLINE long wait_impl(WaitCondition cond,
                             cpp::optional<Timeout> timeout,
                             bool is_shared) {
    for (;;) {
      long ret =
          wait_one_cycle<Interruptible, HasPredicate>(cond, timeout,
                                                      is_shared);
      if constexpr (HasPredicate) {
        if (ret == 0 &&
            !cond.satisfied(value_.load(cpp::MemoryOrder::ACQUIRE)))
          continue; // spurious wake; re-enter.
      }
      return ret;
    }
  }

  // One full Phase 0-4 cycle. Predicate-path extras (hw-spin re-arm
  // on pred-false, in-kernel re-park on SIGNALED + pred-false) gate
  // under `if constexpr (HasPredicate)`.
  template <bool Interruptible, bool HasPredicate>
  LIBC_INLINE long wait_one_cycle(WaitCondition cond,
                                   cpp::optional<Timeout> timeout,
                                   bool is_shared) {
    (void)is_shared;

    // Capture `observed` so Phase 1's hw-spin watches the specific
    // value we read.
    FutexValueType observed = get_value(cpp::MemoryOrder::RELAXED);
    if (cond.satisfied(observed))
      return 0;

    // Phase 1: hw spin on value_ (UMWAIT/MWAITX when available —
    // better than PAUSE at 16T+ where PAUSE steals SMT slots).
    // Predicate path uses spin_on_pred, which re-arms the hw monitor
    // on writes that don't flip pred — absorbs thundering-herd
    // losers in user space.
    if constexpr (HasPredicate) {
      if (spin_wait::spin_on_pred(&value_, cond.pred, cond.arg))
        return 0;
    } else {
      if (spin_wait::spin_until_changed(&value_, observed))
        return 0;
    }

    // Phase 1.5: timeout already expired?
    if (timeout) {
      LARGE_INTEGER probe;
      LARGE_INTEGER *p = timeout_to_nt(timeout, probe);
      if (p && p->QuadPart == -1) {
        return -ETIMEDOUT;
      }
    }

    // Phase 1.75: stale-slot self-reclaim. A prior timed-out wait
    // may have left our TLS slot TIMED_OUT and still linked. Splice
    // before alloc to bound pool ghost growth.
    //
    // expected_gen (TLS-captured) gates the unlinker: mismatch ⇒
    // another actor reclaimed (possibly realloc'd to a live waiter)
    // — MUST NOT reclaim here.
    //
    // Subsystem dispatch via typed accessors — each tag-gates the
    // cast behind itself; raw reinterpret_cast<Futex*>(stale_addr)
    // would be a layering bug.
    {
      uintptr_t stale_addr = 0;
      uint32_t expected_gen = 0;
      uint32_t stale_idx =
          wait_slot::get_stale_slot(stale_addr, expected_gen);
      if (stale_idx != wait_slot::NULL_INDEX && stale_addr) {
        auto &stale_slot = wait_slot::get_slot(stale_idx);
        bool should_reclaim = false;
        if (Futex *stale_futex = Futex::try_from_owner_slot(stale_slot)) {
          should_reclaim = stale_futex->harris_unlink(
              static_cast<uint16_t>(stale_idx), expected_gen);
        } else if (void *pl_addr = futex_addr::
                       try_user_addr_from_owner_slot(stale_slot)) {
          should_reclaim = futex_addr::parking_lot_unlink_thread_exit(
              pl_addr, static_cast<uint16_t>(stale_idx), expected_gen);
        }
        // subsystem == None (or unknown): no live chain residency,
        // so should_reclaim stays false (slot is off any chain by
        // construction; clear_tls_slot below releases the TLS ref).
        if (should_reclaim)
          wait_slot::reclaim_slot(
              wait_slot::ReclaimAuthority::after_unlinker_dispatch(stale_idx));
        wait_slot::clear_tls_slot();
      } else if (stale_idx != wait_slot::NULL_INDEX) {
        // stale_addr=0 ⇒ orphan with no owning structure. Treated as
        // trivial-true unlinker dispatch (proof of detach is "no
        // chain to be on").
        wait_slot::reclaim_slot(
            wait_slot::ReclaimAuthority::after_unlinker_dispatch(stale_idx));
        wait_slot::clear_tls_slot();
      }
    }

    // Phase 2: CAS-64 push.
    //
    // Pool exhaustion returns -ENOMEM, not 0 — silent 0 would
    // hot-spin the caller's `while (!cond) wait()` loop. Reachable
    // only at >32K threads × (primary + VEH secondary).
    uint32_t primary_idx = wait_slot::get_slot_index();
    if (primary_idx == wait_slot::NULL_INDEX) {
      return -ENOMEM;
    }
    bool nested =
        linkage::link_load_state(wait_slot::get_slot(primary_idx).link,
                                    cpp::MemoryOrder::RELAXED) !=
        wait_slot::IDLE;
    uint32_t my_idx = nested ? wait_slot::alloc_secondary() : primary_idx;
    if (my_idx == wait_slot::NULL_INDEX) {
      return -ENOMEM;
    }

    auto &slot = wait_slot::get_slot(my_idx);
    // No defensive pre-push harris_unlink: get_slot_index's TLS
    // fast-path gates reuse on CERT=1 (off-chain), and alloc_slot
    // post-pop init sets CERT=1, so slot is provably off-chain.
    // link_store_state preserves CERT through IDLE → WAITING; the
    // push loop's link_store_next_uncertify clears it atomically
    // with the combined_ commit.
    //
    // Owner-exclusive optimization: load slot.link ONCE, then thread
    // the local Link through the IDLE→WAITING set and the push loop.
    // The slot is off-chain across the whole sequence (until combined_
    // CAS publishes), so no concurrent writer can invalidate the local
    // tracking — each per-iteration load was redundant.
    linkage::Link slot_link = linkage::link_store_state_known(
        slot.link, slot.link.load(cpp::MemoryOrder::RELAXED),
        wait_slot::WAITING);
    slot.wait_address.store(reinterpret_cast<uintptr_t>(this),
                              cpp::MemoryOrder::RELAXED);
    slot.subsystem.store(wait_slot::SubsystemKind::Futex,
                         cpp::MemoryOrder::RELAXED);
    // Filter stores are UNCONDITIONAL on both branches:
    // reclaim_slot doesn't clear filter_*, so a stale value from a
    // prior cycle on a reallocated secondary slot would leak.
    // nullptr/0 on the classic path = "wake unconditionally".
    if constexpr (HasPredicate) {
      slot.filter_fn.store(cond.pred, cpp::MemoryOrder::RELAXED);
      slot.filter_arg.store(cond.arg, cpp::MemoryOrder::RELAXED);
    } else {
      slot.filter_fn.store(nullptr, cpp::MemoryOrder::RELAXED);
      slot.filter_arg.store(0, cpp::MemoryOrder::RELAXED);
    }

    // CAS-64 push: atomic value-check + enqueue. The CAS covers the
    // full 64-bit combined_; value change (upper 32) cancels, stack
    // change (lower 32) retries. CAS is the linearization point —
    // no Dekker re-check.
    for (;;) {
      uint64_t old = combined_.load(cpp::MemoryOrder::RELAXED);
      uint32_t val = combined_value(old);
      if (cond.satisfied(val)) {
        // Bail without push (we never committed combined_, slot is
        // off-chain). Re-publish CERT — a prior iteration's
        // _uncertify cleared it — before clear_slot_owned consumes.
        linkage::link_field_set_cert(slot.link,
                                        cpp::MemoryOrder::RELAXED);
        clear_slot_owned(slot, my_idx);
        return wait_epilogue(nested, my_idx, 0);
      }
      uint32_t old_stk = combined_stack(old);
      // Writer-exclusive until CAS publishes. link_store_next_
      // uncertify clears LINK_CERT_BIT — slot is about to commit
      // onto the chain via the combined_ CAS below; on-chain
      // residency reads CERT=0 (truthful).
      //
      // stack_top ≠ my_idx here: get_slot_index's TLS fast-path
      // gates reuse on CERT=1 (off-chain), and alloc_slot's post-
      // pop init sets CERT=1, so the slot is provably off-chain at
      // Phase 2 entry. No defensive pre-push harris_unlink needed.
      //
      // Threaded variant: caller-tracked Link skips the per-iteration
      // RELAXED load (owner-exclusive across the whole push window).
      slot_link = linkage::link_store_next_uncertify_known(
          slot.link, slot_link, stack_top(old_stk));
      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1,
                                     static_cast<uint16_t>(my_idx));
      uint64_t desired = combined_pack(val, new_stk);
      if (combined_.compare_exchange_weak(old, desired,
                                           cpp::MemoryOrder::ACQ_REL,
                                           cpp::MemoryOrder::RELAXED)) {
        break;
      }
    }
    // Phase 2.5: pre-kernel spin on slot.link's state byte.
    // spin_on_link_state exits the moment a waker's link_cas_snap
    // SIGNALED_*. ORPHAN ⇒ waker's detach lost to a push, so
    // self_splice_if_orphan is proactive cleanup; otherwise we'd
    // leave an IDLE-mid-chain reference for walkers to opp-splice.
    //
    // ALERT_FIRED bit: pre-mark from WAITING (which is what Phase
    // 2.5 spinners observe) does NOT issue an alert (Phase 2.5
    // cache spin catches the wake without a syscall), so the bit
    // is 0 here by structural invariant. The is_alert_fired()
    // check is hence a no-op on this site, but kept uniform with
    // the rule shared by every SIGNALED-observation site so any
    // future refactor that introduces an IN_KERNEL pre-mark route
    // here will behave correctly.
    if (spin_wait::spin_on_link_state(&slot.link, wait_slot::WAITING)) {
      linkage::Link snap = slot.link.load(cpp::MemoryOrder::ACQUIRE);
      uint8_t st = snap.state();
      if (wait_slot::state_is_signaled(st)) {
        if (snap.is_alert_fired())
          wait_slot::mark_expect_late_alert();
        long ho = decode_signaled_ret(st);
        bool invalidated =
        (slot.wait_address.load(cpp::MemoryOrder::RELAXED) == 0);
        if (wait_slot::state_is_orphan(st))
          self_splice_if_orphan(slot, my_idx);
        clear_slot_owned(slot, my_idx);
        return wait_epilogue(nested, my_idx, invalidated ? -EINVAL : ho);
      }
      // Non-SIGNALED with no waker pre-mark shouldn't happen
      // (TIMED_OUT requires our CAS); fall through as if spin timed
      // out, defensively.
    }

    // Phase 3: WAITING → IN_KERNEL. Tells the popper to use the
    // alert syscall instead of relying on the Phase-2.5 cache spin.
    // CAS fail ⇒ waker pre-marked between Phase-2.5 exit and here;
    // decode and exit. Pre-mark snap was WAITING (CAS expected
    // WAITING), so ALERT_FIRED is 0 by invariant — the bit check
    // is a no-op here for the same reason as Phase 2.5.
    if (!linkage::link_cas_state<wait_slot::WAITING,
                                     wait_slot::IN_KERNEL, wait_slot::WaitSlotStateTraits>(slot.link)) {
      linkage::Link snap = slot.link.load(cpp::MemoryOrder::ACQUIRE);
      uint8_t st = snap.state();
      if (wait_slot::state_is_signaled(st) && snap.is_alert_fired())
        wait_slot::mark_expect_late_alert();
      long ho = decode_signaled_ret(st);
      bool invalidated =
        (slot.wait_address.load(cpp::MemoryOrder::RELAXED) == 0);
      if (wait_slot::state_is_orphan(st))
        self_splice_if_orphan(slot, my_idx);
      clear_slot_owned(slot, my_idx);
      return wait_epilogue(nested, my_idx, invalidated ? -EINVAL : ho);
    }
    // Phase 4: kernel sleep.
    LARGE_INTEGER nt_timeout;
    LARGE_INTEGER *nt_timeout_ptr = timeout_to_nt(timeout, nt_timeout);
    PVOID tid_ptr =
        reinterpret_cast<PVOID>(static_cast<uintptr_t>(
            slot.thread_id.load(cpp::MemoryOrder::RELAXED)));

    // Stale-latched-alert distinguisher — full protocol in
    // wait_slot.h. Single discriminator now: the per-thread
    // expect_late_alert flag, set at SIGNALED-observation sites
    // whose link snap carries LINK_ALERT_FIRED_BIT (waker pre-
    // marked at IN_KERNEL → alert was fired) and at drain misses.
    // Cross-thread RELEASE-bumps on a per-thread counter are no
    // longer needed — the bit publishes "alert in flight"
    // structurally on the wake's own atomic.

    long ret = 0;
    for (;;) {
      // Pre-wait check: waker may have pre-marked between Phase-3
      // CAS and here. NO drain on this branch — we haven't entered
      // NtWait yet, so any waker alert latches for the first NtWait
      // after re-park. ALERT_FIRED is the structural signal: state
      // went WAITING → IN_KERNEL via Phase 3, so a waker pre-mark
      // observed here was at IN_KERNEL ⇒ alert was fired ⇒ set the
      // flag so the next NtWait classifies the latched alert stale.
      {
        linkage::Link snap = slot.link.load(cpp::MemoryOrder::ACQUIRE);
        uint8_t st = snap.state();
        if (wait_slot::state_is_signaled(st)) {
          if (snap.is_alert_fired())
            wait_slot::mark_expect_late_alert();
          long sig_ret = decode_signaled_ret(st);
          if constexpr (HasPredicate) {
            PhaseFourActionResult result = handle_pred_signaled_wake(
                slot, my_idx, nested, cond, st, sig_ret, timeout,
                nt_timeout, nt_timeout_ptr, tid_ptr);
            // Exhaustive — -Wswitch-enum catches new PhaseFourAction
            // values without a case.
            switch (result.action()) {
            case PhaseFourAction::Continue:
              continue;
            case PhaseFourAction::ReturnZero:
              return wait_epilogue(nested, my_idx, 0);
            case PhaseFourAction::Break:
              ret = result.break_ret();
              break;
            }
            break; // exit Phase 4 loop on Break
          } else {
            ret = sig_ret;
            break;
          }
        }
      }

      NTSTATUS status = ::NtWaitForAlertByThreadId(tid_ptr, nt_timeout_ptr);

      // Post-wake state check. SIGNALED ⇒ waker committed. If
      // status == STATUS_ALERTED the alert WAS this wake's wake-up
      // event — already consumed, no flag needed. Otherwise drain
      // (which sets the flag on miss).
      {
        linkage::Link snap = slot.link.load(cpp::MemoryOrder::ACQUIRE);
        uint8_t st = snap.state();
        if (wait_slot::state_is_signaled(st)) {
          long decoded_ret = decode_signaled_ret(st);
          if (status != STATUS_ALERTED) {
            // Wakeup came from a different source (stray APC, zero
            // timeout). Drain the waker's pending alert BEFORE any
            // re-park, else it latches into re-park's first
            // iteration and misclassifies as signal.
            drain_waker_alert(tid_ptr);
          }
          if constexpr (HasPredicate) {
            PhaseFourActionResult result = handle_pred_signaled_wake(
                slot, my_idx, nested, cond, st, decoded_ret, timeout,
                nt_timeout, nt_timeout_ptr, tid_ptr);
            switch (result.action()) {
            case PhaseFourAction::Continue:
              continue;
            case PhaseFourAction::ReturnZero:
              return wait_epilogue(nested, my_idx, 0);
            case PhaseFourAction::Break:
              ret = result.break_ret();
              break;
            }
            break;
          } else {
            ret = decoded_ret;
            break;
          }
        }
      }

      if (status == STATUS_TIMEOUT) {
        ret = cancel_or_absorb(slot, my_idx, nested, tid_ptr, -ETIMEDOUT);
        break;
      }

      // Still IN_KERNEL, no TIMED_OUT, no SIGNALED. Cases:
      //   (a) STATUS_ALERTED + flag set → stale latched alert;
      //       reloop.
      //   (b) STATUS_USER_APC → Interruptible: EINTR, else reloop.
      //   (c) Stray kernel alert (no flag, no APC) → signal-style.
      if (wait_slot::classify_stale_alert(status)) {
        // Pre-park re-check on stale alert: mirrors Linux
        // futex_wait spurious handling — return 0 if cond now
        // holds rather than re-park indefinitely.
        if (cond.satisfied(value_.load(cpp::MemoryOrder::ACQUIRE))) {
          // cancel_or_absorb's internal drain may re-set the flag
          // on CAS-fail — intentional: a NEW racing waker's alert
          // can latch for a future cycle independently.
          ret = cancel_or_absorb(slot, my_idx, nested, tid_ptr, 0);
          break;
        }
        nt_timeout_ptr = timeout_to_nt(timeout, nt_timeout);
        if (nt_timeout_ptr && nt_timeout_ptr->QuadPart == -1) {
          ret = cancel_or_absorb(slot, my_idx, nested, tid_ptr, -ETIMEDOUT);
          break;
        }
        continue;
      }

      // Signal-style wake (b/c). Interruptible → EINTR; else reloop.
      if constexpr (Interruptible) {
        ret = cancel_or_absorb(slot, my_idx, nested, tid_ptr, -EINTR);
        break;
      }

      nt_timeout_ptr = timeout_to_nt(timeout, nt_timeout);
      if (nt_timeout_ptr && nt_timeout_ptr->QuadPart == -1) {
        ret = cancel_or_absorb(slot, my_idx, nested, tid_ptr, -ETIMEDOUT);
        break;
      }
    }

    if (ret != -ETIMEDOUT && ret != -EINTR) {
      // wait_address==0 ⇒ drain_waiters / stale-slot handler
      // invalidated us while we slept (mutex destroyed/recycled).
      // Bail -EINVAL rather than return to lock_slow on recycled
      // storage.
      bool invalidated =
        (slot.wait_address.load(cpp::MemoryOrder::RELAXED) == 0);
      // ORPHAN proactive splice. Skip on invalidated — stale-slot
      // waker already emitted CLEAN.
      if (!invalidated)
        self_splice_if_orphan(slot, my_idx);
      clear_slot_owned(slot, my_idx);
      if (invalidated)
        return wait_epilogue(nested, my_idx, -EINVAL);
    }
    // ETIMEDOUT/EINTR: slot was spliced+reclaimed via
    // inline_unlink (or another actor; gen-check kept reclaim
    // single-actor). DO NOT roll state back from TIMED_OUT — would
    // alert a moved-on thread.
    return wait_epilogue(nested, my_idx, ret);
  }

  // Help-detach a top slot already SIGNALED_* (a prior popper
  // pre-marked but lost detach to a push). Atomic with the detach:
  // clear subsystem and publish CERT via the ORPHAN→CLEAN state
  // upgrade so the slot leaves our chain in the CLEAN+CERT=1
  // terminal, satisfying clear_slot_owned for any concurrent owner.
  //
  // Returns true iff the stack_ CAS committed. Caller `continue`s
  // either way (CAS fail ⇒ chain head moved, re-snap).
  //
  // CLEAN inputs accepted (state_is_signaled covers both): the
  // originating detacher already cleared subsystem + published
  // CERT; our certify CAS fails on state mismatch (harmless) and
  // the subsystem store is a benign redundant write.
  LIBC_INLINE bool help_detach_signaled(wait_slot::WaitSlot &slot,
                                         uint8_t st, uint32_t old_stk,
                                         uint32_t new_stk) {
    if (!stack_.compare_exchange_strong(old_stk, new_stk,
                                         cpp::MemoryOrder::ACQ_REL,
                                         cpp::MemoryOrder::RELAXED))
      return false;
    slot.subsystem.store(wait_slot::SubsystemKind::None,
                          cpp::MemoryOrder::RELAXED);
    if (st == wait_slot::SIGNALED_ORPHAN) {
      (void)linkage::link_cas_state_certify<
          wait_slot::SIGNALED_ORPHAN, wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(slot.link);
    } else if (st == wait_slot::SIGNALED_HANDOFF_ORPHAN) {
      (void)linkage::link_cas_state_certify<
          wait_slot::SIGNALED_HANDOFF_ORPHAN,
          wait_slot::SIGNALED_HANDOFF_CLEAN, wait_slot::WaitSlotStateTraits>(slot.link);
    }
    return true;
  }

public:
  // Waker step labels used by pop_and_signal_one / handoff_one /
  // signal_first_match_after:
  //   [B']  link_cas_snap WAITING/IN_KERNEL → SIGNALED_*_ORPHAN.
  //   [A']  stack_ CAS detach (best-effort; loss ⇒ ORPHAN, waiter
  //         self-splices via self_splice_if_orphan).
  //   [U]   ORPHAN → CLEAN+CERT on detach win (best-effort).
  //   [K]   Alert IN_KERNEL waiters (unconditional — [B'] committed).
  // Cost: 2 LOCK'd RMWs + ~1 for [U]. ORPHAN waiters pay O(D) self-
  // splice on their own wake path; lock-holder unblock stays O(1).
  //
  // The commit helpers below package the load-bearing orderings
  // (capture-tid-with-owner-ref I4, store-value-before-alert,
  // post-detach state-upgrade) so a single fix here covers every
  // call site and a new caller can't silently reorder them.

  // I4 token: tid + captured ThreadHandle (packed), the two pieces a
  // waker needs to alert an IN_KERNEL owner. Both must be read pre-
  // recycle — late capture can bind a recycled slot's new owner.
  // Constructible only via capture_wake_target; commit helpers
  // consume the token and never re-load the slot, so there is no
  // "second sample" path.
  class WakeCommitToken {
  public:
    LIBC_INLINE constexpr uint64_t owner_packed() const {
      return owner_packed_;
    }
    LIBC_INLINE constexpr uint32_t tid() const { return tid_; }

  private:
    LIBC_INLINE constexpr WakeCommitToken(uint64_t o, uint32_t t)
        : owner_packed_(o), tid_(t) {}
    // Friend the enclosing class so the static factory below can
    // call the private ctor (free-function friend would name a
    // different entity in the enclosing namespace).
    friend class Futex;
    uint64_t owner_packed_;
    uint32_t tid_;
  };

  // Caller MUST have pre-marked (link_cas_snap → SIGNALED_*) before
  // calling — pre-mark blocks owner's clear_slot_owned, so tid +
  // owner handle reflect the current lifecycle. tid first so the
  // cheaper u32 read narrows the recycle window between the two
  // loads.
  LIBC_INLINE static WakeCommitToken
  capture_wake_target(wait_slot::WaitSlot &slot) {
    uint32_t tid = slot.thread_id.load(cpp::MemoryOrder::RELAXED);
    uint64_t owner_packed = wait_slot::owner_handle_packed(slot);
    return WakeCommitToken{owner_packed, tid};
  }

  // Post-detach commit + alert for pop_and_signal_one /
  // signal_first_match_after.
  //   (1) iff detached: subsystem=None, ORPHAN→CLEAN+CERT atomic.
  //   (2) iff pre_mark_state==IN_KERNEL: alert via captured token.
  // ORPHAN-stay-ORPHAN bail is benign — waiter's
  // self_splice_if_orphan publishes CERT on splice success.
  // subsystem clear is detach-gated: an undetached slot may still
  // be chain-reachable, so subsystem must remain Futex for thread-
  // exit cleanup to dispatch the right unlinker.
  LIBC_INLINE void
  commit_pop_wake(wait_slot::WaitSlot &slot,
                   const WakeCommitToken &token,
                   uint8_t pre_mark_state, bool detached) {
    if (detached) {
      slot.subsystem.store(wait_slot::SubsystemKind::None,
                            cpp::MemoryOrder::RELAXED);
      // [U] ORPHAN → CLEAN + CERT atomic.
      (void)linkage::link_cas_state_certify<
          wait_slot::SIGNALED_ORPHAN, wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(
          slot.link);
    }
    // [K] Phase 2.5 cache spin catches WAITING; IN_KERNEL needs syscall.
    if (pre_mark_state == wait_slot::IN_KERNEL)
      wait_slot::alert_one_if_live(token.owner_packed(), token.tid(), slot);
  }

  // Post-detach commit for handoff_one WAITING. Differs from
  // commit_pop_wake: HANDOFF state pair, no alert (HANDOFF only
  // emits to WAITING; alert-loss risk on IN_KERNEL), no value
  // store (transit_val published BEFORE the pre-mark — see
  // handoff_one WAITING branch).
  LIBC_INLINE void commit_handoff_waiting(wait_slot::WaitSlot &slot,
                                            bool detached) {
    if (detached) {
      slot.subsystem.store(wait_slot::SubsystemKind::None,
                            cpp::MemoryOrder::RELAXED);
      (void)linkage::link_cas_state_certify<
          wait_slot::SIGNALED_HANDOFF_ORPHAN,
          wait_slot::SIGNALED_HANDOFF_CLEAN, wait_slot::WaitSlotStateTraits>(slot.link);
    }
  }

  // Full IN_KERNEL unlock-wake — most ordering-sensitive helper.
  // Used only by handoff_one's IN_KERNEL branch.
  //
  // Order is LOAD-BEARING (inverting (2)/(3) is the 16T starve
  // vector documented in handoff_one IN_KERNEL):
  //   (1) iff detached: subsystem=None, ORPHAN→CLEAN+CERT atomic.
  //   (2) value_.store(unlock_val, SEQ_CST)   ← MUST PRECEDE (3)
  //   (3) alert_one_if_live(token)
  //
  // Alert is a cumulative fence, so the woken waiter's re-read of
  // value_ sees unlock_val. Inverted, the waker can be preempted
  // between alert return and store; waiter re-CAS's stale LOCKED,
  // re-parks → livelock.
  //
  // SEQ_CST store: RawMutex's exchange(IN_CONTENTION, ACQUIRE)
  // retry depends on globally-ordered observation.
  LIBC_INLINE void commit_handoff_in_kernel_unlock(
      wait_slot::WaitSlot &slot, const WakeCommitToken &token,
      bool detached, FutexValueType unlock_val) {
    if (detached) {
      slot.subsystem.store(wait_slot::SubsystemKind::None,
                            cpp::MemoryOrder::RELAXED);
      // CERT publish via fetch_or instead of certify CAS-loop. This
      // path is mutex-only (handoff_one IN_KERNEL → RawMutex unlock,
      // no predicate-wait waiters), so the SIGNALED_ORPHAN→CLEAN
      // upgrade is functionally equivalent to ORPHAN+CERT=1: the
      // waiter's self_splice_if_orphan early-exits on CERT=1, then
      // clear_slot_owned proceeds. fetch_or is a single LOCK op, never
      // iterates under walker race.
      linkage::link_field_set_cert(slot.link, cpp::MemoryOrder::RELEASE);
    }
    value_.store(unlock_val, cpp::MemoryOrder::SEQ_CST);
    wait_slot::alert_one_if_live(token.owner_packed(), token.tid(), slot);
  }

private:
  // Walker mid-stack skip. Only entered when pop_and_signal_one's
  // top waiter has a FALSE filter. Walks from top, pre-marks +
  // detaches the first live same-futex matching waiter via prev.link
  // mid-splice. top_idx seeds `prev` — top is not dead, just filtered.
  //
  // Cost (only on this path; fast path stays O(1) lock-free):
  // O(D) slot reads, 1 pre-mark CAS + 1 splice CAS on success.
  //
  // Failure modes:
  //   Pre-mark fail   — state raced; advance past (another actor
  //                     wakes them).
  //   Splice fail     — target already pre-marked SIGNALED_ORPHAN;
  //                     waiter self-splices via harris_unlink.
  //   Walk to NULL    — no match; caller returns false.
  LIBC_INLINE bool signal_first_match_after(uint16_t top_idx,
                                             linkage::Link top_snap) {
    uint16_t prev = top_idx;
    linkage::Link prev_link = top_snap;
    uint16_t curr = top_snap.next();

    while (curr != wait_slot::NULL_INDEX) {
      auto &cs = wait_slot::get_slot(curr);
      linkage::Link c_snap = cs.link.load(cpp::MemoryOrder::ACQUIRE);
      uint16_t c_next = c_snap.next();
      uint8_t c_st = c_snap.state();

      // Advance past non-live-same-futex slots. wait_address!=this
      // ⇒ cross-futex; state non-live ⇒ dead (harris_unlink paths).
      if (cs.wait_address.load(cpp::MemoryOrder::RELAXED) !=
              reinterpret_cast<uintptr_t>(this) ||
          (c_st != wait_slot::WAITING &&
           c_st != wait_slot::IN_KERNEL)) {
        prev = curr;
        prev_link = c_snap;
        curr = c_next;
        continue;
      }

      // Null fn ⇒ unconditional match (classic wait() parked
      // beneath filtered waiters).
      wait_slot::PredicateFn filter =
          cs.filter_fn.load(cpp::MemoryOrder::RELAXED);
      if (filter) {
        uint32_t farg = cs.filter_arg.load(cpp::MemoryOrder::RELAXED);
        if (!filter(value_.load(cpp::MemoryOrder::ACQUIRE), farg)) {
          prev = curr;
          prev_link = c_snap;
          curr = c_next;
          continue;
        }
      }

      // [B'] Pre-mark ORPHAN. Fail ⇒ state raced; advance (legit
      // wake comes from another actor — don't retry in place).
      if (!linkage::link_cas_snap<wait_slot::SIGNALED_ORPHAN, wait_slot::WaitSlotStateTraits>(
              cs.link, c_snap)) {
        prev = curr;
        prev_link = c_snap;
        curr = c_next;
        continue;
      }

      // I4 token: post pre-mark, pre-detach.
      WakeCommitToken token = capture_wake_target(cs);

      // [A'] Mid-stack detach via prev.link CAS. Success ⇒ commit
      // upgrades ORPHAN→CLEAN; fail ⇒ slot stays ORPHAN, waiter
      // self-splices.
      auto &ps = wait_slot::get_slot(prev);
      bool detached = ps.link.compare_exchange_strong(
          prev_link, prev_link.with_next_uncertify(c_next),
          cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE);

      // [U] + [K]: state upgrade + IN_KERNEL alert.
      commit_pop_wake(cs, token, c_st, detached);
      return true;
    }

    // No match; caller returns false. Next unlock/notify re-scans.
    return false;
  }

public:
  LIBC_INLINE bool pop_and_signal_one() {
    for (;;) {
      uint32_t old_stk = stack_.load(cpp::MemoryOrder::ACQUIRE);
      uint16_t top = stack_top(old_stk);
      if (top == STACK_NULL) {
        return false;
      }

      auto &slot = wait_slot::get_slot(top);
      linkage::Link slot_snap = slot.link.load(cpp::MemoryOrder::ACQUIRE);
      uint16_t next_only = slot_snap.next();
      uint8_t st = slot_snap.state();

      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1, next_only);

      // ---- Stale slot: wait_address points elsewhere ----
      //
      // Owning thread is still alive and holds a TLS reference; do not
      // reclaim. Wake via CLEAN state transition so the waiter's
      // invalidation check (wait_address == 0) trips, then detach and
      // keep scanning. See drain_stale_top for the full protocol.
      if (slot.wait_address.load(cpp::MemoryOrder::RELAXED) !=
              reinterpret_cast<uintptr_t>(this)) {
        drain_stale_top(slot, slot_snap, top, st, old_stk, new_stk);
        continue;
      }

      // ---- Live slot on this Futex ----

      if (st == wait_slot::WAITING || st == wait_slot::IN_KERNEL) {
        // Filter (I7). Null fn ⇒ classic wake. Non-null + FALSE ⇒
        // descend into the walker to find a matching waiter deeper.
        // MUST NOT mutate link/subsystem on this filter path.
        // slot_snap's ACQUIRE pairs with Phase 2 push release, so
        // filter_fn/arg stores from the owner are visible.
        wait_slot::PredicateFn filter_fn =
            slot.filter_fn.load(cpp::MemoryOrder::RELAXED);
        if (filter_fn) {
          uint32_t filter_arg =
              slot.filter_arg.load(cpp::MemoryOrder::RELAXED);
          if (!filter_fn(value_.load(cpp::MemoryOrder::ACQUIRE), filter_arg))
            return signal_first_match_after(top, slot_snap);
        }

        // [B'] Pre-mark. Fail ⇒ state raced (owner moved
        // WAITING→IN_KERNEL or TIMED_OUT, or walker bumped tag);
        // retry with fresh snap.
        if (!linkage::link_cas_snap<wait_slot::SIGNALED_ORPHAN, wait_slot::WaitSlotStateTraits>(
                slot.link, slot_snap)) {
          continue;
        }
        // I4 token, post pre-mark, pre-detach.
        WakeCommitToken token = capture_wake_target(slot);

        // [A'] Best-effort detach — no harris_unlink here, keeps
        // lock-holder unblock O(1). Fail ⇒ waiter self-splices.
        bool detached = stack_.compare_exchange_strong(
            old_stk, new_stk, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::RELAXED);
        // [U]+[K]: state upgrade + alert iff IN_KERNEL.
        commit_pop_wake(slot, token, st, detached);
        return true;
      }

      if (st == wait_slot::TIMED_OUT) {
        // Dead at head — detach + reclaim. No state CAS (terminal).
        // wait_address stays as `this`; reclaim_slot's freelist_push
        // clears it before any new owner observes the slot.
        if (!stack_.compare_exchange_strong(
                old_stk, new_stk, cpp::MemoryOrder::ACQ_REL,
                cpp::MemoryOrder::RELAXED))
          continue;
        wait_slot::reclaim_slot(
            wait_slot::ReclaimAuthority::after_stack_pop(top));
        continue;
      }

      if (wait_slot::state_is_signaled(st)) {
        // Orphan from another popper that lost detach. Atomic
        // detach + certify; CAS fail ⇒ chain head moved, re-snap.
        (void)help_detach_signaled(slot, st, old_stk, new_stk);
        continue;
      }

      // IDLE at top = stale snapshot from a stolen-list reclaim
      // (see walker race table). Re-snap, NEVER assert.
      // Out-of-enum state also lands here defensively.
      continue;
    }
  }

  // notify_one honours the slot.filter_fn — waiters whose pred is
  // false on the current value_ are skipped (descend the walker via
  // signal_first_match_after).
  LIBC_INLINE long notify_one(bool is_shared = false) {
    (void)is_shared;
    // No has_waiters() pre-check — pop_and_signal_one already checks
    // STACK_NULL on its first load.
    pop_and_signal_one();
    return 0;
  }

  // Steal the entire stack, then collect TIDs and batch-alert via
  // NtAlertMultipleThreadByThreadId. State CAS happens AFTER
  // capturing tid so WAITING threads don't recycle their slots
  // before we read.
  //
  // Filter policy: broadcast IGNORES slot.filter_fn — caller opted
  // into fan-out. Waiters whose user condition doesn't match
  // re-check in their caller loop and re-park via Phase 0. Applying
  // the filter here would break condvar-broadcast and fork-reinit.
  LIBC_INLINE long notify_all(bool is_shared = false) {
    (void)is_shared;

    // Empty short-circuit (common in condvar-broadcast even when no
    // waiter is parked). In-loop empty check still handles a
    // concurrent drainer between hint and steal CAS.
    if (stack_top(stack_.load(cpp::MemoryOrder::ACQUIRE)) == STACK_NULL)
      return 0;

    // 128-entry stack ring; mid-walk flushes when full. CompactTarget
    // (8B) over BatchTarget (24B) for 3× L1 density on the alert-time
    // TID re-check pass. AutoBoost key = wait-key address (waiters
    // parked on `this`); reused on every flush_alert_batch.
    wait_slot::CompactTarget tgt_buf[kAlertBatch];
    HANDLE out_buf[kAlertBatch];
    uint32_t scratch_used = 0;
    PS_ALERT_THREAD_EXTENDED_PARAMETER ab_ctx{};
    ab_ctx.Pointer = this;

    // Steal the entire stack atomically (CAS-32 on stack_).
    uint32_t old_stk;
    for (;;) {
      old_stk = stack_.load(cpp::MemoryOrder::ACQUIRE);
      if (stack_top(old_stk) == STACK_NULL)
        return 0;
      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1, STACK_NULL);
      if (stack_.compare_exchange_weak(old_stk, new_stk,
                                        cpp::MemoryOrder::ACQ_REL,
                                        cpp::MemoryOrder::RELAXED))
        break;
    }

    // Per-slot counter bumps happen inside alert_multiple_if_live
    // (under pin, on resolved lifecycles) — no in-walk writes to
    // potentially-retired lifecycle memory.

    uint32_t curr = stack_top(old_stk);
    uint32_t total_woken = 0;

    // Software pipeline (DEPTH=4): in-flight ACQUIRE loads via MSHRs
    // overlap cold-line latency across iterations instead of
    // stacking behind each LOCK XCHG fence. ~200-250 ns DRAM miss /
    // ~50-80 ns per-slot; deeper bloats the ring without benefit.
    static constexpr uint32_t PIPELINE_DEPTH = 4;
    struct PipeEntry {
      uint32_t idx;
      linkage::Link snap;
    };
    PipeEntry pipe[PIPELINE_DEPTH];
    uint32_t pipe_head = 0;
    uint32_t pipe_fill = 0;

    auto pipe_push_one = [&]() -> bool {
      if (curr == wait_slot::NULL_INDEX)
        return false;
      uint32_t tail = (pipe_head + pipe_fill) % PIPELINE_DEPTH;
      pipe[tail].idx = curr;
      pipe[tail].snap =
          wait_slot::get_slot(curr).link.load(cpp::MemoryOrder::ACQUIRE);
      curr = linkage::link_next(pipe[tail].snap);
      ++pipe_fill;
      return true;
    };

    while (pipe_fill < PIPELINE_DEPTH && pipe_push_one())
      ;

    while (pipe_fill > 0) {
      uint32_t slot_idx = pipe[pipe_head].idx;
      linkage::Link slot_snap = pipe[pipe_head].snap;
      pipe_head = (pipe_head + 1) % PIPELINE_DEPTH;
      --pipe_fill;

      auto &slot = wait_slot::get_slot(slot_idx);
      // TID before state CAS (CompactTarget; I4 contract — captured
      // tid must be pre-detach to bind the original owner, not a
      // recycled tid post-CAS).
      uint32_t tid = slot.thread_id.load(cpp::MemoryOrder::RELAXED);
      // Strong CAS-with-snap (not XCHG) closes the silent-overwrite
      // leak: an unrelated waker whose detach lost to our steal but
      // whose pre-mark + alert preceded would otherwise have its
      // owner's IDLE+CERT clobbered with our SIGNALED_CLEAN.
      bool ours = false;
      uint8_t old = linkage::link_cas_state_detached<
          wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(slot.link, slot_snap, ours);
      if (!ours) {
        // Owner / another waker handled the slot. Only TIMED_OUT
        // obliges us to reclaim (owner's harris walks post-steal
        // NULL head and won't reclaim). SIGNALED_*_ORPHAN ⇒ slot
        // off-chain by our steal but our failed CAS didn't publish
        // CERT; follow up via certify CAS. SIGNALED_*_CLEAN and IDLE
        // already carry CERT=1. wait_address stays as `this` —
        // reclaim_slot's freelist_push clears it before any new
        // owner could observe it.
        if (old == wait_slot::TIMED_OUT) {
          wait_slot::reclaim_slot(
              wait_slot::ReclaimAuthority::after_stack_steal(slot_idx));
        } else if (old == wait_slot::SIGNALED_ORPHAN) {
          (void)linkage::link_cas_state_certify<
              wait_slot::SIGNALED_ORPHAN, wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(
              slot.link);
        } else if (old == wait_slot::SIGNALED_HANDOFF_ORPHAN) {
          (void)linkage::link_cas_state_certify<
              wait_slot::SIGNALED_HANDOFF_ORPHAN,
              wait_slot::SIGNALED_HANDOFF_CLEAN, wait_slot::WaitSlotStateTraits>(slot.link);
        }
        pipe_push_one();
        continue;
      }
      // ours == true: clear subsystem alongside the wake commit
      // (matches commit_pop_wake's pattern). clear_slot_owned writes
      // subsystem=None on owner wake, so this is technically
      // redundant for the steady-state path; explicit clear keeps
      // thread-exit dispatch tight on the rare owner-death window.
      slot.subsystem.store(wait_slot::SubsystemKind::None,
                           cpp::MemoryOrder::RELAXED);
      if (old == wait_slot::IN_KERNEL) {
        tgt_buf[scratch_used++] = {tid, slot_idx};
        if (scratch_used == kAlertBatch)
          flush_alert_batch(tgt_buf, out_buf, scratch_used, &ab_ctx);
        ++total_woken;
      } else if (old == wait_slot::WAITING) {
        // Phase 2.5 cache spin observes our state CAS.
        ++total_woken;
      } else if (wait_slot::state_is_signaled(old)) {
        // Concurrent pop already pre-marked — our CAS to CLEAN is
        // benign (same wake, same off-stack semantics).
      } else if (old == wait_slot::TIMED_OUT) {
        // wait_address stays as `this`; reclaim_slot's freelist_push
        // clears it.
        wait_slot::reclaim_slot(
            wait_slot::ReclaimAuthority::after_stack_steal(slot_idx));
      }

      pipe_push_one();
    }

    flush_alert_batch(tgt_buf, out_buf, scratch_used, &ab_ctx);

    return static_cast<long>(total_woken);
  }

  // Test-only: chunked notify_all with yield between chunks.
  // chunk_size=0 ⇒ use LP count.
  LIBC_INLINE long notify_all_chunked(uint32_t chunk_size,
                                       bool do_yield = true) {
    if (stack_top(stack_.load(cpp::MemoryOrder::ACQUIRE)) == STACK_NULL)
      return 0;

    uint32_t lp = chunk_size
                      ? chunk_size
                      : windows_util::shared_user_data()->ActiveProcessorCount;

    // 128-entry stack ring shared across chunks; flush at chunk
    // boundaries (preserving yield-after-alert ordering) and mid-chunk
    // when full (rare — only when one chunk's lp budget exceeds 128).
    // AutoBoost key = wait-key address; one ab_ctx for every flush.
    wait_slot::CompactTarget tgt_buf[kAlertBatch];
    HANDLE out_buf[kAlertBatch];
    uint32_t scratch_used = 0;
    PS_ALERT_THREAD_EXTENDED_PARAMETER ab_ctx{};
    ab_ctx.Pointer = this;

    uint32_t old_stk;
    for (;;) {
      old_stk = stack_.load(cpp::MemoryOrder::ACQUIRE);
      if (stack_top(old_stk) == STACK_NULL)
        return 0;
      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1, STACK_NULL);
      if (stack_.compare_exchange_weak(old_stk, new_stk,
                                        cpp::MemoryOrder::ACQ_REL,
                                        cpp::MemoryOrder::RELAXED))
        break;
    }

    uint32_t curr = stack_top(old_stk);
    uint32_t total_woken = 0;

    // Same software pipeline as notify_all. The chunk-boundary
    // check gates REFILLS (not drains), so in-flight pipe entries
    // always finish in the current chunk. Worst-case overshoot =
    // PIPELINE_DEPTH-1, within the advisory contract.
    static constexpr uint32_t PIPELINE_DEPTH = 4;
    struct PipeEntry {
      uint32_t idx;
      linkage::Link snap;
    };
    PipeEntry pipe[PIPELINE_DEPTH];
    uint32_t pipe_head = 0;
    uint32_t pipe_fill = 0;

    auto pipe_push_one = [&]() -> bool {
      if (curr == wait_slot::NULL_INDEX)
        return false;
      uint32_t tail = (pipe_head + pipe_fill) % PIPELINE_DEPTH;
      pipe[tail].idx = curr;
      pipe[tail].snap =
          wait_slot::get_slot(curr).link.load(cpp::MemoryOrder::ACQUIRE);
      curr = linkage::link_next(pipe[tail].snap);
      ++pipe_fill;
      return true;
    };

    while (curr != wait_slot::NULL_INDEX || pipe_fill > 0) {
      uint32_t chunk_live = 0;
      uint32_t chunk_alerts = 0; // gates the post-chunk yield

      // Initial fill for this chunk (bounded by chunk budget lp).
      while (pipe_fill < PIPELINE_DEPTH && chunk_live + pipe_fill < lp &&
             pipe_push_one())
        ;

      while (pipe_fill > 0) {
        uint32_t slot_idx = pipe[pipe_head].idx;
        linkage::Link slot_snap = pipe[pipe_head].snap;
        pipe_head = (pipe_head + 1) % PIPELINE_DEPTH;
        --pipe_fill;

        auto &slot = wait_slot::get_slot(slot_idx);
        // TID before state CAS (CompactTarget; I4 contract — pre-CAS
        // capture so a recycled slot post-CAS can't bind a wrong tid).
        uint32_t tid = slot.thread_id.load(cpp::MemoryOrder::RELAXED);
        // Strong CAS-with-snap, not XCHG — see notify_all for the
        // silent-overwrite leak rationale.
        bool ours = false;
        uint8_t old = linkage::link_cas_state_detached<
            wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(slot.link, slot_snap, ours);
        if (!ours) {
          // SIGNALED_*_ORPHAN ⇒ certify CAS follow-up; wait_address
          // stays as `this` for TIMED_OUT — reclaim_slot's freelist_push
          // clears it before any new owner observes the slot.
          if (old == wait_slot::TIMED_OUT) {
            wait_slot::reclaim_slot(
                wait_slot::ReclaimAuthority::after_stack_steal(slot_idx));
          } else if (old == wait_slot::SIGNALED_ORPHAN) {
            (void)linkage::link_cas_state_certify<
                wait_slot::SIGNALED_ORPHAN, wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(
                slot.link);
          } else if (old == wait_slot::SIGNALED_HANDOFF_ORPHAN) {
            (void)linkage::link_cas_state_certify<
                wait_slot::SIGNALED_HANDOFF_ORPHAN,
                wait_slot::SIGNALED_HANDOFF_CLEAN, wait_slot::WaitSlotStateTraits>(slot.link);
          }
          if (chunk_live + pipe_fill < lp)
            pipe_push_one();
          continue;
        }
        // ours == true: clear subsystem alongside the wake commit
        // (matches commit_pop_wake's pattern).
        slot.subsystem.store(wait_slot::SubsystemKind::None,
                             cpp::MemoryOrder::RELAXED);
        if (old == wait_slot::IN_KERNEL) {
          tgt_buf[scratch_used++] = {tid, slot_idx};
          ++chunk_alerts;
          // Mid-chunk flush only when the ring fills (lp > 128).
          if (scratch_used == kAlertBatch)
            flush_alert_batch(tgt_buf, out_buf, scratch_used, &ab_ctx);
          ++chunk_live;
        } else if (old == wait_slot::WAITING) {
          ++chunk_live;
        } else if (wait_slot::state_is_signaled(old)) {
          // Concurrent pop pre-marked; benign.
        } else if (old == wait_slot::TIMED_OUT) {
          // wait_address stays as `this`; reclaim_slot's freelist_push
          // clears it.
          wait_slot::reclaim_slot(
              wait_slot::ReclaimAuthority::after_stack_steal(slot_idx));
        }

        // Refill only while under chunk budget — else drain
        // in-flight and close the chunk.
        if (chunk_live + pipe_fill < lp)
          pipe_push_one();
      }

      total_woken += chunk_live;

      // Per-chunk flush + yield. flush_alert_batch is a no-op if the
      // chunk's alerts already drained via mid-chunk fills. Yield is
      // gated on this chunk having actually issued alerts (a
      // WAITING-only chunk wakes via Phase 2.5 cache spin and skips
      // the yield).
      if (chunk_alerts > 0) {
        flush_alert_batch(tgt_buf, out_buf, scratch_used, &ab_ctx);
        if (do_yield && curr != wait_slot::NULL_INDEX && total_woken > lp)
          ::NtYieldExecution();
      }
    }

    return static_cast<long>(total_woken);
  }

  LIBC_INLINE long store_and_notify_all_chunked(FutexValueType new_val,
                                                 uint32_t chunk_size,
                                                 bool do_yield = true) {
    value_.store(new_val, cpp::MemoryOrder::SEQ_CST);
    return notify_all_chunked(chunk_size, do_yield);
  }

  // SEQ_CST store (XCHG = full barrier on x86) then pop+signal.
  // A waiter mid-push fails their CAS-64 (value portion doesn't
  // match the new value) — no lost wakeup.
  LIBC_INLINE long store_and_notify(FutexValueType new_val,
                                     bool is_shared = false) {
    (void)is_shared;
    value_.store(new_val, cpp::MemoryOrder::SEQ_CST);
    pop_and_signal_one();
    return 0;
  }

  // ===== Mutex handoff =====
  //
  // Scans the stack top and returns Handoff / Completed / Empty per
  // UnlockOutcome. WAITING branch transfers ownership; IN_KERNEL
  // stores unlock_val then alerts. Stale / TIMED_OUT / SIGNALED /
  // IDLE branches all `continue` (drain-and-retry); loop exits only
  // at a live-slot branch or STACK_NULL.
  //
  // STORE-BEFORE-WAKE IS LOAD-BEARING. Every Completed path does
  // value_.store(unlock_val, SEQ_CST) BEFORE the wake (cache
  // publish or alert syscall). Inverting is the 16T starve vector:
  // waker preempted between alert and store, waiter re-CAS's stale
  // LOCKED and re-parks (observed in futex_bench).
  //
  // Filter policy: handoff_one does NOT consult slot.filter_fn.
  // HANDOFF is unconditional — a filter-rejected waiter would
  // strand value_ at transit_val with no consumer. Filtered acquire
  // ⇒ use notify_one / unlock_notify → pop_and_signal_one.
  LIBC_INLINE UnlockOutcome handoff_one(FutexValueType unlock_val,
                                         FutexValueType transit_val,
                                         bool is_shared = false) {
    (void)is_shared;
    // No pre-loop has_waiters() — first iteration's STACK_NULL
    // check returns Empty in the uncontended case (consistent with
    // pop_and_signal_one).
    for (;;) {
      uint32_t old_stk = stack_.load(cpp::MemoryOrder::ACQUIRE);
      uint16_t top = stack_top(old_stk);
      if (top == STACK_NULL) {
        return UnlockOutcome::Empty;
      }

      auto &slot = wait_slot::get_slot(top);
      linkage::Link slot_snap = slot.link.load(cpp::MemoryOrder::ACQUIRE);
      uint16_t next_only = slot_snap.next();
      uint8_t st = slot_snap.state();

      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1, next_only);

      // Stale: wake+detach, never handoff. handoff_one cannot
      // transfer ownership across futexes.
      if (slot.wait_address.load(cpp::MemoryOrder::RELAXED) !=
              reinterpret_cast<uintptr_t>(this)) {
        drain_stale_top(slot, slot_snap, top, st, old_stk, new_stk);
        continue;
      }

      if (st == wait_slot::WAITING) {
        // Store transit_val BEFORE the pre-mark CAS — load-bearing.
        // link_cas_snap IS the single wake publish; a WAITING
        // waiter spinning in Phase 2.5 can wake the instant the
        // state byte changes, so any value_.store AFTER the
        // pre-mark would race the claim CAS (waiter sees stale
        // LOCKED → fails → re-parks → handoff stranded). The
        // marker is only emitted when transit_val != unlock_val;
        // otherwise this degrades to "leave value at locked
        // sentinel" (RawMutex).
        bool use_transit = (transit_val != unlock_val);
        if (use_transit)
          value_.store(transit_val, cpp::MemoryOrder::SEQ_CST);

        // [B'] Pre-mark HANDOFF_ORPHAN.
        if (!linkage::link_cas_snap<wait_slot::SIGNALED_HANDOFF_ORPHAN, wait_slot::WaitSlotStateTraits>(
                slot.link, slot_snap, cpp::MemoryOrder::ACQ_REL,
                cpp::MemoryOrder::ACQUIRE)) {
          // DO NOT roll value_ back to unlock_val on pre-mark fail.
          // The transient UNLOCKED window would let try_lock steal
          // the lock; our IN_KERNEL retry's later store(UNLOCKED)
          // would then clobber their LOCKED → double ownership.
          // Leaving value_ at transit_val is safe: try_lock sees
          // TRANSIT and fails; lock_slow parks on wait(TRANSIT);
          // retry path (IN_KERNEL or Empty) publishes UNLOCKED
          // atomically.
          continue;
        }
        // [A'] Best-effort detach.
        bool detached = stack_.compare_exchange_strong(
            old_stk, new_stk, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::RELAXED);
        // [U]: HANDOFF_ORPHAN → HANDOFF_CLEAN+CERT. No alert
        // (WAITING catches via Phase 2.5 cache spin). No value
        // store (transit_val published pre pre-mark above).
        commit_handoff_waiting(slot, detached);
        return UnlockOutcome::Handoff;
      }

      if (st == wait_slot::IN_KERNEL) {
        // Handoff to a parked waiter is unsafe (alert-loss risk):
        // pre-mark plain ORPHAN, waiter returns ret=0 and retries
        // acquire against unlock_val.
        //
        //   [B']  IN_KERNEL → SIGNALED_ORPHAN.
        //   [A']  Best-effort detach.
        //         Capture tid + owner_ref (I4).
        //   store value_ = unlock_val (SEQ_CST)   ← BEFORE alert
        //   [U]   ORPHAN → CLEAN.
        //   [K]   alert_one_if_live (unconditional).
        //
        // Store-before-alert is mandatory (alert syscall is the
        // cumulative fence). Inverting it is the hang vector —
        // waker preempted between alert and store, waiter re-CASs
        // LOCKED, re-parks, starves indefinitely. The full
        // sequence is packaged in commit_handoff_in_kernel_unlock
        // so per-site reordering is impossible.
        if (!linkage::link_cas_snap<wait_slot::SIGNALED_ORPHAN, wait_slot::WaitSlotStateTraits>(
                slot.link, slot_snap)) {
          continue;
        }
        WakeCommitToken token = capture_wake_target(slot);
        bool detached = stack_.compare_exchange_strong(
            old_stk, new_stk, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::RELAXED);
        commit_handoff_in_kernel_unlock(slot, token, detached, unlock_val);
        return UnlockOutcome::Completed;
      }

      if (st == wait_slot::TIMED_OUT) {
        // wait_address stays as `this`; reclaim_slot's freelist_push
        // clears it before any new owner observes the slot.
        if (!stack_.compare_exchange_strong(
                old_stk, new_stk, cpp::MemoryOrder::ACQ_REL,
                cpp::MemoryOrder::RELAXED))
          continue;
        wait_slot::reclaim_slot(
            wait_slot::ReclaimAuthority::after_stack_pop(top));
        continue;
      }

      if (wait_slot::state_is_signaled(st)) {
        // Orphan from a prior pre-mark — atomic detach + certify.
        (void)help_detach_signaled(slot, st, old_stk, new_stk);
        continue;
      }

      // IDLE at top = stale snapshot from stolen-list reclaim.
      // Re-snap, NEVER assert.
      continue;
    }
  }

  // Dispatch on handoff_one's UnlockOutcome:
  //   Handoff/Completed — handoff_one already finished; return true.
  //   Empty             — store + pop (Dekker catch for a post-scan
  //                       pusher). Returns true iff a waiter woke.
  //
  // transit_val: distinct value during a WAITING handoff so new
  // acquirers see "handoff in progress" and refrain from CAS
  // 0→locked. transit_val == unlock_val disables the marker (legacy;
  // used by RawMutex until migrated).
  LIBC_INLINE bool unlock_notify(FutexValueType unlock_val,
                                  FutexValueType transit_val,
                                  bool is_shared = false) {
    (void)is_shared;
    UnlockOutcome outcome = handoff_one(unlock_val, transit_val);
    // Exhaustive switch — `-Wswitch-enum` warns if a future
    // UnlockOutcome value is added without an explicit case. The
    // post-switch store + Dekker-catch is the Empty path; Handoff/
    // Completed both return early because handoff_one already
    // sequenced the value_ store and the wake.
    switch (outcome) {
    case UnlockOutcome::Handoff:
    case UnlockOutcome::Completed:
      return true;
    case UnlockOutcome::Empty:
      break;
    }

    // Empty: store unlock_val, then Dekker-catch any push that
    // landed before our store.
    value_.store(unlock_val, cpp::MemoryOrder::SEQ_CST);
    return pop_and_signal_one();
  }

  LIBC_INLINE long store_and_notify_all(FutexValueType new_val,
                                         bool is_shared = false) {
    (void)is_shared;
    value_.store(new_val, cpp::MemoryOrder::SEQ_CST);
    return notify_all();
  }
};

static_assert(__is_standard_layout(Futex),
              "Futex must be a standard layout type.");
static_assert(sizeof(Futex) == 8, "Futex must be 8 bytes.");

// Trampoline registered as wait_slot's SubsystemKind::Futex
// dispatcher so thread-exit cleanup can splice a dying thread's
// Futex-linked slot. expected_gen gates the splice against a
// reallocated-since-capture slot.
LIBC_INLINE bool
harris_unlink_futex_trampoline(void *futex_as_void, uint16_t idx,
                               uint32_t expected_gen) {
  if (!futex_as_void)
    return false;
  Futex *f = static_cast<Futex *>(futex_as_void);
  return f->harris_unlink(idx, expected_gen);
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_UTILS_H
