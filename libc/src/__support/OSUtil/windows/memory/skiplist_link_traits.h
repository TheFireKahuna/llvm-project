//===- skiplist_link_traits.h - Skiplist link bit layout --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Bit layout and edge-validation policy for the interval-skiplist link
// word — the canonical encoding contract for the 64-bit linkage::Link
// used by every node in the va_tracker's concurrent interval skiplist
// (Kim, Kwon, Kang SOSP 2025).
//
// Link word, 64 bits:
//
//  63    56 55      51 50  49   48  47          16 15        0
// +--------+----------+----+----+----+-------------+----------+
// | state  | reserved | AF |CERT|MARK|     tag     |   next   |
// +--------+----------+----+----+----+-------------+----------+
//
//   * state — SkiplistNodeState: LIVE / LOCKED / INVALIDATED / IDLE.
//   * tag — monotonic ABA counter, bumped by every substrate CAS via
//     Link::with_tag_bumped.
//   * MARK — Harris-style logical-deletion mark for the splice walker;
//     preserved across state CAS via LINK_PRESERVE_MASK.
//   * CERT — Crystalline-W "proven off-chain" certificate; preserved
//     across state CAS.
//   * ALERT_FIRED — set atomically with state transitions whose
//     fires_alert returns true; paired futex_addr::wake issues
//     NtAlertThreadByThreadId so a parker can distinguish a genuine
//     wake from a spurious refresh.
//   * next — encoded (chunk_id << 8 | slot_idx); 0xFFFF denotes null.
//
// Packing these into one atomic — rather than a separately allocated
// per-node rwlock — is what gives Kim et al. their 13.1x mmap scaling.
// Tag monotonicity, chain pointer, lock state, and the parking address
// all share the one word.
//
// Safety Triad composition with the substrate:
//   T1 (tag monotonicity) — every is_valid edge bumps the tag; a
//      snap-taker that missed a transition cannot succeed on the stale
//      value.
//   T2 (IDLE-on-reachable Retry) — IDLE is reachable only after the
//      Crystalline FreeFn has fired; a Query walker observing IDLE on
//      a chain it just resolved triggers the substrate's retry.
//   T3 (never-freed pool VA) — chunk VA never returns to the OS, so a
//      stale link decode always lands on a canary-validated slot.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_SKIPLIST_LINK_TRAITS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_SKIPLIST_LINK_TRAITS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

// Four-state node lifecycle, encoded in the substrate's 8-bit state
// byte at LINK_STATE_SHIFT. Numeric values are part of the substrate-
// trace ABI (CPU traces / fastfail diagnostics report the raw byte);
// renumbering is a debug-tool break, not a correctness break.
enum class SkiplistNodeState : uint8_t {
  // Out-edges: LIVE -> LOCKED on Lock CAS; LIVE -> INVALIDATED on the
  // non-locked Erase shortcut.
  LIVE = 0,

  // A Map operation holds this link in its locked set. Readers walk
  // through (the node is still chain-resident), but any new Lock
  // acquirer parks on the link until the release transition fires
  // LINK_ALERT_FIRED_BIT and NtAlertThreadByThreadId drains the parker.
  LOCKED = 1,

  // Swap has committed and the old entry has been logically retired.
  // Paper invariant I5 keeps `value` dereferenceable for the grace
  // window so VEH-callable Query can still resolve a faulting address
  // against the about-to-be-retired interval.
  INVALIDATED = 2,

  // Substrate T2 reclaim sentinel. Written only by the Crystalline
  // FreeFn at retire time and MUST never be observed on a chain
  // reachable from a live head_next(i); a Query walker observing IDLE
  // on a just-resolved node is the trigger for the substrate's retry.
  IDLE = 3,
};

// Substrate evaluates is_valid inside a static_assert and fires_alert
// inside an if constexpr, so the hooks MUST be static constexpr — a
// non-constexpr body is a compile error at the substrate's CAS sites.
struct SkiplistLinkTraits {
  // Permitted edges:
  //   LIVE        -> LOCKED       Lock acquisition (paper Algorithm 1).
  //   LOCKED      -> LIVE         Swap relink — predecessor's chain
  //                               commit doubles as the lock release.
  //   LOCKED      -> INVALIDATED  Swap retire — old member sealed
  //                               off-chain.
  //   LIVE        -> INVALIDATED  Erase shortcut — non-locked Erase
  //                               that takes the predecessor lock and
  //                               marks the victim INVALIDATED in one
  //                               CAS.
  //   LOCKED      -> LOCKED       Re-entrant Lock by the same Map op
  //                               for a multi-level upper publish; the
  //                               tag bump preserves liveness signature
  //                               while leaving the lock owner intact.
  //   INVALIDATED -> IDLE         Crystalline FreeFn only — T2
  //                               retire-Retry gate; never reached by
  //                               ordinary CAS callers.
  // All other edges rejected.
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

  // Returns true iff the substrate CAS must set LINK_ALERT_FIRED_BIT
  // atomically with the state transition. with_state_alerting folds
  // the bit into the CAS desired-word so a parker's post-wake snap
  // differs from its pre-park snap; the paired futex_addr::wake in
  // the cpp then issues NtAlertThreadByThreadId.
  //
  // Firing edges:
  //   LIVE -> LOCKED, LOCKED -> LIVE, LIVE -> INVALIDATED,
  //   LOCKED -> INVALIDATED.
  //
  // LIVE -> LOCKED fires so a concurrent acquirer that read pred's LIVE
  // snap and is about to `futex_addr::wait` sees the post-CAS word differ
  // (tag bump + state byte change) and bails without parking. The release
  // edges (LOCKED -> LIVE / -> INVALIDATED) fire so any parker that did
  // sleep wakes when the holder departs.
  //
  // LOCKED -> LOCKED (re-entrant publish on a held lock — no parker to
  // release) and INVALIDATED -> IDLE (reclaim-only — parkers cannot
  // observe an INVALIDATED link) do NOT fire.
  //
  // Other writers preserve any previously-set ALERT_FIRED via
  // LINK_PRESERVE_MASK; transitions that reset the link's wake-cycle
  // identity clear it.
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
