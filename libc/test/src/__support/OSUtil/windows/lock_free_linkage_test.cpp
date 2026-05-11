//===-- Tests for the linkage::Link substrate ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Substrate-only coverage for `linkage::Link` and the link_cas_* /
// mark-finalize / certify / exchange families. Runs against a mock
// intrusive node backed by a static array (T3 "never-freed pool" honored
// by construction).
//
// Scenarios:
//   1. Bit constants — overlap / mutual-exclusion / preserve mask.
//   2. Link factories — pack / pack_marked / pack_certified produce the
//      documented bit pattern with tag preserved on construction.
//   3. Pure withers — every non-static wither bumps tag exactly once and
//      preserves all unrelated bits (MARK/CERT/ALERT_FIRED + state +
//      next).
//   4. Owner-exclusive rewrites — rewrite_certified, with_state_drop_mark,
//      with_next_drop_mark, with_next_uncertify, with_state_certified
//      drop the documented bits and preserve the rest.
//   5. Compound 1-bump withers — with_state_set_cert, with_cert_unmark,
//      with_state_alerting, with_state_set_cert_alerting commit all
//      mutations in a single tag bump.
//   6. Free-decoder shorthand — link_state/link_tag/link_next/
//      link_is_marked/link_is_certified/link_is_alert_fired match the
//      class accessors on identical inputs.
//   7. link_load_state — single-shot atomic load extracts the state byte.
//   8. link_cas_state — From-mismatch fails without writing; From-match
//      success bumps tag, applies state, preserves reserved bits.
//   9. link_cas_state with alerting Traits — fires_alert(F,T)=true
//      selects with_state_alerting, publishes ALERT_FIRED atomically.
//  10. link_cas_state_certify — From→To with CERT publish, MARK
//      preserved, single tag bump.
//  11. link_cas_snap — strong CAS against captured snap; retries
//      against a tag-bumped neighbor fail; runtime alert dispatch on
//      snap.state().
//  12. link_cas_state_detached — strong CAS against pre-detach snap;
//      success returns OLD state, failure returns CURRENT state and
//      ours_out=false; alerting branch publishes ALERT_FIRED.
//  13. link_exchange_state / link_exchange_state_certify — unconditional
//      state write, returns prior state, certify variant ALSO sets CERT.
//  14. link_cas_set_mark / link_pack_after_mark / MarkedLinkSnap — typed
//      contract: from_observed_marked rejects unmarked input via
//      LIBC_ASSERT.
//  15. link_finalize_after_splice — atomic CERT+!MARK publish, one CAS,
//      bail-on-fail (no retry).
//  16. link_clear_mark — CAS-loop clears MARK, preserves CERT, exits
//      early on externally cleared mark.
//  17. link_field_set_cert — fetch_or sets CERT idempotently without
//      bumping tag.
//  18. link_store / link_store_next / link_store_state — owner-exclusive
//      load-modify-store that bumps tag, drops MARK, preserves CERT
//      (or sets it via rewrite_certified for link_store).
//  19. link_cas_next — CAS-loop variant that retries against concurrent
//      writers; single-thread case behaves like link_store_next.
//  20. Multi-threaded ABA defense — N threads racing link_cas_state
//      against a shared Link see a monotonically growing tag and at
//      most one winner per (From, To) cycle.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/lock_free_linkage.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "hdr/stdint_proxy.h"

namespace {

namespace linkage = LIBC_NAMESPACE::linkage;
using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;

// Mock state machine. Symbolic constants exercise both the "dense low
// byte" pattern wait_slot uses AND a sparse pattern that would catch
// incorrect mask widths.
enum MockState : uint8_t {
  M_IDLE = 0,
  M_LIVE = 1,
  M_PARKED = 2,
  M_SIGNALED = 3,
  M_DEAD = 0x7F, // Sparse — verifies state byte uses the full 8 bits.
};

// Permissive Traits — every transition legal, alert never fired. Used
// by tests that only care about the link-side mutation, not the policy.
struct PermissiveTraits {
  static constexpr bool is_valid(uint8_t /*from*/, uint8_t /*to*/) {
    return true;
  }
  static constexpr bool fires_alert(uint8_t /*from*/, uint8_t /*to*/) {
    return false;
  }
};

// Alert-firing Traits — fires_alert for PARKED→SIGNALED (mock analog of
// wait_slot's IN_KERNEL→SIGNALED_*). Drives the link_cas_state ALERT_
// FIRED branch and the runtime selection in link_cas_snap /
// link_cas_state_detached.
struct AlertingTraits {
  static constexpr bool is_valid(uint8_t /*from*/, uint8_t /*to*/) {
    return true;
  }
  static constexpr bool fires_alert(uint8_t from, uint8_t to) {
    return from == M_PARKED && to == M_SIGNALED;
  }
};

// Mock intrusive node — minimal viable substrate consumer. The Link
// field is at the documented offset 0; LINKAGE_REQUIRES_LINK_AT enforces
// it structurally.
struct MockNode {
  Atomic<linkage::Link> link;
  uint64_t payload; // not touched by the substrate
};
LINKAGE_REQUIRES_LINK_AT(MockNode, 0);

// Static pool — T3 "never-freed pool memory" honored.
constexpr uint16_t POOL_CAP = 16;
MockNode g_pool[POOL_CAP];

void reset_pool() {
  for (uint16_t i = 0; i < POOL_CAP; ++i) {
    g_pool[i].link.store(linkage::Link{}, MemoryOrder::RELAXED);
    g_pool[i].payload = 0;
  }
}

linkage::Link load(uint16_t i) {
  return g_pool[i].link.load(MemoryOrder::ACQUIRE);
}

} // namespace

// --- 1. Bit constants ------------------------------------------------------
TEST(LlvmLibcLockFreeLinkageTest, BitConstantsAreNonOverlapping) {
  static_assert((linkage::LINK_MARK_BIT & linkage::LINK_CERT_BIT) == 0);
  static_assert((linkage::LINK_MARK_BIT & linkage::LINK_ALERT_FIRED_BIT) == 0);
  static_assert((linkage::LINK_CERT_BIT & linkage::LINK_ALERT_FIRED_BIT) == 0);
  static_assert(linkage::LINK_PRESERVE_MASK ==
                (linkage::LINK_MARK_BIT | linkage::LINK_CERT_BIT |
                 linkage::LINK_ALERT_FIRED_BIT));
  EXPECT_EQ(static_cast<uint64_t>(0x1ULL << 48), linkage::LINK_MARK_BIT);
  EXPECT_EQ(static_cast<uint64_t>(0x1ULL << 49), linkage::LINK_CERT_BIT);
  EXPECT_EQ(static_cast<uint64_t>(0x1ULL << 50),
            linkage::LINK_ALERT_FIRED_BIT);
}

// --- 2. Static factories ---------------------------------------------------
TEST(LlvmLibcLockFreeLinkageTest, FactoriesPreserveTagAndSetReservedBits) {
  auto plain = linkage::Link::pack(M_LIVE, 0x12345678u, 7);
  EXPECT_EQ(static_cast<uint8_t>(M_LIVE), plain.state());
  EXPECT_EQ(0x12345678u, plain.tag());
  EXPECT_EQ(static_cast<uint16_t>(7), plain.next());
  EXPECT_FALSE(plain.is_marked());
  EXPECT_FALSE(plain.is_certified());
  EXPECT_FALSE(plain.is_alert_fired());

  auto marked = linkage::Link::pack_marked(M_LIVE, 0x12345678u, 7);
  EXPECT_TRUE(marked.is_marked());
  EXPECT_FALSE(marked.is_certified());
  EXPECT_EQ(plain.state(), marked.state());
  EXPECT_EQ(plain.tag(), marked.tag());
  EXPECT_EQ(plain.next(), marked.next());

  auto certified = linkage::Link::pack_certified(M_LIVE, 0x12345678u, 7);
  EXPECT_FALSE(certified.is_marked());
  EXPECT_TRUE(certified.is_certified());
  EXPECT_EQ(plain.state(), certified.state());
  EXPECT_EQ(plain.tag(), certified.tag());
  EXPECT_EQ(plain.next(), certified.next());

  // raw / from_raw round-trip.
  auto round = linkage::Link::from_raw(plain.raw());
  EXPECT_EQ(plain.raw(), round.raw());
}

TEST(LlvmLibcLockFreeLinkageTest, FactoriesUseFullStateByte) {
  // M_DEAD = 0x7F exercises the upper state-byte bits without colliding
  // with the reserved bit window.
  auto l = linkage::Link::pack(M_DEAD, 0xDEADBEEFu, 0xABCD);
  EXPECT_EQ(static_cast<uint8_t>(M_DEAD), l.state());
  EXPECT_EQ(0xDEADBEEFu, l.tag());
  EXPECT_EQ(static_cast<uint16_t>(0xABCD), l.next());
}

// --- 3. Pure withers — tag bump + bit preservation -------------------------
TEST(LlvmLibcLockFreeLinkageTest, PureWithersBumpTagAndPreserveOthers) {
  // Pre-load with all reserved bits set so we can verify preservation.
  auto base = linkage::Link::pack(M_LIVE, 100, 5);
  base = base.marked().certified(); // tags now 102.
  base = linkage::Link::from_raw(base.raw() | linkage::LINK_ALERT_FIRED_BIT);
  EXPECT_TRUE(base.is_marked());
  EXPECT_TRUE(base.is_certified());
  EXPECT_TRUE(base.is_alert_fired());

  uint32_t base_tag = base.tag();

  auto wstate = base.with_state(M_PARKED);
  EXPECT_EQ(static_cast<uint8_t>(M_PARKED), wstate.state());
  EXPECT_EQ(base_tag + 1, wstate.tag());
  EXPECT_EQ(base.next(), wstate.next());
  EXPECT_TRUE(wstate.is_marked());
  EXPECT_TRUE(wstate.is_certified());
  EXPECT_TRUE(wstate.is_alert_fired());

  auto wnext = base.with_next(9);
  EXPECT_EQ(static_cast<uint16_t>(9), wnext.next());
  EXPECT_EQ(base.state(), wnext.state());
  EXPECT_EQ(base_tag + 1, wnext.tag());
  EXPECT_TRUE(wnext.is_marked());
  EXPECT_TRUE(wnext.is_certified());
  EXPECT_TRUE(wnext.is_alert_fired());

  auto unmarked = base.unmarked();
  EXPECT_FALSE(unmarked.is_marked());
  EXPECT_TRUE(unmarked.is_certified());
  EXPECT_TRUE(unmarked.is_alert_fired());
  EXPECT_EQ(base_tag + 1, unmarked.tag());

  auto uncertified = base.uncertified();
  EXPECT_FALSE(uncertified.is_certified());
  EXPECT_TRUE(uncertified.is_marked());
  EXPECT_TRUE(uncertified.is_alert_fired());
  EXPECT_EQ(base_tag + 1, uncertified.tag());
}

// --- 4. Owner-exclusive rewrites — drop MARK (and ALERT_FIRED for the
//        wake-cycle resets) -------------------------------------------------
TEST(LlvmLibcLockFreeLinkageTest, RewriteCertifiedDropsMarkAndAlertFired) {
  auto base = linkage::Link::pack_marked(M_LIVE, 50, 3);
  base =
      linkage::Link::from_raw(base.raw() | linkage::LINK_ALERT_FIRED_BIT);
  EXPECT_TRUE(base.is_marked());
  EXPECT_TRUE(base.is_alert_fired());

  auto rw = base.rewrite_certified(M_IDLE, 0);
  EXPECT_EQ(static_cast<uint8_t>(M_IDLE), rw.state());
  EXPECT_EQ(static_cast<uint16_t>(0), rw.next());
  EXPECT_TRUE(rw.is_certified());
  EXPECT_FALSE(rw.is_marked());
  EXPECT_FALSE(rw.is_alert_fired());
  EXPECT_EQ(base.tag() + 1, rw.tag());
}

TEST(LlvmLibcLockFreeLinkageTest, WithStateDropMarkPreservesCertAndAlert) {
  auto base = linkage::Link::pack_marked(M_LIVE, 50, 3).certified();
  base =
      linkage::Link::from_raw(base.raw() | linkage::LINK_ALERT_FIRED_BIT);

  auto out = base.with_state_drop_mark(M_PARKED);
  EXPECT_EQ(static_cast<uint8_t>(M_PARKED), out.state());
  EXPECT_FALSE(out.is_marked());
  EXPECT_TRUE(out.is_certified());
  EXPECT_TRUE(out.is_alert_fired());
}

TEST(LlvmLibcLockFreeLinkageTest, WithNextUncertifyDropsThreeBits) {
  auto base = linkage::Link::pack_marked(M_LIVE, 50, 3).certified();
  base =
      linkage::Link::from_raw(base.raw() | linkage::LINK_ALERT_FIRED_BIT);

  auto out = base.with_next_uncertify(7);
  EXPECT_EQ(static_cast<uint16_t>(7), out.next());
  EXPECT_EQ(base.state(), out.state());
  EXPECT_FALSE(out.is_marked());
  EXPECT_FALSE(out.is_certified());
  EXPECT_FALSE(out.is_alert_fired());
}

TEST(LlvmLibcLockFreeLinkageTest,
     WithStateCertifiedDropsMarkAndAlertSetsCert) {
  auto base = linkage::Link::pack_marked(M_LIVE, 50, 3);
  base =
      linkage::Link::from_raw(base.raw() | linkage::LINK_ALERT_FIRED_BIT);

  auto out = base.with_state_certified(M_IDLE);
  EXPECT_EQ(static_cast<uint8_t>(M_IDLE), out.state());
  EXPECT_FALSE(out.is_marked());
  EXPECT_TRUE(out.is_certified());
  EXPECT_FALSE(out.is_alert_fired());
}

// --- 5. Compound 1-bump withers --------------------------------------------
TEST(LlvmLibcLockFreeLinkageTest, CompoundWithersBumpTagOnce) {
  auto base = linkage::Link::pack_marked(M_LIVE, 100, 2);
  uint32_t bt = base.tag();

  auto a = base.with_state_set_cert(M_PARKED);
  EXPECT_EQ(bt + 1, a.tag());
  EXPECT_EQ(static_cast<uint8_t>(M_PARKED), a.state());
  EXPECT_TRUE(a.is_marked()); // preserved
  EXPECT_TRUE(a.is_certified());

  auto b = base.with_cert_unmark();
  EXPECT_EQ(bt + 1, b.tag());
  EXPECT_FALSE(b.is_marked());
  EXPECT_TRUE(b.is_certified());
  EXPECT_EQ(base.state(), b.state());

  auto c = base.with_state_alerting(M_SIGNALED);
  EXPECT_EQ(bt + 1, c.tag());
  EXPECT_EQ(static_cast<uint8_t>(M_SIGNALED), c.state());
  EXPECT_TRUE(c.is_marked()); // preserved
  EXPECT_FALSE(c.is_certified());
  EXPECT_TRUE(c.is_alert_fired());

  auto d = base.with_state_set_cert_alerting(M_SIGNALED);
  EXPECT_EQ(bt + 1, d.tag());
  EXPECT_EQ(static_cast<uint8_t>(M_SIGNALED), d.state());
  EXPECT_TRUE(d.is_marked()); // preserved
  EXPECT_TRUE(d.is_certified());
  EXPECT_TRUE(d.is_alert_fired());
}

// `with_state_and_next` mutates state + next in one tag bump, preserving
// MARK / CERT / ALERT_FIRED. Skiplist Swap relies on the compound write
// — a two-step `with_state(LIVE).with_next(N)` would expose either
// (LOCKED + new next) or (LIVE + old next) mid-Swap, breaking the
// linearisation point.
TEST(LlvmLibcLockFreeLinkageTest, WithStateAndNextSingleBumpAndPreservation) {
  // Pre-load all reserved bits so we can verify preservation.
  auto base = linkage::Link::pack_marked(M_PARKED, 200, 5).certified();
  base = linkage::Link::from_raw(base.raw() | linkage::LINK_ALERT_FIRED_BIT);
  EXPECT_TRUE(base.is_marked());
  EXPECT_TRUE(base.is_certified());
  EXPECT_TRUE(base.is_alert_fired());

  uint32_t bt = base.tag();
  auto r = base.with_state_and_next(M_LIVE, 9);

  EXPECT_EQ(static_cast<uint8_t>(M_LIVE), r.state());
  EXPECT_EQ(static_cast<uint16_t>(9), r.next());
  EXPECT_EQ(bt + 1, r.tag()); // Single bump.
  // All three reserved bits preserved.
  EXPECT_TRUE(r.is_marked());
  EXPECT_TRUE(r.is_certified());
  EXPECT_TRUE(r.is_alert_fired());
}

// --- 6. Free decoder shorthand --------------------------------------------
TEST(LlvmLibcLockFreeLinkageTest, FreeDecodersMatchClassAccessors) {
  auto l = linkage::Link::pack_marked(M_PARKED, 0xC0FFEEu, 4).certified();
  EXPECT_EQ(l.state(), linkage::link_state(l));
  EXPECT_EQ(l.tag(), linkage::link_tag(l));
  EXPECT_EQ(l.next(), linkage::link_next(l));
  EXPECT_EQ(l.is_marked(), linkage::link_is_marked(l));
  EXPECT_EQ(l.is_certified(), linkage::link_is_certified(l));
  EXPECT_EQ(l.is_alert_fired(), linkage::link_is_alert_fired(l));
}

// --- 7. link_load_state ----------------------------------------------------
TEST(LlvmLibcLockFreeLinkageTest, LinkLoadStateExtractsByte) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_PARKED, 0, 0),
                       MemoryOrder::RELEASE);
  EXPECT_EQ(static_cast<uint8_t>(M_PARKED),
            linkage::link_load_state(g_pool[0].link));
}

// --- 8. link_cas_state — From-mismatch fails, From-match succeeds ----------
TEST(LlvmLibcLockFreeLinkageTest, LinkCasStateRespectsFrom) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_LIVE, 0, 0),
                       MemoryOrder::RELEASE);

  // From=PARKED — mismatch, no write.
  bool ok =
      linkage::link_cas_state<M_PARKED, M_SIGNALED, PermissiveTraits>(
          g_pool[0].link);
  EXPECT_FALSE(ok);
  EXPECT_EQ(static_cast<uint8_t>(M_LIVE), load(0).state());
  EXPECT_EQ(static_cast<uint32_t>(0), load(0).tag());

  // From=LIVE — match, success.
  ok = linkage::link_cas_state<M_LIVE, M_PARKED, PermissiveTraits>(
      g_pool[0].link);
  EXPECT_TRUE(ok);
  EXPECT_EQ(static_cast<uint8_t>(M_PARKED), load(0).state());
  EXPECT_EQ(static_cast<uint32_t>(1), load(0).tag());
  EXPECT_FALSE(load(0).is_alert_fired()); // PermissiveTraits never fires
}

// --- 9. link_cas_state with alerting Traits --------------------------------
TEST(LlvmLibcLockFreeLinkageTest, LinkCasStateFiresAlertViaTraits) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_PARKED, 7, 3),
                       MemoryOrder::RELEASE);

  bool ok = linkage::link_cas_state<M_PARKED, M_SIGNALED, AlertingTraits>(
      g_pool[0].link);
  EXPECT_TRUE(ok);
  auto post = load(0);
  EXPECT_EQ(static_cast<uint8_t>(M_SIGNALED), post.state());
  EXPECT_TRUE(post.is_alert_fired());
  EXPECT_EQ(static_cast<uint16_t>(3), post.next()); // next preserved
  EXPECT_EQ(8u, post.tag());                        // single bump

  // Non-alerting transition (LIVE→SIGNALED) leaves bit clear.
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_LIVE, 0, 0),
                       MemoryOrder::RELEASE);
  ok = linkage::link_cas_state<M_LIVE, M_SIGNALED, AlertingTraits>(
      g_pool[0].link);
  EXPECT_TRUE(ok);
  EXPECT_FALSE(load(0).is_alert_fired());
}

// --- 10. link_cas_state_certify --------------------------------------------
TEST(LlvmLibcLockFreeLinkageTest, LinkCasStateCertifyPublishesCert) {
  reset_pool();
  // Pre-mark to verify MARK preserved across the certify CAS.
  g_pool[0].link.store(linkage::Link::pack_marked(M_LIVE, 5, 2),
                       MemoryOrder::RELEASE);

  bool ok =
      linkage::link_cas_state_certify<M_LIVE, M_PARKED, PermissiveTraits>(
          g_pool[0].link);
  EXPECT_TRUE(ok);
  auto post = load(0);
  EXPECT_EQ(static_cast<uint8_t>(M_PARKED), post.state());
  EXPECT_TRUE(post.is_certified());
  EXPECT_TRUE(post.is_marked()); // preserved through certify
  EXPECT_EQ(6u, post.tag());     // single bump
}

// --- 11. link_cas_snap -----------------------------------------------------
TEST(LlvmLibcLockFreeLinkageTest, LinkCasSnapStrongCasRejectsTagBumped) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_PARKED, 10, 4),
                       MemoryOrder::RELEASE);
  auto snap = load(0);

  // Concurrent mutator bumps tag (simulated single-threadedly).
  g_pool[0].link.store(snap.with_state(M_PARKED), MemoryOrder::RELEASE);

  // Snap is now stale — strong CAS must fail.
  bool ok = linkage::link_cas_snap<M_SIGNALED, PermissiveTraits>(g_pool[0].link,
                                                                  snap);
  EXPECT_FALSE(ok);
  EXPECT_EQ(static_cast<uint8_t>(M_PARKED), load(0).state());
}

TEST(LlvmLibcLockFreeLinkageTest,
     LinkCasSnapAlertingTraitsFiresOnParkedSnap) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_PARKED, 0, 0),
                       MemoryOrder::RELEASE);
  auto snap = load(0);

  bool ok = linkage::link_cas_snap<M_SIGNALED, AlertingTraits>(g_pool[0].link,
                                                                snap);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(load(0).is_alert_fired());
}

TEST(LlvmLibcLockFreeLinkageTest,
     LinkCasSnapAlertingTraitsSilentOnLiveSnap) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_LIVE, 0, 0),
                       MemoryOrder::RELEASE);
  auto snap = load(0);

  bool ok = linkage::link_cas_snap<M_SIGNALED, AlertingTraits>(g_pool[0].link,
                                                                snap);
  EXPECT_TRUE(ok);
  EXPECT_FALSE(load(0).is_alert_fired());
}

// `link_cas_snap_relink` strong-CAS publishes (state, next) atomically
// from a captured snap. The state byte transitions to To while the
// next-pointer advances; reserved bits preserved. This is the
// interval-skiplist Swap linearisation primitive (Kim et al. SOSP 2025
// Algorithm 2).
TEST(LlvmLibcLockFreeLinkageTest, LinkCasSnapRelinkPublishesAtomically) {
  reset_pool();
  // Predecessor with reserved bits set, state LOCKED (M_PARKED used as
  // mock LOCKED), pointing at the soon-to-be-stale successor index 4.
  auto seed = linkage::Link::pack_marked(M_PARKED, 0, 4).certified();
  g_pool[0].link.store(seed, MemoryOrder::RELEASE);
  auto snap = load(0);
  uint32_t snap_tag = snap.tag();

  // Swap commits LIVE + new successor 7 in one CAS.
  bool ok = linkage::link_cas_snap_relink<M_LIVE, PermissiveTraits>(
      g_pool[0].link, snap, /*new_next=*/7);
  EXPECT_TRUE(ok);

  auto post = load(0);
  EXPECT_EQ(static_cast<uint8_t>(M_LIVE), post.state());
  EXPECT_EQ(static_cast<uint16_t>(7), post.next());
  EXPECT_EQ(snap_tag + 1, post.tag()); // Single bump.
  // Reserved bits preserved.
  EXPECT_TRUE(post.is_marked());
  EXPECT_TRUE(post.is_certified());
}

// Strong CAS must reject any concurrent mutation that bumps the tag
// between snap capture and the CAS — otherwise Swap could clobber
// another writer's progress.
TEST(LlvmLibcLockFreeLinkageTest, LinkCasSnapRelinkRejectsTagBumped) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_PARKED, 50, 4),
                       MemoryOrder::RELEASE);
  auto snap = load(0);

  // Concurrent mutation bumps tag.
  g_pool[0].link.store(snap.with_state(M_PARKED), MemoryOrder::RELEASE);

  bool ok = linkage::link_cas_snap_relink<M_LIVE, PermissiveTraits>(
      g_pool[0].link, snap, /*new_next=*/7);
  EXPECT_FALSE(ok);
  // Original successor preserved (the post-mutation value, next=4).
  EXPECT_EQ(static_cast<uint16_t>(4), load(0).next());
}

// --- 12. link_cas_state_detached ------------------------------------------
TEST(LlvmLibcLockFreeLinkageTest, LinkCasStateDetachedReturnsOldOnSuccess) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_PARKED, 0, 0),
                       MemoryOrder::RELEASE);
  auto snap = load(0);

  bool ours = false;
  uint8_t prior =
      linkage::link_cas_state_detached<M_SIGNALED, AlertingTraits>(
          g_pool[0].link, snap, ours);
  EXPECT_TRUE(ours);
  EXPECT_EQ(static_cast<uint8_t>(M_PARKED), prior);
  auto post = load(0);
  EXPECT_EQ(static_cast<uint8_t>(M_SIGNALED), post.state());
  EXPECT_TRUE(post.is_certified());
  EXPECT_TRUE(post.is_alert_fired()); // PARKED→SIGNALED via AlertingTraits
}

TEST(LlvmLibcLockFreeLinkageTest,
     LinkCasStateDetachedReturnsCurrentOnFailure) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_PARKED, 0, 0),
                       MemoryOrder::RELEASE);
  auto snap = load(0);
  // Race: external mutator transitions IDLE.
  g_pool[0].link.store(linkage::Link::pack(M_IDLE, 0, 0).certified(),
                       MemoryOrder::RELEASE);

  bool ours = true; // sentinel — must be set false on failure.
  uint8_t cur = linkage::link_cas_state_detached<M_SIGNALED, AlertingTraits>(
      g_pool[0].link, snap, ours);
  EXPECT_FALSE(ours);
  EXPECT_EQ(static_cast<uint8_t>(M_IDLE), cur);
}

// --- 13. link_exchange_state / link_exchange_state_certify -----------------
TEST(LlvmLibcLockFreeLinkageTest, LinkExchangeStateReturnsPriorState) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack_marked(M_LIVE, 5, 1),
                       MemoryOrder::RELEASE);
  uint8_t prior =
      linkage::link_exchange_state(g_pool[0].link, M_PARKED);
  EXPECT_EQ(static_cast<uint8_t>(M_LIVE), prior);
  auto post = load(0);
  EXPECT_EQ(static_cast<uint8_t>(M_PARKED), post.state());
  EXPECT_TRUE(post.is_marked()); // preserved
}

TEST(LlvmLibcLockFreeLinkageTest, LinkExchangeStateCertifySetsCert) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack_marked(M_LIVE, 5, 1),
                       MemoryOrder::RELEASE);
  uint8_t prior =
      linkage::link_exchange_state_certify(g_pool[0].link, M_IDLE);
  EXPECT_EQ(static_cast<uint8_t>(M_LIVE), prior);
  auto post = load(0);
  EXPECT_EQ(static_cast<uint8_t>(M_IDLE), post.state());
  EXPECT_TRUE(post.is_certified());
  EXPECT_TRUE(post.is_marked()); // preserved (caller is reclaimer; walker
                                 //             may still hold splice)
}

// --- 14. MarkedLinkSnap typed contract ------------------------------------
TEST(LlvmLibcLockFreeLinkageTest, LinkPackAfterMarkProducesMarkedSnap) {
  auto unmarked = linkage::Link::pack(M_LIVE, 1, 2);
  auto snap = linkage::link_pack_after_mark(unmarked);
  EXPECT_TRUE(snap.link.is_marked());
  EXPECT_EQ(unmarked.tag() + 1, snap.link.tag());
  EXPECT_EQ(unmarked.state(), snap.link.state());
  EXPECT_EQ(unmarked.next(), snap.link.next());

  // from_observed_marked must accept a marked link.
  auto obs =
      linkage::MarkedLinkSnap::from_observed_marked(unmarked.marked());
  EXPECT_TRUE(obs.link.is_marked());
}

// --- 15. link_finalize_after_splice — atomic CERT+!MARK publish -----------
TEST(LlvmLibcLockFreeLinkageTest, LinkFinalizeAfterSpliceCommitsAtomically) {
  reset_pool();
  // Walker has marked the slot; pack the post-mark snap.
  g_pool[0].link.store(linkage::Link::pack(M_LIVE, 5, 3),
                       MemoryOrder::RELEASE);
  bool marked = linkage::link_cas_set_mark(g_pool[0].link, load(0));
  EXPECT_TRUE(marked);
  auto post_mark = load(0);
  EXPECT_TRUE(post_mark.is_marked());

  auto snap = linkage::MarkedLinkSnap::from_observed_marked(post_mark);
  linkage::link_finalize_after_splice(g_pool[0].link, snap);
  auto post = load(0);
  EXPECT_FALSE(post.is_marked());
  EXPECT_TRUE(post.is_certified());
  EXPECT_EQ(post_mark.state(), post.state()); // state preserved
  EXPECT_EQ(post_mark.next(), post.next());   // next preserved
  EXPECT_EQ(post_mark.tag() + 1, post.tag()); // single bump
}

TEST(LlvmLibcLockFreeLinkageTest, LinkFinalizeBailsOnConcurrentMutation) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_LIVE, 5, 3),
                       MemoryOrder::RELEASE);
  (void)linkage::link_cas_set_mark(g_pool[0].link, load(0));
  auto snap = linkage::MarkedLinkSnap::from_observed_marked(load(0));

  // Racing mutator changes the link past our snap.
  g_pool[0].link.store(linkage::Link::pack(M_PARKED, 9, 3).certified(),
                       MemoryOrder::RELEASE);

  // Finalize is bail-on-fail — the CAS misses, no retry, no spurious
  // CERT publish on the new lifecycle.
  linkage::link_finalize_after_splice(g_pool[0].link, snap);
  auto post = load(0);
  EXPECT_EQ(static_cast<uint8_t>(M_PARKED), post.state());
  EXPECT_TRUE(post.is_certified());
  EXPECT_FALSE(post.is_marked()); // cleared by the racing store
}

// --- 16. link_clear_mark ---------------------------------------------------
TEST(LlvmLibcLockFreeLinkageTest, LinkClearMarkLoopsAndExits) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack_marked(M_LIVE, 1, 0).certified(),
                       MemoryOrder::RELEASE);
  linkage::link_clear_mark(g_pool[0].link);
  auto post = load(0);
  EXPECT_FALSE(post.is_marked());
  EXPECT_TRUE(post.is_certified()); // preserved
  EXPECT_EQ(static_cast<uint8_t>(M_LIVE), post.state());
}

TEST(LlvmLibcLockFreeLinkageTest, LinkClearMarkNoopWhenAlreadyClear) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_LIVE, 1, 0).certified(),
                       MemoryOrder::RELEASE);
  uint32_t pre_tag = load(0).tag();
  linkage::link_clear_mark(g_pool[0].link);
  EXPECT_EQ(pre_tag, load(0).tag()); // no CAS issued
}

// --- 17. link_field_set_cert ----------------------------------------------
TEST(LlvmLibcLockFreeLinkageTest, LinkFieldSetCertIdempotentNoTagBump) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_LIVE, 50, 7),
                       MemoryOrder::RELEASE);
  uint32_t pre_tag = load(0).tag();
  linkage::link_field_set_cert(g_pool[0].link);
  auto post = load(0);
  EXPECT_TRUE(post.is_certified());
  EXPECT_EQ(pre_tag, post.tag()); // fetch_or doesn't bump
  EXPECT_EQ(static_cast<uint8_t>(M_LIVE), post.state());
  EXPECT_EQ(static_cast<uint16_t>(7), post.next());

  // Idempotent.
  linkage::link_field_set_cert(g_pool[0].link);
  EXPECT_TRUE(load(0).is_certified());
  EXPECT_EQ(pre_tag, load(0).tag());
}

// --- 18. link_store / link_store_next / link_store_state ------------------
TEST(LlvmLibcLockFreeLinkageTest, LinkStoreSetsStateNextCertDropsMark) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack_marked(M_LIVE, 5, 1),
                       MemoryOrder::RELEASE);
  linkage::link_store(g_pool[0].link, M_IDLE, 9);
  auto post = load(0);
  EXPECT_EQ(static_cast<uint8_t>(M_IDLE), post.state());
  EXPECT_EQ(static_cast<uint16_t>(9), post.next());
  EXPECT_TRUE(post.is_certified());
  EXPECT_FALSE(post.is_marked());
  EXPECT_EQ(6u, post.tag());
}

TEST(LlvmLibcLockFreeLinkageTest, LinkStoreNextPreservesStateAndCert) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_LIVE, 5, 1).certified(),
                       MemoryOrder::RELEASE);
  linkage::link_store_next(g_pool[0].link, 12);
  auto post = load(0);
  EXPECT_EQ(static_cast<uint8_t>(M_LIVE), post.state());
  EXPECT_EQ(static_cast<uint16_t>(12), post.next());
  EXPECT_TRUE(post.is_certified()); // preserved
  EXPECT_FALSE(post.is_marked());
}

TEST(LlvmLibcLockFreeLinkageTest, LinkStoreStatePreservesNextAndCert) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_LIVE, 5, 7).certified(),
                       MemoryOrder::RELEASE);
  linkage::link_store_state(g_pool[0].link, M_PARKED);
  auto post = load(0);
  EXPECT_EQ(static_cast<uint8_t>(M_PARKED), post.state());
  EXPECT_EQ(static_cast<uint16_t>(7), post.next()); // preserved
  EXPECT_TRUE(post.is_certified());
}

TEST(LlvmLibcLockFreeLinkageTest, LinkStoreNextUncertifyClearsThreeBits) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack_marked(M_LIVE, 5, 1).certified(),
                       MemoryOrder::RELEASE);
  g_pool[0].link.store(
      linkage::Link::from_raw(load(0).raw() | linkage::LINK_ALERT_FIRED_BIT),
      MemoryOrder::RELEASE);

  linkage::link_store_next_uncertify(g_pool[0].link, 11);
  auto post = load(0);
  EXPECT_EQ(static_cast<uint16_t>(11), post.next());
  EXPECT_FALSE(post.is_marked());
  EXPECT_FALSE(post.is_certified());
  EXPECT_FALSE(post.is_alert_fired());
}

// --- 19. link_cas_next — single-thread parity with link_store_next --------
TEST(LlvmLibcLockFreeLinkageTest, LinkCasNextSingleThreadMatchesLinkStoreNext) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_LIVE, 5, 1).certified(),
                       MemoryOrder::RELEASE);
  linkage::link_cas_next(g_pool[0].link, 12);
  auto post = load(0);
  EXPECT_EQ(static_cast<uint8_t>(M_LIVE), post.state());
  EXPECT_EQ(static_cast<uint16_t>(12), post.next());
  EXPECT_TRUE(post.is_certified());
  EXPECT_FALSE(post.is_marked());
}

// --- 20. Multi-threaded ABA defense ---------------------------------------
//
// N threads race link_cas_state from M_LIVE → M_PARKED → M_LIVE in a tight
// loop. The substrate's tag-bump invariant must yield monotonically growing
// tags with no spurious stale-snap CAS wins (which would produce a tag
// regression). Each thread accumulates the "saw a regression" flag — must
// remain false for the entire run.
namespace {
constexpr uint32_t kAbaIterations = 50000;
constexpr uint32_t kAbaWorkers = 4;

struct AbaCtx {
  Atomic<uint64_t> last_tag{0};
  Atomic<uint32_t> regressions{0};
  Atomic<uint32_t> ready{0};
  Atomic<uint32_t> start{0};
};

LIBC_MSABI DWORD aba_worker(void *arg) {
  auto *ctx = static_cast<AbaCtx *>(arg);
  ctx->ready.fetch_add(1, MemoryOrder::ACQ_REL);
  while (ctx->start.load(MemoryOrder::ACQUIRE) == 0) {
    // spin
  }
  for (uint32_t i = 0; i < kAbaIterations; ++i) {
    // LIVE → PARKED
    bool ok =
        linkage::link_cas_state<M_LIVE, M_PARKED, PermissiveTraits>(
            g_pool[0].link);
    if (ok) {
      uint64_t tag_now = load(0).tag();
      uint64_t prior =
          ctx->last_tag.exchange(tag_now, MemoryOrder::ACQ_REL);
      if (tag_now <= prior)
        ctx->regressions.fetch_add(1, MemoryOrder::RELAXED);
    }
    // PARKED → LIVE
    ok = linkage::link_cas_state<M_PARKED, M_LIVE, PermissiveTraits>(
        g_pool[0].link);
    if (ok) {
      uint64_t tag_now = load(0).tag();
      uint64_t prior =
          ctx->last_tag.exchange(tag_now, MemoryOrder::ACQ_REL);
      if (tag_now <= prior)
        ctx->regressions.fetch_add(1, MemoryOrder::RELAXED);
    }
  }
  return 0;
}
} // namespace

TEST(LlvmLibcLockFreeLinkageTest, MultiThreadedTagMonotonicityNoRegressions) {
  reset_pool();
  g_pool[0].link.store(linkage::Link::pack(M_LIVE, 0, 0),
                       MemoryOrder::RELEASE);
  AbaCtx ctx;

  HANDLE threads[kAbaWorkers] = {};
  for (uint32_t i = 0; i < kAbaWorkers; ++i) {
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(&aba_worker, &ctx);
    ASSERT_TRUE(LIBC_NAMESPACE::test_support::is_valid_handle(threads[i]));
  }
  // Wait for all workers to be parked at the start gate.
  while (ctx.ready.load(MemoryOrder::ACQUIRE) != kAbaWorkers) {
    LIBC_NAMESPACE::test_support::sleep_ms(1);
  }
  // Release.
  ctx.start.store(1, MemoryOrder::RELEASE);

  for (uint32_t i = 0; i < kAbaWorkers; ++i) {
    LIBC_NAMESPACE::test_support::wait_for_single_object(
        threads[i], LIBC_NAMESPACE::test_support::INFINITE);
    ::NtClose(threads[i]);
  }

  EXPECT_EQ(0u, ctx.regressions.load(MemoryOrder::ACQUIRE));
  // Tag must be at least one bump per successful CAS — bound it loosely.
  EXPECT_GT(load(0).tag(), 100u);
}
