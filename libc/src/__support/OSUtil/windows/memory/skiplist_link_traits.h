//===- skiplist_link_traits.h - Skiplist link bit layout --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bit layout and edge-validation policy for the interval-skiplist link
/// word. This file is the canonical encoding contract for the 64-bit
/// `linkage::Link` value used by every node in the va_tracker's
/// concurrent interval skiplist (Kim, Kwon, and Kang, SOSP 2025).
///
/// The link word is the substrate's single atomic unit. It must
/// simultaneously carry:
///
///   * The chain-pointer half (encoded chunk_id + slot_idx, 16 bits).
///   * A monotonic ABA tag (32 bits) bumped on every successful CAS so
///     in-place reuse cannot fool a snap-taker.
///   * The Harris MARK + Crystalline CERT bits (bits 48-49) used by the
///     mid-chain splice protocol and the reclamation off-chain proof.
///   * The wake-event ALERT_FIRED bit (bit 50) signalling that a
///     release-class transition has issued (or will issue) an
///     `NtAlertThreadByThreadId` against this link's parker.
///   * The skiplist node state byte (bits 56-63).
///
/// Packing these into one atomic — rather than allocating a separate
/// rwlock per node — is what gives Kim et al. their 13.1x mmap-microbench
/// scaling. Tag monotonicity, chain pointer, lock state, and the parking
/// address (the link word itself) share one atomic; a Lock-acquisition
/// CAS simultaneously bumps the tag, flips the state byte, and publishes
/// the ALERT_FIRED bit that any `futex_addr::wait` parker uses to
/// distinguish a genuine wake from a spurious refresh.
///
/// Link word, 64 bits:
///
/// \code
///  63    56 55      51 50  49   48  47          16 15        0
/// +--------+----------+----+----+----+-------------+----------+
/// | state  | reserved | AF |CERT|MARK|     tag     |   next   |
/// +--------+----------+----+----+----+-------------+----------+
/// \endcode
///
///   * `state` (bits 56-63, mask `LINK_STATE_MASK << LINK_STATE_SHIFT`)
///     — `SkiplistNodeState` value: LIVE / LOCKED / INVALIDATED / IDLE.
///   * `tag` (bits 16-47, mask `LINK_TAG_MASK << LINK_TAG_SHIFT`) —
///     monotonic ABA counter, bumped by every substrate CAS via
///     `Link::with_tag_bumped`.
///   * `MARK` (bit 48) — Harris-style logical-deletion mark for the
///     mid-chain splice walker; preserved across state CAS via
///     `LINK_PRESERVE_MASK`.
///   * `CERT` (bit 49) — Crystalline-W "proven off-chain" certificate;
///     preserved across state CAS.
///   * `ALERT_FIRED` (bit 50, `LINK_ALERT_FIRED_BIT`) — set atomically
///     with the state transition whose `fires_alert` returns true; the
///     paired `futex_addr::wake` issues `NtAlertThreadByThreadId` to
///     drain any parker.
///   * `next` (bits 0-15) — encoded `(chunk_id << 8 | slot_idx)`
///     successor pointer, resolved through the process-global chunk
///     table; sentinel `0xFFFF` denotes null.
///
/// The Safety Triad inherited from the substrate composes directly with
/// this state machine. T1 (tag monotonicity) — every `is_valid` edge is
/// a substrate CAS that bumps the tag, so a snap-taker that misses a
/// transition cannot succeed on the stale value. T2 (IDLE-on-reachable-
/// chain Retry) — a Query walker that observes IDLE on a chain it just
/// resolved triggers the substrate's structural retry, since IDLE is
/// reachable only after the Crystalline `FreeFn` has fired. T3 (never-
/// freed pool memory) — the per-class chunk-bitmap allocator never
/// returns chunk VA to the OS, so a stale link decode always lands on a
/// canary-validated slot.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_SKIPLIST_LINK_TRAITS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_SKIPLIST_LINK_TRAITS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

/// Four-state lifecycle for an interval-skiplist node, encoded in the
/// substrate's 8-bit `state` byte at `LINK_STATE_SHIFT`.
///
/// Numeric values are part of the substrate-trace ABI (CPU traces /
/// fastfail diagnostics report the raw `state()` byte). Renumbering is a
/// debug-tool break — not a correctness break — and should be avoided.
enum class SkiplistNodeState : uint8_t {
  /// Chain entry observable by readers and a candidate target for
  /// Lock-acquisition. Transitions out: LIVE -> LOCKED on a successful
  /// Lock CAS; LIVE -> INVALIDATED on the non-locked Erase shortcut.
  LIVE = 0,

  /// A Map operation holds this link in its locked set. Readers walk
  /// through (the node is still chain-resident), but any new Lock
  /// acquirer parks on the link via `futex_addr::wait` until the
  /// release transition (LOCKED -> LIVE or LOCKED -> INVALIDATED) fires
  /// `LINK_ALERT_FIRED_BIT` and `NtAlertThreadByThreadId` drains the
  /// parker.
  LOCKED = 1,

  /// Swap has committed and the old chain entry has been logically
  /// retired. Readers preserve the node's `value` payload until the
  /// Crystalline `FreeFn` fires; paper invariant I5 keeps INVALIDATED
  /// dereferenceable for the grace window so VEH-callable Query can
  /// still resolve a faulting address against the about-to-be-retired
  /// interval.
  INVALIDATED = 2,

  /// Substrate-T2 reclaim sentinel. Written only by the Crystalline
  /// `FreeFn` at retire time and MUST never be observed on a chain
  /// reachable from a live `head_next(i)`. A Query walker that observes
  /// IDLE on a node it just resolved is the trigger condition for the
  /// substrate's structural retry.
  IDLE = 3,
};

/// Substrate-Link policy class binding the interval-skiplist state
/// machine onto the generic `linkage::Link` substrate.
///
/// The substrate evaluates `is_valid` inside a `static_assert` and
/// `fires_alert` inside an `if constexpr`, so the hooks MUST be
/// `static constexpr` — a non-`constexpr` body is a compile error at
/// the substrate's CAS sites.
struct SkiplistLinkTraits {
  /// Returns true iff a substrate CAS from `from` to `to` is a
  /// permitted state transition.
  ///
  /// Permitted edges:
  ///
  ///   LIVE        -> LOCKED       Lock acquisition (paper Algorithm 1).
  ///   LOCKED      -> LIVE         Swap relink — predecessor's chain
  ///                               commit doubles as the lock release.
  ///   LOCKED      -> INVALIDATED  Swap retire — old locked-set member
  ///                               sealed off-chain.
  ///   LIVE        -> INVALIDATED  Erase shortcut — non-locked Erase
  ///                               that takes the predecessor lock and
  ///                               marks the victim INVALIDATED in one
  ///                               CAS.
  ///   LOCKED      -> LOCKED       Re-entrant Lock by the same Map op
  ///                               for a multi-level upper publish; the
  ///                               tag bump preserves liveness signature
  ///                               while leaving the lock owner intact.
  ///   INVALIDATED -> IDLE         Crystalline `FreeFn` only — T2
  ///                               retire-Retry gate; not reachable by
  ///                               ordinary CAS callers.
  ///
  /// All other edges (LIVE<->IDLE, LOCKED<->IDLE, IDLE<->anything
  /// outside reclaim) are rejected.
  ///
  /// \param from Raw `uint8_t` form of the source `SkiplistNodeState`
  ///             (the substrate caller passes the byte already widened).
  /// \param to   Raw `uint8_t` form of the destination state.
  /// \returns true if the edge is permitted; false otherwise.
  [[nodiscard]] LIBC_INLINE static constexpr bool is_valid(uint8_t from,
                                                            uint8_t to) {
    const auto F = static_cast<SkiplistNodeState>(from);
    const auto T = static_cast<SkiplistNodeState>(to);
    if (F == SkiplistNodeState::LIVE && T == SkiplistNodeState::LOCKED)
      return true;
    if (F == SkiplistNodeState::LOCKED && T == SkiplistNodeState::LIVE)
      return true;
    if (F == SkiplistNodeState::LOCKED &&
        T == SkiplistNodeState::INVALIDATED)
      return true;
    if (F == SkiplistNodeState::LIVE &&
        T == SkiplistNodeState::INVALIDATED)
      return true;
    if (F == SkiplistNodeState::LOCKED && T == SkiplistNodeState::LOCKED)
      return true;
    if (F == SkiplistNodeState::INVALIDATED &&
        T == SkiplistNodeState::IDLE)
      return true;
    return false;
  }

  /// Returns true iff the substrate CAS for this edge must set
  /// `LINK_ALERT_FIRED_BIT` atomically with the state transition.
  ///
  /// Contention parkers wait on the 64-bit link word via
  /// `futex_addr::wait(&link, snap)`. The substrate's
  /// `with_state_alerting` wither folds `LINK_ALERT_FIRED_BIT` into the
  /// CAS desired-word so the parker's post-wake snap differs from its
  /// pre-park snap. The paired `futex_addr::wake` in
  /// `interval_skiplist.cpp` then issues `NtAlertThreadByThreadId`
  /// against the parker's thread, distinguishing a genuine wake from a
  /// spurious refresh.
  ///
  /// Edges that fire alerts:
  ///
  ///   LIVE        -> LOCKED       Lock acquisition. A pre-park snap
  ///                               captured by `futex_addr::wait`
  ///                               differs from the post-CAS link via
  ///                               the tag bump and state change, so
  ///                               the wait returns immediately without
  ///                               actually parking.
  ///   LOCKED      -> LIVE         Unlock / Swap relink / Map cleanup.
  ///                               Wakes Lock acquirers parked waiting
  ///                               for this link to drop LOCKED.
  ///   LIVE        -> INVALIDATED  Erase shortcut retired this node.
  ///                               Wakes readers holding a stale LIVE
  ///                               snap so they refresh and observe the
  ///                               retire.
  ///   LOCKED      -> INVALIDATED  Swap committed and retired the old
  ///                               locked-set member. Same wake
  ///                               rationale as LOCKED -> LIVE for any
  ///                               concurrent acquirer.
  ///
  /// LOCKED -> LOCKED (re-entrant multi-level publish) and
  /// INVALIDATED -> IDLE (`FreeFn` reclaim) do NOT fire alerts: the
  /// former is a tag-bump-only refresh on a held lock with no parker
  /// to release, the latter is reclaim-only and unreachable while
  /// parkers can still observe the link.
  ///
  /// Other writers preserve any previously-set `LINK_ALERT_FIRED_BIT`
  /// via `LINK_PRESERVE_MASK`; the bit is cleared by transitions that
  /// reset the link's wake-cycle identity.
  ///
  /// \param from Raw `uint8_t` form of the source state.
  /// \param to   Raw `uint8_t` form of the destination state.
  /// \returns true if the substrate CAS must publish the alert bit.
  [[nodiscard]] LIBC_INLINE static constexpr bool fires_alert(uint8_t from,
                                                               uint8_t to) {
    using S = SkiplistNodeState;
    if (from == static_cast<uint8_t>(S::LIVE) &&
        to   == static_cast<uint8_t>(S::LOCKED))
      return true;
    if (from == static_cast<uint8_t>(S::LOCKED) &&
        to   == static_cast<uint8_t>(S::LIVE))
      return true;
    if (from == static_cast<uint8_t>(S::LIVE) &&
        to   == static_cast<uint8_t>(S::INVALIDATED))
      return true;
    if (from == static_cast<uint8_t>(S::LOCKED) &&
        to   == static_cast<uint8_t>(S::INVALIDATED))
      return true;
    return false;
  }
};

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_SKIPLIST_LINK_TRAITS_H
