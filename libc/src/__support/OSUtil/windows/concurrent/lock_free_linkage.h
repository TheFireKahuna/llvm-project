//===--- Lock-free intrusive linkage substrate ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception 
//
//===----------------------------------------------------------------------===//
//
// `linkage::Link` is a 64-bit packed atomic word that fuses chain
// linkage (`next`), per-link ABA defense (`tag`), an 8-bit `state`
// byte, and three reserved bits (`MARK`, `CERT`, `ALERT_FIRED`) into
// one value. Every chained-node subsystem in the libc that wants
// lock-free intrusive linkage stamps a `cpp::Atomic<linkage::Link>`
// at a fixed offset of its node and consumes the helpers below.
//
// History: this file was extracted from `wait_slot.h` in Phase 0b of
// the NTPOSIX roadmap. Prior callers reached for hand-rolled Treiber
// chains, hazard pointers, retire queues, or `ReclaimAuthority`
// proofs. The Safety Triad below makes those mechanisms unnecessary.
//
// ---------------------------------------------------------------------------
// SAFETY TRIAD — the three structural invariants every consumer
// inherits by construction. Codified here so future consumers do
// not reach for hazard pointers or retire queues:
//
//  T1. TAG MONOTONICITY. Every Link write bumps the 32-bit `tag`
//      field. The wither family on `Link` is the sole writer surface
//      and bumps unconditionally; the only no-bump entry points are
//      static factories (`Link::pack`, `Link::pack_marked`,
//      `Link::pack_certified`, `Link::from_raw`) which exist for
//      pool-init and atomic-load round-trips. Any concurrent
//      mutation between a snap-load and a CAS publishes a different
//      tag, so the CAS naturally fails — no separate verification
//      step needed. 32-bit width is load-bearing: at 10K reclaims/sec
//      the wraparound horizon is >130 years; 16-bit would wrap in
//      seconds under stress.
//
//  T2. IDLE-ON-REACHABLE-CHAIN RETRY. Walkers that observe an
//      "already-idle" state byte on a node still threaded into a
//      live chain MUST retry rather than splice. The state byte and
//      the chain linkage live in the same atomic word, so the only
//      way to legally observe `state==idle` mid-walk is to be on a
//      snap that predates a concurrent reclaim; the retry refreshes
//      that snap. Combined with T1, this eliminates the entire class
//      of "splice freed memory" races.
//
//  T3. NEVER-FREED POOL MEMORY. Substrate consumers are required to
//      back their `Link` storage with a pool whose physical memory
//      is acquired once and never returned to the OS. Static
//      arrays, demand-committed reserve regions, and lifetime-of-
//      process slab arenas are all valid. What is NOT valid is
//      `delete`, `munmap`, or any other path that lets a `Link`
//      address become invalid for read. Combined with T1 and T2,
//      this means a stale walker dereference always lands on valid
//      memory and reads either a tag-mismatch (CAS bails) or an
//      IDLE-on-reachable-chain (retry).
//
// Together T1 + T2 + T3 replace hazard pointers, RCU grace periods,
// epoch-based reclamation, and ad-hoc retire queues for every chain
// built on this substrate. Reach for those mechanisms ONLY if you
// can prove the Safety Triad is insufficient for your access
// pattern; the burden of proof is on the new consumer.
// ---------------------------------------------------------------------------
//
// State-machine semantics are NOT baked into the substrate. The
// state-aware CAS helpers (`link_cas_state`, `link_cas_state_certify`,
// `link_cas_state_detached`, `link_cas_snap`) take a `Traits` policy
// class with two compile-time hooks:
//
//   - `is_valid(uint8_t from, uint8_t to)`  — gates illegal
//     transitions at static_assert time. `DefaultStateTraits`
//     accepts every transition.
//   - `fires_alert(uint8_t from, uint8_t to)` — when true, the
//     CAS publishes `LINK_ALERT_FIRED_BIT` atomically with the
//     state transition. `DefaultStateTraits` never fires.
//
// Consumers wire their state machine into `Traits`. wait_slot's
// `WaitSlotStateTraits` (in wait_slot.h) is the canonical example.
// New consumers writing a different state machine supply their own.
//
// ---------------------------------------------------------------------------
// CONSUMER CHECKLIST. To plug a new subsystem onto this substrate:
//   1. Embed `cpp::Atomic<linkage::Link>` at a fixed offset in your
//      node type. Use `LINKAGE_REQUIRES_LINK_AT(NodeT, offset)` in
//      the consumer header to lock the offset structurally.
//   2. Back your nodes with pool memory honoring T3 above.
//   3. Define a `Traits` class if you need transition validation or
//      alert-bit publishing distinct from `DefaultStateTraits`.
//   4. Use the wither family on `Link` exclusively for writes —
//      never CAS a raw `uint64_t` against `link.value_`. The withers
//      enforce T1 by construction.
//   5. If you mid-chain splice, follow the Harris mark-then-help
//      protocol (see `LINK_MARK_BIT` doc); HELP-NOT-WAIT on observed
//      MARK is mandatory to remain robust to splicer death.
// ---------------------------------------------------------------------------
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_LOCK_FREE_LINKAGE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_LOCK_FREE_LINKAGE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

// ===========================================================================
// Debug instrumentation hooks — default to no-op for production.
// ===========================================================================
//
// LFL_TRACE_BRANCH: called at every walker branch entry that performs a
//   parent-CAS attempt. Records (branch_id, prev_or_gp, prev_link_raw,
//   curr, c_link_raw, c_next, spliced).
//
// LFL_VALIDATE_PUBLISH: called immediately after every successful parent
//   CAS site, passing the post-CAS snap of slot[c_next].link. The harness
//   uses this to detect stale c_next publication. NOTE: a post-CAS load
//   sees state at validation time, NOT at CAS time — so a FREE state is
//   AMBIGUOUS:
//     - STALE-AT-CAS  ⇒ real corruption: the chain has a FREE slot
//                       reachable from head right now.
//     - RACE-AFTER-CAS ⇒ benign: c_next was legitimately on chain at
//                        CAS time, but a peer raced to splice + push
//                        before our load. Chain self-heals.
//   The harness disambiguates by walking the active chain: if any
//   reachable slot is FREE, it's real corruption; otherwise the publish
//   was just outraced.
//
// Branch IDs:
//   1 = mark-prev help
//   2 = dead-prev splice
//   3 = mark-curr help
//   4 = target case
//   5 = c_dead opp-splice
//
// Macros default to no-op so production callers pay zero overhead. The
// crystalline_slot_pool_stress harness defines them before including the
// substrate (transitively via crystalline_slot_pool.h) to plumb diagnostic
// dump + trap on stale publish.
#ifndef LFL_TRACE_BRANCH
#define LFL_TRACE_BRANCH(branch_id, parent, prev_link_raw, curr,                \
                          c_link_raw, c_next, spliced)                          \
  ((void)0)
#endif
#ifndef LFL_VALIDATE_PUBLISH
#define LFL_VALIDATE_PUBLISH(branch_id, parent, c_next, post_link_raw)         \
  ((void)0)
#endif

// LFL_RETRY_OVERFLOW: called when harris_unlink's outer Retry loop
// exceeds LFL_RETRY_LIMIT iterations on a single target. Catches
// livelock at the moment a walker stops making progress — the
// substrate's failure mode under the bug we're hunting (S3 hangs).
//
// Production default: LFL_RETRY_LIMIT is large enough that legitimate
// contention never trips it; LFL_RETRY_OVERFLOW is no-op.
//
// Test (crystalline_slot_pool_stress): LFL_RETRY_LIMIT is small (e.g.
// 10000), LFL_RETRY_OVERFLOW dumps full ring + traps. The walker
// retries are bounded by concurrent activity (~thread count per
// resolution); 10K is well above that.
#ifndef LFL_RETRY_LIMIT
#define LFL_RETRY_LIMIT (1u << 30)
#endif
#ifndef LFL_RETRY_OVERFLOW
#define LFL_RETRY_OVERFLOW(target, retries) ((void)0)
#endif

namespace LIBC_NAMESPACE_DECL {
namespace linkage {

// ===== Layout =====
//
// Link layout: [state:8 | reserved:8 | tag:32 | next:16].
//   next  16 bits — chain index width (consumer-defined NULL sentinel).
//   tag   32-bit per-link counter, bumped on every write. T1.
//   state Caller-defined uint8_t lifecycle byte. Folded into the
//         link so the Harris walker's mid-splice CAS naturally fails
//         if the target's state was concurrently mutated (a state
//         CAS is itself a write to link). Without the fold, walker
//         could CAS successfully against a state-changed prev and
//         splice a node out from under the live chain.
// Walker CAS on prev.link reads the full 64 bits as expected; any
// mutation to state/tag/next fails it.
inline constexpr uint64_t LINK_NEXT_MASK = 0xFFFFull;
inline constexpr uint64_t LINK_TAG_MASK = 0xFFFFFFFFull;
inline constexpr uint64_t LINK_STATE_MASK = 0xFFull;
inline constexpr int LINK_TAG_SHIFT = 16;
inline constexpr int LINK_STATE_SHIFT = 56;

// ===== LINK_MARK_BIT (reserved bit 48) =====
//
// Load-bearing for lock-free mid-chain splice. The Harris walker has
// a two-word problem: it CASes parent.link using a `next` captured
// from node.link. Without a mark, a concurrent op-splice of the
// node's successor between the node.link read and the parent.link
// CAS leaves us CASing parent.next to a stale node — chain
// disconnect, double-reclaim, or IDLE-on-reachable-chain.
//
// Walker protocol (mark-then-help, robust to splicer death):
//   1. CAS-set MARK on node.link (expected unmarked, tag bumped).
//   2. Read node.next from the marked link — guaranteed stable
//      because any other CAS fails on tag or mark mismatch.
//   3. CAS parent.link with node.next as desired.next.
//   4a. Success ⇒ node off-chain. Finalize CAS (marked → CERT+!MARK).
//   4b. Failure ⇒ LEAVE the mark. Splice-fail does not own the
//       mark — the original splicer (if alive) needs it for their
//       own finalize, and clearing it under contention is the
//       dominant livelock vector at N-way concurrent splice on a
//       shared chain. Caller restarts; a future walk re-observes
//       the mark and either we or another walker eventually wins
//       a help-success and finalizes (which clears MARK + sets
//       CERT in one strong CAS). Self-mark callers (target case
//       and dead-intermediate opp-splice) likewise leave their
//       mark and reuse it on retry via `is_marked()` detection
//       at the call site.
//
// HELP-NOT-WAIT on observed MARK: a walker observing MARK=1 on some
// other node.link does NOT spin waiting for the marker to clear. It
// uses the observed marked link as a `MarkedLinkSnap` (via
// `MarkedLinkSnap::from_observed_marked`) and re-runs steps 3-4
// itself. Strong CAS at both step 3 (parent) and step 4 (finalize)
// serializes original-marker vs helper races: each CAS has at most
// one winner, the loser bails harmlessly.
//
// Dead-splicer recovery (when the marker thread is killed mid-
// protocol): the mark persists, but every future walk through the
// chain encounters it and attempts help. Once concurrent activity
// quiesces (chain stabilizes), some walker's help-success wins the
// parent CAS and the finalize clears the mark. Bounded by chain
// stabilization, not by any global retry budget.
//
// Waker/owner primitives MUST preserve the mark bit (link_cas_snap,
// link_cas_state, link_exchange_state, link_cas_state_detached all
// OR the expected's mark into desired via LINK_PRESERVE_MASK), or a
// walker mid-splice would silently unfreeze.
inline constexpr uint64_t LINK_MARK_BIT = 1ULL << 48;

// ===== LINK_CERT_BIT (reserved bit 49) =====
//
// Structural fold of the invariant
//   "consumer's terminal write is permitted only on a node proven
//    off-chain"
// into a single atomic bit.
//
// LOAD-BEARING DIRECTION: CERT==1 ⇒ node off-chain. CERT==0 has the
// weaker "not certified" semantics — NEVER code against
// CERT==0 ⇒ on-chain.
//
// Set by every operation that proves off-chain at the moment of set
// (e.g. waker upgrade ORPHAN→CLEAN, walker splice success,
// stack-steal CAS-with-snap, reclaim's IDLE write, freelist
// push / fresh page, push bail re-publish).
//
// Cleared by exactly one operation: the helper that publishes a
// node's transition onto a chain (see `with_next_uncertify` /
// `link_store_next_uncertify`). On-chain residency truthfully reads
// CERT=0.
//
// All other writers PRESERVE CERT via LINK_PRESERVE_MASK so the
// bit's semantics never silently flip during state transitions.
inline constexpr uint64_t LINK_CERT_BIT = 1ULL << 49;

// ===== LINK_ALERT_FIRED_BIT (reserved bit 50) =====
//
// Structural fold of the invariant
//   "the wake CAS issued (or will issue) an unconditional alert
//    against the node's owner thread"
// into a single atomic bit, atomic with the state transition that
// commits the wake.
//
// Set by every CAS whose `Traits::fires_alert(from, to)` returns
// true (in wait_slot's protocol: pre-marks whose snap state was
// `IN_KERNEL`). NOT set by transitions whose snap state has the
// wake caught by an in-process spin without a syscall, so no alert
// is ever issued.
//
// Cleared by every transition that resets the node's wake-cycle
// identity (rewrite_certified, with_state_certified,
// with_next_uncertify). All other writers PRESERVE ALERT_FIRED via
// LINK_PRESERVE_MASK.
//
// Read by SIGNALED-observation sites that want to publish "alert in
// flight" to the consumer's thread-local late-alert handling logic.
inline constexpr uint64_t LINK_ALERT_FIRED_BIT = 1ULL << 50;

// Reserved bits CAS/XCHG helpers must OR-preserve from expected
// into desired. One mask keeps the discipline uniform — adding a
// new reserved bit means updating this and the new bit's
// set/clear sites only.
inline constexpr uint64_t LINK_PRESERVE_MASK =
    LINK_MARK_BIT | LINK_CERT_BIT | LINK_ALERT_FIRED_BIT;

// Layout guard: the three reserved bits must not overlap each
// other or the state/tag/next fields. Cheap structural check that
// locks the bit positions — adding a fourth reserved bit means
// extending these asserts as well.
static_assert(((LINK_MARK_BIT | LINK_CERT_BIT | LINK_ALERT_FIRED_BIT) &
               ((LINK_STATE_MASK << LINK_STATE_SHIFT) |
                (LINK_TAG_MASK << LINK_TAG_SHIFT) | LINK_NEXT_MASK)) == 0,
              "LINK_{MARK,CERT,ALERT_FIRED}_BIT must not overlap "
              "state/tag/next fields");
static_assert((LINK_MARK_BIT & LINK_CERT_BIT) == 0 &&
                  (LINK_MARK_BIT & LINK_ALERT_FIRED_BIT) == 0 &&
                  (LINK_CERT_BIT & LINK_ALERT_FIRED_BIT) == 0,
              "LINK_{MARK,CERT,ALERT_FIRED}_BIT must be mutually "
              "exclusive reserved bits");

// ===== Link: typed packed-word value =====
//
// Typed wrapper over the 64-bit packed word. Each wither returns a
// new Link with tag bumped, so "tag bumped on every link write" (T1)
// is a type-level invariant — the uint64_t-taking ctor is private;
// the only constructors are static factories (fresh, no history)
// and withers (tag bumped). Layout-identical to uint64_t;
// cpp::Atomic<Link> generates the same assembly as
// cpp::Atomic<uint64_t>.
//
// `from_raw` / `.raw()` exist only for the few sites that need
// round-trip integer serialization (debug instrumentation,
// link_field_set_cert's __atomic_fetch_or on the underlying word).
//
// Tag-bump policy: each wither bumps tag exactly ONCE. Chaining
// `with_state(X).certified()` is correct but wasteful — two bumps
// halve the 32-bit per-node wraparound distance for no benefit.
// The compound withers below cover documented two-step patterns.
//
// State arguments throughout are plain uint8_t. Consumers using a
// scoped enum should `static_cast<uint8_t>` at the call site;
// unscoped enums (preferred) implicitly convert.
//
// Four wither families:
//   Pure                 with_state, with_next, marked, unmarked,
//                        certified, uncertified — preserve every
//                        other field (MARK, CERT, ALERT_FIRED
//                        included).
//   Owner-exclusive      rewrite_certified, with_state_drop_mark,
//                        with_next_drop_mark, with_next_uncertify,
//                        with_state_certified — DROP MARK by
//                        invariant. Caller is owner-exclusive on an
//                        off-chain node; carrying a stale MARK
//                        could freeze a future walker on wrong
//                        data. The drop is structural — lives in
//                        the wither. rewrite_certified,
//                        with_state_certified, and
//                        with_next_uncertify ALSO drop ALERT_FIRED
//                        — these are the node's wake-cycle reset
//                        points (freelist push, terminal publish,
//                        push commit). All other "drop MARK"
//                        withers preserve ALERT_FIRED.
//   Compound (1 bump)    with_state_set_cert, with_cert_unmark,
//                        with_state_alerting,
//                        with_state_set_cert_alerting,
//                        with_state_and_next — replace chained
//                        two-step writers at the detach-prover,
//                        splice-finalize, alerting-pre-mark,
//                        stack-steal-alerting-pre-mark, and
//                        skiplist-Swap-relink sites.
//   Static factories     pack, pack_marked, pack_certified, from_raw
//                        — fresh construction at pool init / atomic-
//                        load boundaries.
class Link {
public:
  // Default-constructs to value 0 (state=0/tag=0/next=0/no MARK/no
  // CERT/no ALERT_FIRED). Required for trivial-copyability and
  // cpp::Atomic<Link>.
  LIBC_INLINE constexpr Link() noexcept = default;

  // ----- Decoders -----
  LIBC_INLINE constexpr uint64_t raw() const { return value_; }
  LIBC_INLINE constexpr uint8_t state() const {
    return static_cast<uint8_t>((value_ >> LINK_STATE_SHIFT) & LINK_STATE_MASK);
  }
  LIBC_INLINE constexpr uint32_t tag() const {
    return static_cast<uint32_t>((value_ >> LINK_TAG_SHIFT) & LINK_TAG_MASK);
  }
  LIBC_INLINE constexpr uint16_t next() const {
    return static_cast<uint16_t>(value_ & LINK_NEXT_MASK);
  }
  LIBC_INLINE constexpr bool is_marked() const {
    return (value_ & LINK_MARK_BIT) != 0;
  }
  LIBC_INLINE constexpr bool is_certified() const {
    return (value_ & LINK_CERT_BIT) != 0;
  }
  LIBC_INLINE constexpr bool is_alert_fired() const {
    return (value_ & LINK_ALERT_FIRED_BIT) != 0;
  }

  // ----- Pure withers (preserve all others, bump tag) -----

  LIBC_INLINE constexpr Link with_state(uint8_t s) const {
    uint64_t v = (value_ & ~kStateField) |
                 (static_cast<uint64_t>(s) << LINK_STATE_SHIFT);
    return Link{bumped_tag(v)};
  }
  LIBC_INLINE constexpr Link with_next(uint16_t n) const {
    uint64_t v = (value_ & ~kNextField) |
                 (static_cast<uint64_t>(n) & LINK_NEXT_MASK);
    return Link{bumped_tag(v)};
  }
  LIBC_INLINE constexpr Link marked() const {
    return Link{bumped_tag(value_ | LINK_MARK_BIT)};
  }
  LIBC_INLINE constexpr Link unmarked() const {
    return Link{bumped_tag(value_ & ~LINK_MARK_BIT)};
  }
  LIBC_INLINE constexpr Link certified() const {
    return Link{bumped_tag(value_ | LINK_CERT_BIT)};
  }
  LIBC_INLINE constexpr Link uncertified() const {
    return Link{bumped_tag(value_ & ~LINK_CERT_BIT)};
  }

  // ----- Owner-exclusive rewrites (drop MARK by invariant) -----
  // Caller is owner-exclusive on an off-chain node (freelist,
  // pre-publish, or under data-structure lock). MARK=1 in the snap
  // would imply a walker mid-splice — impossible here. The drop is
  // structural: a stale MARK carried forward would freeze a future
  // walker mid-splice on wrong data.

  // state + next + CERT, drop MARK + ALERT_FIRED. Used by
  // freelist-push, fresh-page init, fork_reinit, post-pop alloc
  // init. ALERT_FIRED drops because this is a wake-cycle reset —
  // the node is leaving the prior cycle's identity (freelist
  // re-entry or fresh owner alloc); a stale ALERT_FIRED would
  // falsely advertise that the next pre-mark already fired an
  // alert.
  LIBC_INLINE constexpr Link rewrite_certified(uint8_t s, uint16_t n) const {
    uint64_t v = (value_ & ~(kStateField | kNextField | LINK_MARK_BIT |
                              LINK_ALERT_FIRED_BIT)) |
                 (static_cast<uint64_t>(s) << LINK_STATE_SHIFT) |
                 (static_cast<uint64_t>(n) & LINK_NEXT_MASK) | LINK_CERT_BIT;
    return Link{bumped_tag(v)};
  }
  // state, preserve next + CERT, drop MARK. Used by entry
  // transition (off-chain owner-exclusive paths where CERT must
  // survive into the live-not-yet-pushed window) and terminal-
  // publish slow paths (dropping a late MARK is harmless).
  LIBC_INLINE constexpr Link with_state_drop_mark(uint8_t s) const {
    uint64_t v = (value_ & ~(kStateField | LINK_MARK_BIT)) |
                 (static_cast<uint64_t>(s) << LINK_STATE_SHIFT);
    return Link{bumped_tag(v)};
  }
  // next, preserve state + CERT, drop MARK. SLL splice / freelist
  // tail splice — node on its chain via lock or freelist invariant.
  LIBC_INLINE constexpr Link with_next_drop_mark(uint16_t n) const {
    uint64_t v = (value_ & ~(kNextField | LINK_MARK_BIT)) |
                 (static_cast<uint64_t>(n) & LINK_NEXT_MASK);
    return Link{bumped_tag(v)};
  }
  // next, preserve state, drop CERT, MARK, and ALERT_FIRED. Push
  // commit — node is about to commit on-chain for a fresh wait
  // cycle, so the off-chain certificate is revoked and any prior
  // cycle's alert-fired residue is cleared. Also used by mid-splice
  // CASes — those splice off-chain dead/orphan nodes whose
  // ALERT_FIRED is owner-irrelevant past splice (owner's exit drops
  // the bit via terminal publish anyway).
  LIBC_INLINE constexpr Link with_next_uncertify(uint16_t n) const {
    uint64_t v = (value_ & ~(kNextField | LINK_MARK_BIT | LINK_CERT_BIT |
                              LINK_ALERT_FIRED_BIT)) |
                 (static_cast<uint64_t>(n) & LINK_NEXT_MASK);
    return Link{bumped_tag(v)};
  }
  // state, preserve next, set CERT, drop MARK + ALERT_FIRED.
  // Terminal publish (fast and slow paths). ALERT_FIRED drops
  // because the wake has been consumed: the owner observed
  // SIGNALED, set its expect-late-alert flag if the bit was 1, and
  // is now publishing the node's terminal state ready for a fresh
  // cycle.
  LIBC_INLINE constexpr Link with_state_certified(uint8_t s) const {
    uint64_t v = (value_ & ~(kStateField | LINK_MARK_BIT |
                              LINK_ALERT_FIRED_BIT)) |
                 (static_cast<uint64_t>(s) << LINK_STATE_SHIFT) | LINK_CERT_BIT;
    return Link{bumped_tag(v)};
  }

  // ----- Single-bump compound withers -----

  // state + CERT, preserve next AND MARK AND ALERT_FIRED, one bump.
  // Detach-prover helpers (link_cas_state_certify,
  // link_cas_state_detached, link_exchange_state_certify) need the
  // state transition and CERT publish atomic in the same word. MARK
  // preservation is load-bearing on stack-steal: a stolen chain may
  // contain a node a Harris walker marked just before the steal —
  // stripping would silently release the walker's freeze. Distinct
  // from with_state_certified, which DROPS MARK + ALERT_FIRED
  // (terminal publishers).
  LIBC_INLINE constexpr Link with_state_set_cert(uint8_t s) const {
    uint64_t v = (value_ & ~kStateField) |
                 (static_cast<uint64_t>(s) << LINK_STATE_SHIFT) | LINK_CERT_BIT;
    return Link{bumped_tag(v)};
  }

  // Clear MARK + set CERT, preserve state and next, one bump.
  // link_finalize_after_splice's terminal CAS — atomically releases
  // the walker's splice freeze and publishes the off-chain cert.
  LIBC_INLINE constexpr Link with_cert_unmark() const {
    uint64_t v = (value_ & ~LINK_MARK_BIT) | LINK_CERT_BIT;
    return Link{bumped_tag(v)};
  }

  // state + next, preserve MARK + CERT + ALERT_FIRED, one bump.
  // Used by interval-skiplist Swap to publish atomic LOCKED→LIVE
  // state transition simultaneously with the new successor pointer
  // (Kim et al. SOSP 2025 §4.2 linearisation point on level-0
  // predecessor's `next[0]`). Without compound write, the predecessor
  // would expose either (LOCKED + new next) or (LIVE + old next)
  // mid-Swap — the first leaks the in-progress lock to readers, the
  // second resurrects the old chain. Compound publishes both fields in
  // one CAS so the linearisation is observably atomic.
  //
  // MARK preservation: a Harris walker may have marked the
  // predecessor mid-Swap (cleanup of an earlier orphaned chain
  // segment); the compound wither carries the mark forward so the
  // walker's freeze remains intact across the relink. CERT preservation:
  // skiplist predecessors are on-chain (CERT=0), so no-op in practice;
  // hygiene against hypothetical caller misuse. ALERT_FIRED
  // preservation: set on a prior pre-Lock CAS that fired
  // NtAlertThreadByThreadId on a parker; the bit must survive into
  // the Swap so the parker's post-park observation classifies as
  // alert-bearing.
  LIBC_INLINE constexpr Link with_state_and_next(uint8_t s,
                                                  uint16_t n) const {
    uint64_t v = (value_ & ~(kStateField | kNextField)) |
                 (static_cast<uint64_t>(s) << LINK_STATE_SHIFT) |
                 (static_cast<uint64_t>(n) & LINK_NEXT_MASK);
    return Link{bumped_tag(v)};
  }

  // state + ALERT_FIRED, preserve next + MARK + CERT, one bump.
  // Pre-mark CAS variant for transitions whose `Traits::fires_alert`
  // returns true — publishes "alert is in flight against this
  // node's owner" in the same atomic word that commits the wake.
  // The waiter's SIGNALED-observation site reads is_alert_fired()
  // in the same load that drove its state() dispatch; structural
  // distinguisher for "should I set expect_late_alert?" without a
  // per-thread counter.
  //
  // MARK preservation: walker may have marked us mid-splice just
  // before our pre-mark CAS captured the snap; the wither carries
  // the mark forward so the walker's freeze remains intact.
  // CERT preservation: hygiene — pre-mark targets on-chain nodes
  // (CERT=0 by invariant), so this is a no-op in practice.
  LIBC_INLINE constexpr Link with_state_alerting(uint8_t s) const {
    uint64_t v = (value_ & ~kStateField) |
                 (static_cast<uint64_t>(s) << LINK_STATE_SHIFT) |
                 LINK_ALERT_FIRED_BIT;
    return Link{bumped_tag(v)};
  }

  // state + CERT + ALERT_FIRED, preserve next + MARK, one bump.
  // Stack-steal pre-mark variant for the alerting-stealee branch
  // (link_cas_state_detached's `fires_alert(snap.state())` true
  // path). Combines detach-prover CERT publish with alert-in-flight
  // publish in a single wither — keeps the bit set inside the
  // typed wither family rather than via a raw OR at the call site.
  //
  // MARK preservation: same rationale as with_state_set_cert — a
  // stolen chain may contain a node a Harris walker marked just
  // before the steal; stripping would silently release the freeze.
  // CERT publish: stack-steal proves every stolen node off the live
  // chain. ALERT_FIRED publish: the alerting stealee gets an
  // unconditional alert from the batched alert syscall, so the
  // owner's post-park SIGNALED observation must classify as
  // alert-bearing.
  LIBC_INLINE constexpr Link with_state_set_cert_alerting(uint8_t s) const {
    uint64_t v = (value_ & ~kStateField) |
                 (static_cast<uint64_t>(s) << LINK_STATE_SHIFT) | LINK_CERT_BIT |
                 LINK_ALERT_FIRED_BIT;
    return Link{bumped_tag(v)};
  }

  // ----- Static factories -----
  // Fresh construction (no tag bump). Use only at pool init,
  // freelist init, and atomic-load boundaries.

  LIBC_INLINE static constexpr Link from_raw(uint64_t v) { return Link{v}; }
  LIBC_INLINE static constexpr Link pack(uint8_t state, uint32_t tag,
                                          uint16_t next) {
    return Link{(static_cast<uint64_t>(state) << LINK_STATE_SHIFT) |
                (static_cast<uint64_t>(tag) << LINK_TAG_SHIFT) |
                (static_cast<uint64_t>(next) & LINK_NEXT_MASK)};
  }
  LIBC_INLINE static constexpr Link pack_marked(uint8_t state, uint32_t tag,
                                                 uint16_t next) {
    return Link{pack(state, tag, next).value_ | LINK_MARK_BIT};
  }
  LIBC_INLINE static constexpr Link pack_certified(uint8_t state, uint32_t tag,
                                                    uint16_t next) {
    return Link{pack(state, tag, next).value_ | LINK_CERT_BIT};
  }

private:
  // Field masks pre-shifted into their final word position.
  static constexpr uint64_t kStateField = LINK_STATE_MASK << LINK_STATE_SHIFT;
  static constexpr uint64_t kTagField = LINK_TAG_MASK << LINK_TAG_SHIFT;
  static constexpr uint64_t kNextField = LINK_NEXT_MASK;

  // Increment tag inside v, preserving all other fields. Wraps at
  // 2^32, which is the per-node ABA-defense window — see Link
  // header for the wrap-distance derivation.
  LIBC_INLINE static constexpr uint64_t bumped_tag(uint64_t v) {
    uint64_t old_tag = (v >> LINK_TAG_SHIFT) & LINK_TAG_MASK;
    uint64_t new_tag = (old_tag + 1) & LINK_TAG_MASK;
    return (v & ~kTagField) | (new_tag << LINK_TAG_SHIFT);
  }

  LIBC_INLINE constexpr explicit Link(uint64_t v) : value_(v) {}
  // Default member initializer is required for the defaulted
  // constexpr Link() above to be valid before C++23 (otherwise
  // value_ would be left uninitialized in a constexpr context,
  // and the defaulted ctor cannot be marked constexpr). 0 matches
  // the documented "value 0 (state=0, tag=0, next=0, no MARK,
  // no CERT, no ALERT_FIRED)" semantics of the default ctor.
  uint64_t value_{};

  // link_field_set_cert routes through __atomic_fetch_or on the
  // layout-compatible underlying word; needs friend access to
  // value_ for the reinterpret_cast.
  friend LIBC_INLINE void link_field_set_cert(cpp::Atomic<Link> &,
                                               cpp::MemoryOrder);
};

static_assert(sizeof(Link) == sizeof(uint64_t),
              "Link must be a thin wrapper over uint64_t — codegen "
              "neutrality depends on the sizes matching exactly");

// ----- Free-function decoder shorthand accepting Link -----
//
// Some callers prefer the verb-first read form `link_next(snap)`
// over the noun-method form `snap.next()`. Both compile to the
// same code; these are pure thin wrappers around the Link
// accessors. Provided only for the read-side decoders — packers
// and writers go exclusively through Link's withers / static
// factories so tag monotonicity stays a structural invariant.
LIBC_INLINE constexpr uint8_t link_state(Link l) { return l.state(); }
LIBC_INLINE constexpr uint32_t link_tag(Link l) { return l.tag(); }
LIBC_INLINE constexpr uint16_t link_next(Link l) { return l.next(); }
LIBC_INLINE constexpr bool link_is_marked(Link l) { return l.is_marked(); }
LIBC_INLINE constexpr bool link_is_certified(Link l) {
  return l.is_certified();
}
LIBC_INLINE constexpr bool link_is_alert_fired(Link l) {
  return l.is_alert_fired();
}

// Read just the state byte. Equivalent to .load(ord).state() but
// the named helper makes intent visible at call sites.
LIBC_INLINE uint8_t
link_load_state(cpp::Atomic<Link> &link_field,
                cpp::MemoryOrder ord = cpp::MemoryOrder::ACQUIRE) {
  return link_field.load(ord).state();
}

// ===========================================================================
// State-aware CAS Traits policy
// ===========================================================================
//
// State-aware helpers take a `Traits` policy with two compile-time hooks:
//
//   static constexpr bool is_valid(uint8_t from, uint8_t to);
//     Catches transitions the state machine considers illegal at
//     static_assert time. `DefaultStateTraits` accepts every
//     transition.
//
//   static constexpr bool fires_alert(uint8_t from, uint8_t to);
//     When true, the CAS sets LINK_ALERT_FIRED_BIT atomically with
//     the state transition (selects with_state_alerting /
//     with_state_set_cert_alerting). `DefaultStateTraits` never
//     fires.
//
// Both hooks must be `static constexpr` so `static_assert` /
// `if constexpr` evaluate at compile time — that is the codegen
// neutrality contract for substrate consumption.
struct DefaultStateTraits {
  static constexpr bool is_valid(uint8_t /*from*/, uint8_t /*to*/) {
    return true;
  }
  static constexpr bool fires_alert(uint8_t /*from*/, uint8_t /*to*/) {
    return false;
  }
};

// ===========================================================================
// Owner-exclusive (non-CAS) writers
// ===========================================================================
//
// Safe ONLY under exclusive-writer discipline: caller is owner of
// the node, target is off-chain (freelist, pre-publish, or under
// data-structure lock). DO NOT call where other threads may CAS
// the node concurrently — use the link_cas_* family.

// Sets state, next, AND CERT (rewrite_certified). Targets off-chain
// nodes — freelist push, freshly-popped node init, init/fork
// reinit. MARK + ALERT_FIRED drop because exclusive-writer +
// off-chain ⇒ no walker mid-splice and we are at a wake-cycle
// reset.
LIBC_INLINE void link_store(cpp::Atomic<Link> &link_field, uint8_t new_state,
                             uint16_t new_next,
                             cpp::MemoryOrder store_ord = cpp::MemoryOrder::RELAXED) {
  Link old = link_field.load(cpp::MemoryOrder::RELAXED);
  link_field.store(old.rewrite_certified(new_state, new_next), store_ord);
}

// Rewrite next, preserve state and CERT, drop MARK. Used by
// freelist-tail splice — node is on the freelist, off any wait
// chain, no concurrent writer.
//
// For push commit (node transitions on-chain, CERT must clear),
// use link_store_next_uncertify below.
//
// DO NOT call this on chain entries while their owner might be
// CAS-ing state concurrently — the load-modify-store races with
// the owner's state CAS and can silently roll back the transition.
// Use link_cas_next instead.
LIBC_INLINE void link_store_next(cpp::Atomic<Link> &link_field,
                                  uint16_t new_next,
                                  cpp::MemoryOrder store_ord = cpp::MemoryOrder::RELAXED) {
  Link old = link_field.load(cpp::MemoryOrder::RELAXED);
  link_field.store(old.with_next_drop_mark(new_next), store_ord);
}

// CAS-loop variant of link_store_next: rewrite .next, preserve
// state/CERT/ALERT_FIRED, drop MARK, bump tag — but as an atomic
// compare-exchange that retries on any concurrent mutation.
//
// Used by chain ops (insert / remove / wake's inline cleanup)
// when writing the .next field of an existing chain entry whose
// owner can concurrently CAS its state. The non-atomic store-back
// of link_store_next would clobber the owner's state CAS on a
// load-then-CAS-then-store interleave — the owner's transition
// would silently regress and any subsequent waker would observe
// the stale state, mis-route the wake protocol, and deadlock the
// owner. The CAS loop preserves the owner's state transition by
// retrying against the post-CAS link.
LIBC_INLINE void link_cas_next(cpp::Atomic<Link> &link_field,
                                uint16_t new_next,
                                cpp::MemoryOrder success_ord = cpp::MemoryOrder::RELEASE) {
  for (;;) {
    Link old = link_field.load(cpp::MemoryOrder::RELAXED);
    if (link_field.compare_exchange_weak(old, old.with_next_drop_mark(new_next),
                                          success_ord,
                                          cpp::MemoryOrder::RELAXED))
      return;
  }
}

// Push-commit variant: rewrite next, CLEAR CERT and MARK and
// ALERT_FIRED. Semantic: "node is about to commit on-chain via the
// subsequent CAS publish; revoke any prior off-chain cert".
// Post-CAS: on-chain with CERT=0 (truthful). If the push loop
// bails, the bail site re-asserts CERT via fetch_or before the
// terminal publish consumes.
//
// MARK drops because push entry is owner-exclusive on an off-chain
// node. link_finalize_after_splice never leaves MARK=1 on success,
// so any MARK observed pre-load is stale and would freeze a future
// walker on wrong data.
LIBC_INLINE void link_store_next_uncertify(cpp::Atomic<Link> &link_field,
                                            uint16_t new_next,
                                            cpp::MemoryOrder store_ord = cpp::MemoryOrder::RELAXED) {
  Link old = link_field.load(cpp::MemoryOrder::RELAXED);
  link_field.store(old.with_next_uncertify(new_next), store_ord);
}

// Owner-exclusive variant — caller threads the current Link
// through hot-path iterations (push loop) so the per-iteration load
// is elided. Node is owner-exclusive between the initial load and
// the final publish CAS; no concurrent writer can mutate
// node.link, so the local Link tracks ground truth. Returns the
// new Link so the caller can chain into the next iteration.
LIBC_INLINE Link
link_store_next_uncertify_known(cpp::Atomic<Link> &link_field, Link current,
                                 uint16_t new_next,
                                 cpp::MemoryOrder store_ord = cpp::MemoryOrder::RELAXED) {
  Link n = current.with_next_uncertify(new_next);
  link_field.store(n, store_ord);
  return n;
}

// Rewrite state, preserve next + CERT, drop MARK. Entry transition
// (off-chain owner-exclusive paths where CERT must survive into the
// live-not-yet-pushed window). MARK drop: same rationale as
// link_store_next_uncertify above.
LIBC_INLINE void link_store_state(cpp::Atomic<Link> &link_field,
                                   uint8_t new_state,
                                   cpp::MemoryOrder store_ord = cpp::MemoryOrder::RELAXED) {
  Link old = link_field.load(cpp::MemoryOrder::RELAXED);
  link_field.store(old.with_state_drop_mark(new_state), store_ord);
}

// Owner-exclusive variant — same rationale as
// link_store_next_uncertify_known. Caller seeds the loop with one
// initial RELAXED load of node.link, then threads the returned
// Link through entry setup + push. Each subsequent store is a
// wither + 8-byte store, no atomic load.
LIBC_INLINE Link
link_store_state_known(cpp::Atomic<Link> &link_field, Link current,
                        uint8_t new_state,
                        cpp::MemoryOrder store_ord = cpp::MemoryOrder::RELAXED) {
  Link n = current.with_state_drop_mark(new_state);
  link_field.store(n, store_ord);
  return n;
}

// ===========================================================================
// State-aware CAS family (Traits-parameterized)
// ===========================================================================

// Atomic From → To, preserve next/MARK/CERT, bump tag. CAS loop so
// benign concurrent writes (e.g. opp-splice on .next) don't
// spuriously fail us — only a state transition by another actor
// does.
//
// State transitions don't imply chain-membership change; CERT is
// changed only by link_store_next_uncertify (clears) and the
// dedicated detach-prover sites (sets).
//
// ALERT_FIRED publish: when `Traits::fires_alert(From, To)` is
// true, with_state_alerting publishes the bit atomically with the
// state transition. The compile-time `if constexpr` selects the
// alerting wither in that exact case so the bit publishes
// atomically with the state transition.
//
// `static_assert` on (From, To) catches transitions not in
// `Traits::is_valid` at compile time — zero runtime cost vs the
// runtime-uint8 variant (link_exchange_state).
template <auto From, auto To, class Traits = DefaultStateTraits>
LIBC_INLINE bool
link_cas_state(cpp::Atomic<Link> &link_field,
               cpp::MemoryOrder success = cpp::MemoryOrder::ACQ_REL,
               cpp::MemoryOrder failure = cpp::MemoryOrder::ACQUIRE) {
  static_assert(Traits::is_valid(static_cast<uint8_t>(From),
                                  static_cast<uint8_t>(To)),
                "link_cas_state: transition rejected by Traits::is_valid");
  for (;;) {
    Link old = link_field.load(cpp::MemoryOrder::ACQUIRE);
    if (old.state() != static_cast<uint8_t>(From))
      return false;
    Link desired;
    if constexpr (Traits::fires_alert(static_cast<uint8_t>(From),
                                       static_cast<uint8_t>(To))) {
      desired = old.with_state_alerting(static_cast<uint8_t>(To));
    } else {
      desired = old.with_state(static_cast<uint8_t>(To));
    }
    if (link_field.compare_exchange_weak(old, desired, success, failure))
      return true;
  }
}

// Atomic From → To that ALSO sets CERT in the result. Used where
// the caller has proven detach (waker upgrade ORPHAN→CLEAN /
// HANDOFF_ORPHAN→HANDOFF_CLEAN; drain_stale_top fallback). Folds
// state + CERT publish into one atomic so the consumer cannot
// observe CLEAN-without-CERT (which would fail the terminal
// publish's precondition).
template <auto From, auto To, class Traits = DefaultStateTraits>
LIBC_INLINE bool
link_cas_state_certify(cpp::Atomic<Link> &link_field,
                        cpp::MemoryOrder success = cpp::MemoryOrder::ACQ_REL,
                        cpp::MemoryOrder failure = cpp::MemoryOrder::ACQUIRE) {
  static_assert(Traits::is_valid(static_cast<uint8_t>(From),
                                  static_cast<uint8_t>(To)),
                "link_cas_state_certify: transition rejected by "
                "Traits::is_valid");
  for (;;) {
    Link old = link_field.load(cpp::MemoryOrder::ACQUIRE);
    if (old.state() != static_cast<uint8_t>(From))
      return false;
    // with_state_set_cert: state + CERT atomic, preserve MARK +
    // ALERT_FIRED, one tag bump (chained with_state(To).certified()
    // would bump twice).
    if (link_field.compare_exchange_weak(old, old.with_state_set_cert(
                                                  static_cast<uint8_t>(To)),
                                          success, failure))
      return true;
  }
}

// Unconditional state write, return old state. CAS loop so a
// concurrent non-state write (opp-splice on .next) doesn't clobber
// our next/tag invariants. Preserves next/MARK/CERT/ALERT_FIRED.
// Use link_exchange_state_certify for the reclaim variant that
// also sets CERT.
LIBC_INLINE uint8_t
link_exchange_state(cpp::Atomic<Link> &link_field, uint8_t new_state,
                    cpp::MemoryOrder ord = cpp::MemoryOrder::ACQ_REL) {
  for (;;) {
    Link old = link_field.load(cpp::MemoryOrder::ACQUIRE);
    Link desired = old.with_state(new_state);
    if (link_field.compare_exchange_weak(old, desired, ord,
                                          cpp::MemoryOrder::ACQUIRE))
      return old.state();
  }
}

// Reclaim's state-transition primitive. Atomic state=`new_state` +
// CERT publish (caller's contract: detach proven via the
// data-structure CAS preceding reclaim). MARK preserved — a
// concurrent walker may hold a splice freeze; clearing it here
// would silently release that freeze on a node the walker hasn't
// finalized. Returns prior state (caller's idempotency guard:
// new_state ⇒ another reclaimer already won).
LIBC_INLINE uint8_t
link_exchange_state_certify(cpp::Atomic<Link> &link_field, uint8_t new_state,
                             cpp::MemoryOrder ord = cpp::MemoryOrder::ACQ_REL) {
  for (;;) {
    Link old = link_field.load(cpp::MemoryOrder::ACQUIRE);
    Link desired = old.with_state_set_cert(new_state);
    if (link_field.compare_exchange_weak(old, desired, ord,
                                          cpp::MemoryOrder::ACQUIRE))
      return old.state();
  }
}

// Stack-steal-only state transition: strong CAS against the
// caller's pre-detach snap (NOT an unconditional XCHG).
//
// The CAS-with-snap closes the silent-overwrite leak: between
// snap-load and this call, an unrelated waker's pre-mark + alert
// could have driven the owner through terminal publish. An
// unconditional XCHG would clobber owner's IDLE+CERT=1 with our
// SIGNALED — leaving the node SIGNALED with no chain residency,
// so owner can't reuse via TLS, allocs fresh, and orphans the
// original node indefinitely.
//
// Tag monotonicity (T1): writes link_tag(pre_detach_snap) + 1.
// Resetting to 0 would weaken the per-link ABA defense.
//
// ALERT_FIRED publish: runtime-selected via
// `Traits::fires_alert(snap.state(), To)`. Stack-steal of an
// alerting source state fires an unconditional alert at the
// captured tid (notify_all / drain_waiters per-node batched
// alert). Set the bit atomically with the steal-state CAS so the
// owner's post-park SIGNALED observation classifies as
// alert-bearing — same structural contract as link_cas_snap.
// Non-alerting source states leave ALERT_FIRED preserved (which is
// 0 by push-commit invariant) since cache-spin catches the wake
// without a syscall.
//
// Returns:
//   CAS success ⇒ ours_out=true, returns the OLD state.
//   CAS failure ⇒ ours_out=false, returns the CURRENT state.
//
// To-only template: stack-steal always targets a single fixed
// destination state. The from-state is enforced by the CAS match
// against the snap.
template <auto To, class Traits = DefaultStateTraits>
LIBC_INLINE uint8_t
link_cas_state_detached(cpp::Atomic<Link> &link_field, Link pre_detach_snap,
                         bool &cas_succeeded_out,
                         cpp::MemoryOrder ord = cpp::MemoryOrder::ACQ_REL) {
  // with_state_set_cert: state + CERT atomic (steal proves every
  // stolen node off the live chain); MARK preserved because a
  // stolen chain can contain a node a Harris walker marked just
  // before the steal, and stripping would silently release its
  // freeze; next preserved; one tag bump.
  Link desired = Traits::fires_alert(pre_detach_snap.state(),
                                       static_cast<uint8_t>(To))
                     ? pre_detach_snap.with_state_set_cert_alerting(
                           static_cast<uint8_t>(To))
                     : pre_detach_snap.with_state_set_cert(
                           static_cast<uint8_t>(To));
  Link expected = pre_detach_snap;
  if (link_field.compare_exchange_strong(expected, desired, ord,
                                          cpp::MemoryOrder::ACQUIRE)) {
    cas_succeeded_out = true;
    return pre_detach_snap.state();
  }
  // expected now holds the actual current link.
  cas_succeeded_out = false;
  return expected.state();
}

// Single-shot CAS on node.link from a captured full 64-bit
// snapshot to the desired state with tag+1 and `next` preserved.
// No retry loop — any concurrent mutation fails the CAS and the
// caller is expected to reload and retry (or branch on the
// observed state).
//
// Intent: the "pre-mark before detach" primitive for pop-and-signal
// / handoff. By writing node.link FIRST — bumping the tag and
// flipping state to the wake state before the CAS that physically
// detaches the head — any walker that captured this node as `prev`
// off the live chain finds its mid-splice CAS (which expects the
// pre-mark link value) naturally failing.
//
// To-only template: pre-mark always targets a single fixed wake-
// state destination family. The from-state lives in the dynamic
// snap and is enforced by the strong CAS — if snap's state doesn't
// match the actual link, the CAS fails.
//
// ALERT_FIRED publish: `Traits::fires_alert(snap.state(), To)` at
// runtime selects with_state_alerting in that exact case so the
// bit publishes atomically with the state transition.
template <auto To, class Traits = DefaultStateTraits>
LIBC_INLINE bool
link_cas_snap(cpp::Atomic<Link> &link_field, Link expected_snap,
              cpp::MemoryOrder success = cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder failure = cpp::MemoryOrder::ACQUIRE) {
  Link desired = Traits::fires_alert(expected_snap.state(),
                                       static_cast<uint8_t>(To))
                     ? expected_snap.with_state_alerting(
                           static_cast<uint8_t>(To))
                     : expected_snap.with_state(static_cast<uint8_t>(To));
  return link_field.compare_exchange_strong(expected_snap, desired, success,
                                             failure);
}

// Runtime-alerting variant of link_cas_snap. The alerting decision
// is supplied by the caller (rather than computed by Traits at
// compile time from the source state). Callers whose alert
// condition lives outside the Link word — e.g., Futex's notifier
// reading slot.park_state as a Dekker hint — use this entry point.
//
// Single-shot strong CAS, same shape as link_cas_snap; semantics
// identical except for the source of the ALERT_FIRED choice.
//
// Use cases:
//   Futex notifiers   pre-mark CAS WAITING → SIGNALED_* with
//                     `alerting = (slot.park_state == 1)`. Owner's
//                     SIGNALED-observation site sets
//                     expect_late_alert iff the bit is set.
template <auto To>
LIBC_INLINE bool
link_cas_snap_runtime_alert(cpp::Atomic<Link> &link_field, Link expected_snap,
                             bool alerting,
                             cpp::MemoryOrder success = cpp::MemoryOrder::ACQ_REL,
                             cpp::MemoryOrder failure = cpp::MemoryOrder::ACQUIRE) {
  Link desired = alerting
                     ? expected_snap.with_state_alerting(
                           static_cast<uint8_t>(To))
                     : expected_snap.with_state(static_cast<uint8_t>(To));
  return link_field.compare_exchange_strong(expected_snap, desired, success,
                                             failure);
}

// Runtime-alerting variant of link_cas_state_detached. Stack-steal
// pre-mark with the alert decision computed by the caller from a
// runtime hint (Futex broadcast paths read slot.park_state pre-CAS).
//
// CAS-with-snap, MARK preserved, CERT set on success, ALERT_FIRED
// set iff `alerting` is true. Returns OLD state on success,
// CURRENT state on failure (same shape as the Traits-based variant).
template <auto To>
LIBC_INLINE uint8_t
link_cas_state_detached_runtime_alert(cpp::Atomic<Link> &link_field,
                                       Link pre_detach_snap, bool alerting,
                                       bool &cas_succeeded_out,
                                       cpp::MemoryOrder ord = cpp::MemoryOrder::ACQ_REL) {
  Link desired = alerting
                     ? pre_detach_snap.with_state_set_cert_alerting(
                           static_cast<uint8_t>(To))
                     : pre_detach_snap.with_state_set_cert(
                           static_cast<uint8_t>(To));
  Link expected = pre_detach_snap;
  if (link_field.compare_exchange_strong(expected, desired, ord,
                                          cpp::MemoryOrder::ACQUIRE)) {
    cas_succeeded_out = true;
    return pre_detach_snap.state();
  }
  cas_succeeded_out = false;
  return expected.state();
}

// Single-shot strong CAS publishing state + next atomically from a
// captured snap. State transition To and a new next-pointer commit
// in one tag bump; MARK / CERT / ALERT_FIRED preserved.
//
// Use case: interval-skiplist Swap (Kim et al. SOSP 2025 Algorithm 2).
// The level-0 linearisation CAS on the predecessor's `next[0]`
// transitions the predecessor's state byte (e.g. LOCKED→LIVE) AND
// pivots the chain to point at the head of the freshly-published
// run of new nodes — both must be observably atomic, otherwise a
// concurrent Query would either traverse half-published state
// (state=LIVE but next still points at the locked old chain) or
// observe LOCKED on a node that has already structurally moved on.
//
// Single-shot, no retry loop: any concurrent mutation tag-bumps the
// snap, the CAS fails, and the Swap caller is expected to restart
// its Lock acquisition phase from the new predecessor (Kim et al.
// §4.3 — the swap is the linearisation point and must not race with
// any other writer; concurrent activity invalidates the locked-set
// proof). To-only template: Swap always targets a single fixed
// destination state. Non-noop state changes are checked through
// Traits::is_valid; same-state relinks are allowed for ordinary upper-level
// skiplist insertion.
template <auto To, class Traits = DefaultStateTraits>
LIBC_INLINE bool
link_cas_snap_relink(cpp::Atomic<Link> &link_field, Link expected_snap,
                     uint16_t new_next,
                     cpp::MemoryOrder success = cpp::MemoryOrder::ACQ_REL,
                     cpp::MemoryOrder failure = cpp::MemoryOrder::ACQUIRE) {
  const uint8_t to_state = static_cast<uint8_t>(To);
  if (expected_snap.state() != to_state &&
      !Traits::is_valid(expected_snap.state(), to_state))
    return false;
  Link desired = expected_snap.with_state_and_next(to_state, new_next);
  return link_field.compare_exchange_strong(expected_snap, desired, success,
                                             failure);
}

// ===== Walker mark/unmark primitives =====
//
// Used by Harris-style splice sites (op-splice, dead-prev-splice,
// target-splice) to freeze a node's .next field before CASing
// parent.link. See LINK_MARK_BIT doc for the full protocol.

// Set MARK. Expected must be unmarked. CAS fail ⇒ node.link was
// mutated (another marker, state transition, etc.); caller restarts.
LIBC_INLINE bool
link_cas_set_mark(cpp::Atomic<Link> &link_field, Link expected_unmarked,
                  cpp::MemoryOrder success = cpp::MemoryOrder::ACQ_REL,
                  cpp::MemoryOrder failure = cpp::MemoryOrder::ACQUIRE) {
  return link_field.compare_exchange_strong(
      expected_unmarked, expected_unmarked.marked(), success, failure);
}

// Typed wrapper for a post-mark link snap — the value
// link_cas_set_mark would have written on success. Constructible
// only via the two factories below.
//
// link_finalize_after_splice's correctness depends on the input
// carrying MARK=1 (it CASes from marked → MARK=0+CERT=1, expecting
// our mark in flight). The wrapper turns "input must be a marked
// snap" from a hand-written contract into a type-level invariant.
struct MarkedLinkSnap {
  Link link;

  // Helper-completion factory: a walker observed MARK=1 set by
  // SOME marker (possibly itself, possibly another walker, possibly
  // a walker that has since died) and wants to drive the splice's
  // finalize CAS without a self-issued link_cas_set_mark. The
  // observed marked link IS structurally identical to what the
  // marker's link_cas_set_mark wrote — it's the same atomic word.
  // is_marked() is a hard precondition: a non-marked input would
  // make link_finalize_after_splice race-misbehaving (it CASes
  // from marked → unmarked+CERT, and an unmarked expected would
  // risk certifying a node whose state has moved on).
  LIBC_INLINE static MarkedLinkSnap from_observed_marked(Link observed) {
    LIBC_ASSERT(observed.is_marked());
    return MarkedLinkSnap{observed};
  }

private:
  LIBC_INLINE constexpr explicit MarkedLinkSnap(Link l) : link(l) {}
  friend MarkedLinkSnap link_pack_after_mark(Link expected_unmarked);
};

// Compute the post-mark snap link_cas_set_mark would write for
// `expected_unmarked`. Walker call sites use this to feed
// link_finalize_after_splice WITHOUT re-loading node.link — a
// re-load under long preemption could sample a different
// lifecycle's state on a re-pushed node and certify it wrongly
// (the documented hazard the prior CAS-loop variant tripped).
LIBC_INLINE MarkedLinkSnap link_pack_after_mark(Link expected_unmarked) {
  return MarkedLinkSnap{expected_unmarked.marked()};
}

// Walker splice-success terminal: atomic CERT-set + MARK-clear,
// single strong CAS with BAIL-ON-FAIL (NO retry).
//
// CAS fail ⇒ another actor wrote past our snap (owner's terminal
// publish slow path, or owner cleared/reused/re-pushed and a new
// waker pre-marked). RETRYING WOULD RISK CERTIFYING A NODE NOW
// ON-CHAIN IN A NEW LIFECYCLE — the bug the prior loop tripped.
// Correctness is upheld by the alternate publisher.
LIBC_INLINE void
link_finalize_after_splice(cpp::Atomic<Link> &link_field,
                            MarkedLinkSnap expected_marked,
                            cpp::MemoryOrder ord = cpp::MemoryOrder::ACQ_REL) {
  // with_cert_unmark: clear MARK (release freeze) + set CERT
  // (publish certificate), preserve state/next, one tag bump.
  Link expected = expected_marked.link;
  (void)link_field.compare_exchange_strong(
      expected, expected.with_cert_unmark(), ord, cpp::MemoryOrder::RELAXED);
}

// Atomic LOCK-OR set of CERT (cpp::Atomic<T>::fetch_or is integral-
// only; Link routes via __atomic_fetch_or on the layout-compatible
// underlying word). Idempotent; preserves all other fields. DOES
// NOT bump tag — fetch_or is bit-set semantics, not a state
// transition observers respond to.
//
// Callers: push-bail (re-publish CERT after a prior
// link_store_next_uncertify cleared it); post-detach CERT publish
// proving detach.
LIBC_INLINE void
link_field_set_cert(cpp::Atomic<Link> &link_field,
                    cpp::MemoryOrder ord = cpp::MemoryOrder::RELEASE) {
  static_assert(sizeof(Link) == sizeof(uint64_t),
                "link_field_set_cert: Link must be layout-compatible "
                "with uint64_t for __atomic_fetch_or");
  __atomic_fetch_or(reinterpret_cast<uint64_t *>(&link_field.val),
                    LINK_CERT_BIT, static_cast<int>(ord));
}

// Clear MARK on splice-fail rollback. Loops past benign concurrent
// state transitions (which preserve MARK). Terminates because only
// the mark-owner walker writes MARK=0; exits early if MARK was
// cleared externally (reclaim's link_store, owner's terminal
// publish). Preserves CERT.
LIBC_INLINE void link_clear_mark(cpp::Atomic<Link> &link_field) {
  for (;;) {
    Link cur = link_field.load(cpp::MemoryOrder::ACQUIRE);
    if (!cur.is_marked())
      return;
    // .unmarked() clears MARK, preserves state/next/CERT, bumps tag.
    if (link_field.compare_exchange_weak(cur, cur.unmarked(),
                                          cpp::MemoryOrder::ACQ_REL,
                                          cpp::MemoryOrder::ACQUIRE))
      return;
  }
}

// ===========================================================================
// Harris walker — single canonical implementation for every consumer
// ===========================================================================
//
// `harris_unlink` / `harris_walk_attempt` codify the mark-then-help
// splice protocol that wait_slot, futex_addr, and crystalline_slot_pool
// independently re-derived. Consumers parameterise the walker with two
// policy types:
//
//   Ctx        — instance-level adapter for the consumer's pool.
//   DeadPolicy — compile-time classification of state bytes.
//
// Ctx contract (duck-typed, called as `ctx.foo(...)`):
//   cpp::Atomic<Link> &link_at(uint16_t idx);     // node-link accessor
//   uint32_t           load_gen(uint16_t idx);    // ACQUIRE
//   uint16_t           load_head();                // ACQUIRE chain head
//   bool               try_splice_head(uint16_t expected_head,
//                                       uint16_t new_top);
//   void               on_dead_intermediate_reclaim(uint16_t idx,
//                                                    uint8_t pre_state);
// `null_index()` must equal the consumer's chain-end sentinel (0 in
// every existing consumer). Encoded as `Ctx::kNullIndex` static
// constexpr — the sentinel is structural, not per-instance.
//
// DeadPolicy contract (all `static constexpr`):
//   bool is_idle_on_chain(uint8_t state);
//   bool is_dead_intermediate(Link l);
//   bool target_state_reclaims(uint8_t pre_mark_state);
//
//   `is_idle_on_chain`     IDLE/FREE state observed on a node that is
//                          still reachable from head — the documented
//                          T2 retry signal of the Safety Triad.
//   `is_dead_intermediate` Per-consumer "node has died on the chain"
//                          predicate. Takes the full Link so consumers
//                          can inspect reserved bits (CERT/MARK) in
//                          addition to state (futex's TIMED_OUT /
//                          SIGNALED_* on state byte alone;
//                          crystalline_slot_pool's CLAIMED+CERT=1
//                          post-finalize-pre-freelist-push transient).
//                          Walkers opp-splice such nodes and continue,
//                          repairing chains left dangling by other
//                          splicers — the load-bearing self-healing
//                          mechanism that prevents stale-link
//                          accumulation.
//   `target_state_reclaims` Whether a target observed in this state
//                          authorises reclaim by the walker (true →
//                          SplicedReclaim, false → SplicedNoReclaim).
//
// Per-slot `generation` field (32-bit atomic, ACQUIRE-readable) is a
// MANDATORY contract for every consumer of this walker. Bumped on
// every reclaim / freelist-push (consumer-side); captured by the
// caller of `harris_unlink` BEFORE entry; re-checked at the target
// site. Closes the slot-lifecycle ABA window (slot reclaimed +
// reallocated mid-walk) the per-link tag (T1) doesn't cover.
//
// Walker shape: identical to wait_slot's prior open-coded
// `harris_walk_attempt` (futex_utils.h) — mark-prev help-or-recede,
// dead-prev splice (futex-only via DeadPolicy), prev-state
// idle-on-chain Retry, mark-curr help-or-recede returning Retry,
// curr-state idle-on-chain Retry, target case with gen re-check,
// dead-curr opp-splice with continue (futex-only). Pool consumers
// where DeadPolicy::is_dead_intermediate ≡ false elide the dead-
// prev / dead-curr branches at compile time.

enum class WalkResult : int8_t {
  Retry = -1,
  NotFound = 0,
  SplicedNoReclaim = 1,
  SplicedReclaim = 2,
};

// Splice helpers shared by the walker and (optionally) consumer-side
// fast paths. Both are templates over Ctx so the link-and-CAS
// predicates resolve through the consumer's adapter without indirection.

// Mid splice via pred.link strong CAS: rewrite pred.next, drop
// CERT/MARK/ALERT, preserve state. Any concurrent mutation to
// pred.link (state transition, walker mark/finalize, opp-splice on
// pred's predecessor) fails the CAS — caller restarts.
template <class Ctx>
LIBC_INLINE bool harris_try_splice_at_pred(Ctx &ctx, uint16_t pred,
                                            Link expected_pred_link,
                                            uint16_t new_next) {
  Link desired = expected_pred_link.with_next_uncertify(new_next);
  return ctx.link_at(pred).compare_exchange_strong(
      expected_pred_link, desired, cpp::MemoryOrder::ACQ_REL,
      cpp::MemoryOrder::ACQUIRE);
}

// Dead-prev mid-splice. Distinct from harris_try_splice_at_pred: no
// captured snap of gp.link, so re-loads gp.link fresh and requires
// it to still point at prev with a live state and unmarked.
template <class Ctx, class DeadPolicy>
LIBC_INLINE bool harris_try_splice_dead_prev_mid(Ctx &ctx, uint16_t gp,
                                                  uint16_t prev,
                                                  uint16_t p_next) {
  auto &gs = ctx.link_at(gp);
  Link expected_link = gs.load(cpp::MemoryOrder::ACQUIRE);
  if (expected_link.next() != prev || expected_link.is_marked() ||
      DeadPolicy::is_idle_on_chain(expected_link.state()) ||
      DeadPolicy::is_dead_intermediate(expected_link))
    return false;
  return gs.compare_exchange_strong(
      expected_link, expected_link.with_next_uncertify(p_next),
      cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE);
}

// One head-anchored walk attempt. Caller (harris_unlink) loops on
// Retry; each attempt does one walk + race dispatch.
template <class Ctx, class DeadPolicy>
LIBC_INLINE WalkResult harris_walk_attempt(Ctx &ctx, uint16_t target,
                                            uint32_t target_gen_entry) {
  // Entry gen check — target reclaimed between bind and now ⇒ done.
  if (ctx.load_gen(target) != target_gen_entry)
    return WalkResult::NotFound;

  uint16_t gp = Ctx::kNullIndex;     // grandparent (prev of prev)
  uint16_t prev = Ctx::kNullIndex;   // virtual head sentinel
  uint16_t curr = ctx.load_head();

  while (curr != Ctx::kNullIndex) {
    // One atomic re-read of prev.link serves three purposes:
    //   (1) reachability check prev.next == curr,
    //   (2) expected-value for the mid-splice CAS (closes the
    //       "prev popped, CAS succeeds on detached slot" race),
    //   (3) prev.state dispatch — live, idle-on-chain, or dead.
    uint16_t observed;
    Link prev_link;
    if (prev == Ctx::kNullIndex) {
      observed = ctx.load_head();
    } else {
      prev_link = ctx.link_at(prev).load(cpp::MemoryOrder::ACQUIRE);
      // Mark-prev: another walker is mid-splicing prev. HELP-NOT-WAIT
      // — strong CAS at the parent rewrite serializes original-marker
      // vs helper races. On help-success, finalize publishes CERT
      // atomically with MARK clear; if the spliced slot was a
      // reclaimable dead state, reclaim too. On help-failure, LEAVE
      // the mark — clearing sabotages a live original splicer's
      // finalize. Dead-splicer recovery: a future help-success on
      // this slot clears the mark via finalize once the chain
      // stabilizes.
      if (prev_link.is_marked()) {
        uint16_t p_next = prev_link.next();
        uint8_t pre_state = prev_link.state();
        bool spliced =
            (gp == Ctx::kNullIndex)
                ? ctx.try_splice_head(prev, p_next)
                : harris_try_splice_dead_prev_mid<Ctx, DeadPolicy>(
                      ctx, gp, prev, p_next);
        LFL_TRACE_BRANCH(1, gp, prev_link.raw(), prev, prev_link.raw(),
                          p_next, spliced);
        if (spliced) {
          // Validate published p_next is not on freelist. Inline load
          // is intentional — the no-op macro discards args unevaluated,
          // so production pays nothing.
          LFL_VALIDATE_PUBLISH(
              1, gp, p_next,
              ctx.link_at(p_next).load(cpp::MemoryOrder::ACQUIRE).raw());
          link_finalize_after_splice(
              ctx.link_at(prev),
              MarkedLinkSnap::from_observed_marked(prev_link));
          if (DeadPolicy::is_dead_intermediate(prev_link) &&
              DeadPolicy::target_state_reclaims(pre_state))
            ctx.on_dead_intermediate_reclaim(prev, pre_state);
        }
        return WalkResult::Retry;
      }
      observed = prev_link.next();
      uint8_t prev_state = prev_link.state();

      // Idle-on-chain (T2): stale snapshot from a stolen-list reclaim
      // or peer-completed splice. Retry, NEVER assert.
      if (DeadPolicy::is_idle_on_chain(prev_state))
        return WalkResult::Retry;

      // Dead-prev splice — opp-splice through nodes the consumer
      // declares dead-on-chain. For futex this is TIMED_OUT/SIGNALED_*
      // (state-byte based); for crystalline_slot_pool this is
      // CLAIMED+CERT=1 (post-finalize, pre-freelist-push transient
      // — load-bearing for chain self-healing). Splice via gp.link
      // (or head if gp is null); resume with prev := gp.
      if (DeadPolicy::is_dead_intermediate(prev_link)) {
        bool reclaim_dead = DeadPolicy::target_state_reclaims(prev_state);
        // Self-mark on dead prev to freeze prev.next while we CAS
        // the parent. Closes the stale-p_next race where a
        // concurrent op-splice of prev's successor advances
        // prev.next between our capture and the parent CAS.
        auto &ps = ctx.link_at(prev);
        if (!link_cas_set_mark(ps, prev_link))
          return WalkResult::Retry;
        MarkedLinkSnap prev_marked_snap = link_pack_after_mark(prev_link);
        uint16_t p_next = prev_link.next();
        bool spliced =
            (gp == Ctx::kNullIndex)
                ? ctx.try_splice_head(prev, p_next)
                : harris_try_splice_dead_prev_mid<Ctx, DeadPolicy>(
                      ctx, gp, prev, p_next);
        LFL_TRACE_BRANCH(2, gp, prev_link.raw(), prev, prev_link.raw(),
                          p_next, spliced);
        if (!spliced) {
          // LEAVE the self-mark — clearing forces re-marking on
          // retry. A future walker observing the leftover mark
          // opp-splices and reclaims if the state is reclaimable,
          // so the dead slot doesn't leak.
          return WalkResult::Retry;
        }
        // Validate published p_next is not on freelist.
        LFL_VALIDATE_PUBLISH(
            2, gp, p_next,
            ctx.link_at(p_next).load(cpp::MemoryOrder::ACQUIRE).raw());
        link_finalize_after_splice(ps, prev_marked_snap);
        if (reclaim_dead)
          ctx.on_dead_intermediate_reclaim(prev, prev_state);
        // Rewind: gp's own predecessor isn't tracked, so drop gp
        // to NULL. Bounded — dead-set shrinks by one per splice.
        prev = gp;
        gp = Ctx::kNullIndex;
        continue;
      }
      // prev live.
    }
    if (observed != curr)
      return WalkResult::Retry;

    // One load covers curr's next AND state (folded in link).
    auto &cs = ctx.link_at(curr);
    Link c_link = cs.load(cpp::MemoryOrder::ACQUIRE);
    // Mark-curr: another walker mid-splice on curr (or our own
    // leftover self-mark from a prior failed dead-curr / target
    // splice attempt). HELP-NOT-WAIT — opp-splice + finalize.
    //
    // Splice-fail always Retries — leave the mark for the original
    // splicer's finalize (or a future helper).
    if (c_link.is_marked()) {
      uint16_t c_next_marked = c_link.next();
      uint8_t pre_state = c_link.state();
      bool spliced =
          (prev == Ctx::kNullIndex)
              ? ctx.try_splice_head(curr, c_next_marked)
              : harris_try_splice_at_pred<Ctx>(ctx, prev, prev_link,
                                                c_next_marked);
      LFL_TRACE_BRANCH(3, prev, prev_link.raw(), curr, c_link.raw(),
                        c_next_marked, spliced);
      if (spliced) {
        // Validate published c_next_marked is not on freelist.
        LFL_VALIDATE_PUBLISH(3, prev, c_next_marked,
                              ctx.link_at(c_next_marked)
                                  .load(cpp::MemoryOrder::ACQUIRE)
                                  .raw());
        link_finalize_after_splice(
            cs, MarkedLinkSnap::from_observed_marked(c_link));
        if (DeadPolicy::is_dead_intermediate(c_link) &&
            DeadPolicy::target_state_reclaims(pre_state))
          ctx.on_dead_intermediate_reclaim(curr, pre_state);
      }
      return WalkResult::Retry;
    }
    uint16_t c_next = c_link.next();
    uint8_t c_state = c_link.state();
    // Idle-on-chain (T2): stale snapshot. Retry, NEVER assert.
    if (DeadPolicy::is_idle_on_chain(c_state))
      return WalkResult::Retry;
    bool c_dead = DeadPolicy::is_dead_intermediate(c_link);

    if (curr == target) {
      // Target reclaimed by another actor between bind and now.
      if (ctx.load_gen(target) != target_gen_entry)
        return WalkResult::NotFound;

      // Mark target.link to freeze target.next during parent CAS.
      // If a prior failed attempt left our self-mark, reuse the
      // existing snap (re-marking would CAS-fail on the already-
      // set MARK).
      MarkedLinkSnap target_marked_snap =
          c_link.is_marked()
              ? MarkedLinkSnap::from_observed_marked(c_link)
              : link_pack_after_mark(c_link);
      if (!c_link.is_marked()) {
        if (!link_cas_set_mark(cs, c_link))
          return WalkResult::Retry;
      }
      bool spliced =
          (prev == Ctx::kNullIndex)
              ? ctx.try_splice_head(target, c_next)
              : harris_try_splice_at_pred<Ctx>(ctx, prev, prev_link,
                                                c_next);
      LFL_TRACE_BRANCH(4, prev, prev_link.raw(), target, c_link.raw(),
                        c_next, spliced);
      if (spliced) {
        // Validate published c_next is not on freelist.
        LFL_VALIDATE_PUBLISH(
            4, prev, c_next,
            ctx.link_at(c_next).load(cpp::MemoryOrder::ACQUIRE).raw());
        link_finalize_after_splice(cs, target_marked_snap);
        return DeadPolicy::target_state_reclaims(c_state)
                   ? WalkResult::SplicedReclaim
                   : WalkResult::SplicedNoReclaim;
      }
      // Splice failed. LEAVE the self-mark — next retry observes
      // it via the c_link.is_marked() check above and reuses
      // the snap without re-CAS'ing.
      return WalkResult::Retry;
    }

    // Opp-splice dead intermediates (futex-only via DeadPolicy).
    // Self-reclaim is safe: reclaim direct-pushes to freelist; the
    // state+tag fold ensures any later walker that observes the
    // freelist-pushed slot either trips the IDLE-on-chain retry or
    // fails its mid-splice CAS via tag mismatch.
    if (c_dead) {
      if (!link_cas_set_mark(cs, c_link))
        return WalkResult::Retry;
      MarkedLinkSnap opp_marked_snap = link_pack_after_mark(c_link);
      bool spliced =
          (prev == Ctx::kNullIndex)
              ? ctx.try_splice_head(curr, c_next)
              : harris_try_splice_at_pred<Ctx>(ctx, prev, prev_link,
                                                c_next);
      LFL_TRACE_BRANCH(5, prev, prev_link.raw(), curr, c_link.raw(),
                        c_next, spliced);
      if (spliced) {
        // Validate published c_next is not on freelist.
        LFL_VALIDATE_PUBLISH(
            5, prev, c_next,
            ctx.link_at(c_next).load(cpp::MemoryOrder::ACQUIRE).raw());
        link_finalize_after_splice(cs, opp_marked_snap);
        if (DeadPolicy::target_state_reclaims(c_state))
          ctx.on_dead_intermediate_reclaim(curr, c_state);
        curr = c_next;
        continue;
      }
      // Splice failed. LEAVE the self-mark. A future walker visiting
      // this slot via the c_link.is_marked() observed-mark branch
      // above will opp-splice and reclaim if the state is reclaimable.
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

// Lock-free find+splice of `target` from the chain `ctx` exposes.
// Loops on Retry; bounded by concurrent activity (≤ thread count).
//
// `expected_gen` — caller's gen capture. Mismatch ⇒ slot was
// reclaimed and possibly reallocated; MUST NOT splice (would steal
// a live slot from an unrelated waiter). Return false immediately.
//
// Returns true iff the walker spliced the target AND
// DeadPolicy::target_state_reclaims(target_state) — i.e., the caller
// is authorised to reclaim. Pool consumers (DeadPolicy where
// target_state_reclaims ≡ true on the live-state) get true on
// successful splice; futex-style consumers get true only when
// target was in a reclaimable dead state.
template <class Ctx, class DeadPolicy>
LIBC_INLINE bool harris_unlink(Ctx &ctx, uint16_t target,
                                uint32_t expected_gen) {
  if (target == Ctx::kNullIndex)
    return false;
  if (ctx.load_gen(target) != expected_gen)
    return false;
  uint32_t retries = 0;
  for (;;) {
    WalkResult r = harris_walk_attempt<Ctx, DeadPolicy>(ctx, target,
                                                          expected_gen);
    if (r == WalkResult::Retry) {
      if (++retries > LFL_RETRY_LIMIT)
        LFL_RETRY_OVERFLOW(target, retries);
      continue;
    }
    return r == WalkResult::SplicedReclaim;
  }
}

// ===========================================================================
// Consumer offset contract
// ===========================================================================
//
// Every consumer SHOULD use this macro at namespace scope (or
// directly after the node type's class definition) to lock the
// offset of the node's `cpp::Atomic<Link>` field structurally.
// The static_assert catches reorderings that would silently change
// the cache layout the substrate's hot path was tuned against.
//
// Usage:
//   struct MyNode { ... cpp::Atomic<linkage::Link> link; ... };
//   LINKAGE_REQUIRES_LINK_AT(MyNode, /*expected offset=*/ 16);
#define LINKAGE_REQUIRES_LINK_AT(NodeT, OFFSET)                                \
  static_assert(__builtin_offsetof(NodeT, link) == (OFFSET),                   \
                #NodeT "::link must live at offset " #OFFSET                   \
                       " — substrate's hot path depends on the documented "  \
                       "layout");                                              \
  static_assert(sizeof(decltype(NodeT::link)) ==                               \
                    sizeof(::LIBC_NAMESPACE::cpp::Atomic<                      \
                           ::LIBC_NAMESPACE::linkage::Link>),                  \
                #NodeT "::link must be cpp::Atomic<linkage::Link>")

// Per-slot 32-bit generation counter — substrate-mandatory whenever
// a consumer plugs the harris_walk_attempt / harris_unlink walker
// templates. Bumped on every reclaim / freelist-push; captured at
// walker entry; re-checked at the target site. 32-bit width is
// load-bearing: at 10K reclaims/sec the wraparound horizon is >130
// years; 16-bit would wrap in seconds under stress.
#define LINKAGE_REQUIRES_GENERATION_AT(NodeT, OFFSET)                          \
  static_assert(__builtin_offsetof(NodeT, generation) == (OFFSET),             \
                #NodeT "::generation must live at offset " #OFFSET             \
                       " — substrate walker reads it for entry-bind + "      \
                       "target re-check");                                     \
  static_assert(sizeof(decltype(NodeT::generation)) ==                         \
                    sizeof(::LIBC_NAMESPACE::cpp::Atomic<uint32_t>),           \
                #NodeT "::generation must be cpp::Atomic<uint32_t>")

} // namespace linkage
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_LOCK_FREE_LINKAGE_H
