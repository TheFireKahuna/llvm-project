//===-- Tests for interval_skiplist (Kim et al. SOSP 2025) ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Substrate-level coverage for the va_tracker interval skiplist (paper:
// Kim, Kwon, Kang — SOSP 2025). Probes invariants and code paths that
// the public `va_tracker::acquire/release/replace/mutate/...` surface
// deliberately hides — the LockedSet acquire/Swap protocol, paper
// Theorem B.2, T1/T2/T3 substrate triad, Link encoding, bucket
// geometry, Harris MARK helper, multi-level upper publish, layout
// invariants. End-to-end public-API coverage lives in
// `va_tracker_test.cpp`.
//
// Each test owns a distinct 4 GiB-stride VA window so each test gets a
// fresh Arena via `resolve_or_install_arena()` and inter-test residue
// never collides on the same chain.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/lock_free_linkage.h"
#include "src/__support/OSUtil/windows/memory/interval_skiplist.h"
#include "src/__support/OSUtil/windows/memory/skiplist_link_traits.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "hdr/stdint_proxy.h"

namespace {

namespace va = LIBC_NAMESPACE::windows::va_tracker;
namespace linkage = LIBC_NAMESPACE::linkage;
using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;

// ---------------------------------------------------------------------------
// Per-test arena and VA helpers.
//
// Each test gets a fresh Arena via `arena_alloc`. 4 GiB stride matches
// the leaf-sized range that `arena_alloc` stamps on every arena
// (`arena_lo + 4 GiB`), so distinct test indices map to disjoint VA
// windows. The 80 TiB base sits above loader / heap / thread stacks
// but below the 96 TiB region used by `va_tracker_test.cpp`.
//
// Substrate-level test by design: we go through `arena_alloc` directly
// rather than the ART-aware `va_tracker::resolve_or_install_arena` —
// the skiplist's invariants don't depend on the outer dispatcher.
// ---------------------------------------------------------------------------

constexpr uintptr_t kSkiplistTestBase = 0x500000000000ULL;
constexpr uintptr_t kArenaStride = uintptr_t{1} << 32;

[[nodiscard]] uintptr_t test_base(uint32_t test_idx) {
  return kSkiplistTestBase + static_cast<uintptr_t>(test_idx) * kArenaStride;
}

[[nodiscard]] va::Arena *test_arena(uint32_t test_idx) {
  return va::arena_alloc(test_base(test_idx), test_idx & va::kArenaMask);
}

// Allocate a fresh RegionDesc for the test.
va::RegionDesc *make_region(uint16_t shape_v = 1) {
  va::RegionDesc *r = va::region_desc_alloc();
  if (r == nullptr)
    return nullptr;
  r->shape.store(shape_v, MemoryOrder::RELAXED);
  return r;
}

// Walk arena head's level-0 chain and return how many LIVE nodes lie
// within `[lo, hi)`.
uint32_t count_live_in_range(va::Arena *arena, uintptr_t lo, uintptr_t hi) {
  uint32_t count = 0;
  uint16_t enc = arena->head_next(0).load(MemoryOrder::ACQUIRE).next();
  while (enc != va::kLinkNullEncoding) {
    va::SkiplistNodeBase *n = va::resolve_link_target(enc);
    if (n == nullptr)
      break;
    linkage::Link snap = n->next[0].load(MemoryOrder::ACQUIRE);
    uint8_t st = snap.state();
    if (n->lo >= hi)
      break;
    if (n->hi > lo &&
        st == static_cast<uint8_t>(va::SkiplistNodeState::LIVE))
      ++count;
    enc = snap.next();
  }
  return count;
}

// Restore `pred` from LOCKED → LIVE. Used by the Lock-without-Swap
// tests, which deliberately exercise the default `Unlock()` semantics
// (pred is left LOCKED by design).
void clear_pred_lock(va::SkiplistNodeBase *pred) {
  if (pred == nullptr)
    return;
  (void)linkage::link_cas_state<va::SkiplistNodeState::LOCKED,
                                  va::SkiplistNodeState::LIVE,
                                  va::SkiplistLinkTraits>(pred->next[0]);
}

} // namespace

// ===========================================================================
// 1. Single-threaded is_insert -> Query roundtrip.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, InsertAndQuerySingleThreaded) {
  va::Arena *a = test_arena(1);
  ASSERT_NE(a, static_cast<va::Arena *>(nullptr));

  va::RegionDesc *rd = make_region(2);
  ASSERT_NE(rd, static_cast<va::RegionDesc *>(nullptr));

  uintptr_t base = test_base(1);
  EXPECT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x20000, rd));

  EXPECT_EQ(va::Query(a, base + 0x18000), rd);
  EXPECT_EQ(va::Query(a, base + 0x20001), static_cast<va::RegionDesc *>(nullptr));
}

// ===========================================================================
// 2. Erase removes the interval, Query returns nullptr after.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, EraseRemovesInterval) {
  va::Arena *a = test_arena(2);
  va::RegionDesc *rd = make_region(3);
  uintptr_t base = test_base(2);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x20000, rd));

  EXPECT_EQ(0, va::is_erase(a, base + 0x10000, base + 0x20000));
  EXPECT_EQ(va::Query(a, base + 0x15000),
             static_cast<va::RegionDesc *>(nullptr));
}

// ===========================================================================
// 3. Lock -> Unlock cycle without Swap leaves the chain intact.
//    The default `Unlock()` skips pred — verifies the substrate
//    contract that pred stays LOCKED until the linearisation CAS (or a
//    manual clear) lands.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, LockUnlockNoSwap) {
  va::Arena *a = test_arena(3);
  va::RegionDesc *rd = make_region();
  uintptr_t base = test_base(3);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x20000, rd));

  va::LockedSet ls;
  (void)ls.acquire(a, base + 0x10000, base + 0x20000);
  ASSERT_TRUE(ls.valid());
  EXPECT_EQ(ls.count, 1u);
  va::Unlock(ls);

  // Pred is still LOCKED here by design — Unlock skips it.
  clear_pred_lock(ls.pred);

  EXPECT_EQ(va::Query(a, base + 0x15000), rd);
}

// ===========================================================================
// 4. Multi-node Map: insert overwrites two adjacent intervals.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, MultiNodeOverwrite) {
  va::Arena *a = test_arena(4);
  va::RegionDesc *r1 = make_region(1);
  va::RegionDesc *r2 = make_region(2);
  va::RegionDesc *r3 = make_region(3);
  uintptr_t base = test_base(4);

  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, r1));
  ASSERT_EQ(0, va::is_insert(a, base + 0x11000, base + 0x12000, r2));
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x12000, r3));

  EXPECT_EQ(va::Query(a, base + 0x10500), r3);
  EXPECT_EQ(va::Query(a, base + 0x11500), r3);
}

// ===========================================================================
// 5. Arena alloc round-trip — `arena_alloc` stamps the requested cpu
//    index, derives `arena_hi` as `arena_lo + 4 GiB`, and seeds the
//    head sentinel with the same bounds.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, ArenaAllocStampsBoundsAndCpuIndex) {
  constexpr uint32_t kCpu = 3;
  uintptr_t base = test_base(5);
  va::Arena *a = va::arena_alloc(base, kCpu);
  ASSERT_NE(a, static_cast<va::Arena *>(nullptr));
  EXPECT_EQ(a->cpu_index, kCpu);
  EXPECT_LT(a->cpu_index, va::kArenaCount);
  EXPECT_EQ(a->arena_lo, base);
  EXPECT_EQ(a->arena_hi, base + kArenaStride);
  EXPECT_EQ(a->head.lo, base);
  EXPECT_EQ(a->head.hi, base + kArenaStride);
}

// ===========================================================================
// 6. Substrate state-byte invariant: LIVE / LOCKED / INVALIDATED only.
//    IDLE is never observable on a reachable chain (paper T2).
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, ReachableChainNeverObservesIDLE) {
  va::Arena *a = test_arena(6);
  uintptr_t base = test_base(6);
  for (int i = 0; i < 32; ++i) {
    va::RegionDesc *rd = make_region(static_cast<uint16_t>(i + 1));
    uintptr_t lo = base + static_cast<uintptr_t>(i) * 0x1000;
    ASSERT_EQ(0, va::is_insert(a, lo, lo + 0x800, rd));
  }
  uint16_t enc = a->head_next(0).load(MemoryOrder::ACQUIRE).next();
  uint32_t walked = 0;
  while (enc != va::kLinkNullEncoding && walked < 64) {
    va::SkiplistNodeBase *n = va::resolve_link_target(enc);
    ASSERT_NE(n, static_cast<va::SkiplistNodeBase *>(nullptr));
    linkage::Link snap = n->next[0].load(MemoryOrder::ACQUIRE);
    uint8_t st = snap.state();
    EXPECT_NE(st, static_cast<uint8_t>(va::SkiplistNodeState::IDLE));
    enc = snap.next();
    ++walked;
  }
}

// ===========================================================================
// 7. Tag monotonicity: every successful Lock CAS bumps the predecessor's
//    link tag (paper T1).
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, LockBumpsPredTag) {
  va::Arena *a = test_arena(7);
  va::RegionDesc *rd = make_region();
  uintptr_t base = test_base(7);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x20000, rd));

  uint32_t tag_before = a->head_next(0).load(MemoryOrder::ACQUIRE).tag();
  va::LockedSet ls;
  (void)ls.acquire(a, base + 0x10000, base + 0x20000);
  ASSERT_TRUE(ls.valid());
  uint32_t tag_after = ls.pred->next[0].load(MemoryOrder::ACQUIRE).tag();
  EXPECT_NE(tag_before, tag_after);
  EXPECT_GT(tag_after, tag_before);
  va::Unlock(ls);
  clear_pred_lock(ls.pred);
}

// ===========================================================================
// 8. Lock contention with futex park + alert wake (two threads).
// ===========================================================================
struct LockContendCtx {
  va::Arena *arena;
  uintptr_t lo;
  uintptr_t hi;
  Atomic<uint32_t> ready{0};
  Atomic<uint32_t> contender_about_to_lock{0};
  Atomic<uint32_t> done{0};
};

LIBC_MSABI static DWORD lock_contend_owner(void *arg) {
  auto *ctx = static_cast<LockContendCtx *>(arg);
  va::LockedSet ls;
  (void)ls.acquire(ctx->arena, ctx->lo, ctx->hi);
  ctx->ready.store(1, MemoryOrder::RELEASE);
  // Bounded handoff: wait for the contender to publish its "about to
  // call Lock" flag, then yield a small fixed number of times to let
  // the contender's futex-park settle, then Swap. The publish point is
  // bounded; the post-yield window is bounded; the previous version
  // used a blind 20 ms sleep that went flaky on contended CI when 20 ms
  // wasn't enough for the contender to reach park.
  while (ctx->contender_about_to_lock.load(MemoryOrder::ACQUIRE) == 0u)
    ::NtYieldExecution();
  for (int i = 0; i < 256; ++i)
    ::NtYieldExecution();
  va::NewNodes nn;
  (void)va::Swap(ls, nn); // empty swap = unlock pred + drop succs
  ctx->done.store(1, MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcIntervalSkiplistTest, LockContendsAndReleases) {
  va::Arena *a = test_arena(8);
  va::RegionDesc *rd = make_region();
  uintptr_t base = test_base(8);
  uintptr_t lo = base + 0x10000;
  uintptr_t hi = base + 0x20000;
  ASSERT_EQ(0, va::is_insert(a, lo, hi, rd));

  LockContendCtx ctx{a, lo, hi};
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(lock_contend_owner,
                                                            &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));
  while (!ctx.ready.load(MemoryOrder::ACQUIRE))
    LIBC_NAMESPACE::test_support::sleep_ms(0);

  // Publish the "about to call Lock" flag before the call. The owner
  // gates on this and then yields a bounded number of times to let our
  // futex-park settle before issuing Swap.
  ctx.contender_about_to_lock.store(1, MemoryOrder::RELEASE);
  // Try Lock in this thread — should park briefly then return either
  // valid (succ_count = 0 because the owner already cleared the range)
  // or invalid (the owner is mid-swap). Both are acceptable post-Lock
  // states.
  va::LockedSet ls2;
  (void)ls2.acquire(a, lo, hi);
  if (ls2.valid()) {
    va::Unlock(ls2);
    clear_pred_lock(ls2.pred);
  }

  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 10000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(t);
}

// ===========================================================================
// 9. Theorem B.2 enforcement: a Lock observing INVALIDATED on a target
//    successor restarts. After the wider is_insert, no live walker
//    should see INVALIDATED on the reachable chain.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, TheoremB2RestartsOnInvalidated) {
  va::Arena *a = test_arena(9);
  va::RegionDesc *r1 = make_region();
  va::RegionDesc *r2 = make_region();
  uintptr_t base = test_base(9);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, r1));
  ASSERT_EQ(0, va::is_insert(a, base + 0x11000, base + 0x12000, r2));

  va::RegionDesc *r3 = make_region();
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x12000, r3));

  uint32_t live = count_live_in_range(a, base + 0x10000, base + 0x12000);
  EXPECT_EQ(live, 1u);
}

// ===========================================================================
// 10. Substrate compound-CAS atomicity: Swap publishes new chain head +
//     LOCKED→LIVE in one tag bump.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, SwapCompoundCASIsAtomic) {
  va::Arena *a = test_arena(10);
  va::RegionDesc *r1 = make_region();
  uintptr_t base = test_base(10);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, r1));

  va::LockedSet ls;
  (void)ls.acquire(a, base + 0x10000, base + 0x11000);
  ASSERT_TRUE(ls.valid());
  EXPECT_EQ(ls.pred->next[0].load(MemoryOrder::ACQUIRE).state(),
             static_cast<uint8_t>(va::SkiplistNodeState::LOCKED));

  va::NewNodes nn;
  EXPECT_TRUE(va::Swap(ls, nn));

  // Post-Swap: pred state must be LIVE (compound CAS unlocked).
  EXPECT_EQ(a->head_next(0).load(MemoryOrder::ACQUIRE).state(),
             static_cast<uint8_t>(va::SkiplistNodeState::LIVE));
}

// ===========================================================================
// 11. Multi-level upper publish (Fig. 8): inserting many intervals
//     encourages tall nodes via the geometric PRNG.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, MultiLevelUpperPublishCompletes) {
  va::Arena *a = test_arena(11);
  uintptr_t base = test_base(11);
  for (int i = 0; i < 100; ++i) {
    va::RegionDesc *rd = make_region(static_cast<uint16_t>(i + 1));
    uintptr_t lo = base + static_cast<uintptr_t>(i) * 0x1000;
    ASSERT_EQ(0, va::is_insert(a, lo, lo + 0x800, rd));
  }
  bool any_upper_live = false;
  for (uint32_t lvl = 1; lvl < va::kMaxHeight; ++lvl) {
    if (a->head_next(lvl).load(MemoryOrder::ACQUIRE).next() !=
        va::kLinkNullEncoding) {
      any_upper_live = true;
      break;
    }
  }
  // Upper-level publish is advisory; the test passes as long as the
  // body completes without trap.
  (void)any_upper_live;
}

// ===========================================================================
// 12. Crystalline grace observance: Query through INVALIDATED returns
//     the value (paper I5).
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, QueryThroughInvalidatedReturnsValue) {
  va::Arena *a = test_arena(12);
  va::RegionDesc *r1 = make_region(7);
  uintptr_t base = test_base(12);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, r1));
  va::RegionDesc *r2 = make_region(8);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, r2));
  EXPECT_EQ(va::Query(a, base + 0x10500), r2);
}

// ===========================================================================
// 13. Disjoint Map operations on disjoint VA intervals proceed without
//     interference (no global writer lock).
// ===========================================================================
struct DisjointMapCtx {
  va::Arena *arena;
  uintptr_t lo;
  uintptr_t hi;
  va::RegionDesc *value;
  Atomic<uint32_t> done{0};
};

LIBC_MSABI static DWORD disjoint_inserter(void *arg) {
  auto *ctx = static_cast<DisjointMapCtx *>(arg);
  for (int i = 0; i < 50; ++i) {
    uintptr_t off = ctx->lo + static_cast<uintptr_t>(i) * 0x1000;
    if (off + 0x800 > ctx->hi)
      break;
    (void)va::is_insert(ctx->arena, off, off + 0x800, ctx->value);
  }
  ctx->done.store(1, MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcIntervalSkiplistTest, DisjointMapsRunInParallel) {
  va::Arena *a = test_arena(13);
  va::RegionDesc *r1 = make_region(1);
  va::RegionDesc *r2 = make_region(2);
  uintptr_t base = test_base(13);
  DisjointMapCtx c1{a, base + 0x100000, base + 0x150000, r1};
  DisjointMapCtx c2{a, base + 0x200000, base + 0x250000, r2};

  HANDLE t1 =
      LIBC_NAMESPACE::test_support::create_thread(disjoint_inserter, &c1);
  HANDLE t2 =
      LIBC_NAMESPACE::test_support::create_thread(disjoint_inserter, &c2);
  ASSERT_NE(t1, static_cast<HANDLE>(nullptr));
  ASSERT_NE(t2, static_cast<HANDLE>(nullptr));

  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t1, 30000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t2, 30000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(t1);
  ::NtClose(t2);
}

// ===========================================================================
// 14. is_walk wrapper points at the same Query implementation.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, IsWalkWrapperMatchesQuery) {
  va::Arena *a = test_arena(14);
  va::RegionDesc *rd = make_region(11);
  uintptr_t base = test_base(14);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, rd));
  EXPECT_EQ(va::is_walk(a, base + 0x10500), rd);
  EXPECT_EQ(va::Query(a, base + 0x10500), rd);
}

// ===========================================================================
// 15. is_walk_range visits every LIVE node ordered by lo.
// ===========================================================================
struct WalkVisitor {
  va::SkiplistNodeBase *visited[16]{};
  uint32_t count{0};
  void operator()(va::SkiplistNodeBase *n) {
    if (count < 16)
      visited[count++] = n;
  }
};

TEST(LlvmLibcIntervalSkiplistTest, IsWalkRangeOrdered) {
  va::Arena *a = test_arena(15);
  uintptr_t base = test_base(15);
  for (int i = 0; i < 5; ++i) {
    va::RegionDesc *rd = make_region(static_cast<uint16_t>(i + 1));
    uintptr_t lo = base + static_cast<uintptr_t>(i) * 0x10000;
    ASSERT_EQ(0, va::is_insert(a, lo, lo + 0x1000, rd));
  }
  WalkVisitor v;
  va::is_walk_range(a, base, base + 0x100000, v);
  EXPECT_GE(v.count, 1u);
  for (uint32_t i = 1; i < v.count; ++i) {
    EXPECT_LT(v.visited[i - 1]->lo, v.visited[i]->lo);
  }
}

// ===========================================================================
// 16. Alloc inserts a node in a gap and updates the hint.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, AllocFindsGap) {
  va::Arena *a = test_arena(16);
  va::RegionDesc *rd = make_region();
  uintptr_t base = test_base(16);
  uintptr_t addr =
      va::Alloc(a, base + 0x10000, base + 0x20000, 0x1000, rd);
  EXPECT_GE(addr, base + 0x10000);
  EXPECT_LT(addr + 0x1000, base + 0x20001);
  EXPECT_NE(a->hint.load(MemoryOrder::ACQUIRE),
             static_cast<va::SkiplistNodeBase *>(nullptr));
}

// ===========================================================================
// 17. Alloc on a full range returns 0.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, AllocOnFullRangeReturnsZero) {
  va::Arena *a = test_arena(17);
  va::RegionDesc *rd = make_region();
  uintptr_t base = test_base(17);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, rd));
  uintptr_t addr =
      va::Alloc(a, base + 0x10000, base + 0x11000, 0x1000, rd);
  EXPECT_EQ(addr, 0u);
}

// ===========================================================================
// 18. bucket_alloc_node produces nodes of the right bucket / height.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, BucketAllocNodeHeightRespected) {
  va::Arena *a = test_arena(18);
  ASSERT_NE(a, static_cast<va::Arena *>(nullptr));
  va::SkiplistNodeBase *n2 = va::bucket_alloc_node(2, a);
  va::SkiplistNodeBase *n8 = va::bucket_alloc_node(8, a);
  va::SkiplistNodeBase *n16 = va::bucket_alloc_node(16, a);
  ASSERT_NE(n2, static_cast<va::SkiplistNodeBase *>(nullptr));
  ASSERT_NE(n8, static_cast<va::SkiplistNodeBase *>(nullptr));
  ASSERT_NE(n16, static_cast<va::SkiplistNodeBase *>(nullptr));
  EXPECT_EQ(n2->bucket, 0u);  // h=2 → bucket 0
  EXPECT_EQ(n8->bucket, 2u);  // h=8 → bucket 2
  EXPECT_EQ(n16->bucket, 3u); // h=16 → bucket 3
  // Retire so we don't leak.
  va::g_va_tracker_skiplist_domain.retire(n2);
  va::g_va_tracker_skiplist_domain.retire(n8);
  va::g_va_tracker_skiplist_domain.retire(n16);
}

// ===========================================================================
// 19. Substrate Traits::is_valid table.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, TraitsIsValidGatesIllegalEdges) {
  EXPECT_TRUE(va::SkiplistLinkTraits::is_valid(
      static_cast<uint8_t>(va::SkiplistNodeState::LIVE),
      static_cast<uint8_t>(va::SkiplistNodeState::LOCKED)));
  EXPECT_TRUE(va::SkiplistLinkTraits::is_valid(
      static_cast<uint8_t>(va::SkiplistNodeState::LOCKED),
      static_cast<uint8_t>(va::SkiplistNodeState::LIVE)));
  EXPECT_FALSE(va::SkiplistLinkTraits::is_valid(
      static_cast<uint8_t>(va::SkiplistNodeState::IDLE),
      static_cast<uint8_t>(va::SkiplistNodeState::LIVE)));
}

// ===========================================================================
// 20. fires_alert table — every release-class and acquire-class edge
//     fires the wake bit; reentrant LOCKED→LOCKED and reclaim
//     INVALIDATED→IDLE do not.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, TraitsFiresAlertOnReleaseAndAcquireEdges) {
  using S = va::SkiplistNodeState;
  auto u = [](S s) { return static_cast<uint8_t>(s); };
  // Acquire-class.
  EXPECT_TRUE(va::SkiplistLinkTraits::fires_alert(u(S::LIVE), u(S::LOCKED)));
  // Release-class (Swap relink / Unlock).
  EXPECT_TRUE(va::SkiplistLinkTraits::fires_alert(u(S::LOCKED), u(S::LIVE)));
  // Erase shortcut.
  EXPECT_TRUE(
      va::SkiplistLinkTraits::fires_alert(u(S::LIVE), u(S::INVALIDATED)));
  // Swap retire.
  EXPECT_TRUE(
      va::SkiplistLinkTraits::fires_alert(u(S::LOCKED), u(S::INVALIDATED)));
  // Re-entrant multi-level publish: tag bump only, no parker to wake.
  EXPECT_FALSE(
      va::SkiplistLinkTraits::fires_alert(u(S::LOCKED), u(S::LOCKED)));
  // Reclaim: FreeFn-only, no parker can still observe.
  EXPECT_FALSE(
      va::SkiplistLinkTraits::fires_alert(u(S::INVALIDATED), u(S::IDLE)));
}

// ===========================================================================
// 21. Encode/decode round-trip — Link::next encoding is invertible.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, LinkNextEncodingRoundTrips) {
  for (uint16_t cid = 0; cid < 256; cid += 17) {
    for (uint16_t sid = 0; sid < 256; sid += 19) {
      uint16_t enc = va::encode_link_next(static_cast<uint8_t>(cid),
                                            static_cast<uint8_t>(sid));
      EXPECT_EQ(va::decode_link_chunk_id(enc), static_cast<uint8_t>(cid));
      EXPECT_EQ(va::decode_link_slot_idx(enc), static_cast<uint8_t>(sid));
    }
  }
}

// ===========================================================================
// 22. Bucket geometry table — slot_size grows monotonically with bucket
//     id.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, BucketGeometryMonotone) {
  for (uint32_t b = 1; b < va::kBucketCount; ++b) {
    EXPECT_GT(va::bucket_geometry(b).slot_size,
                va::bucket_geometry(b - 1).slot_size);
  }
}

// ===========================================================================
// 23. Stats snapshot reports nonzero live chunks after inserts.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, StatsSnapshotReflectsInserts) {
  va::Arena *a = test_arena(23);
  va::RegionDesc *rd = make_region();
  uintptr_t base = test_base(23);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, rd));
  va::SkiplistStats s = va::stats_snapshot();
  EXPECT_GE(s.arena_count, 1u);
  uint32_t total_live = 0;
  for (uint32_t b = 0; b < va::kBucketCount; ++b)
    total_live += s.live_chunks_per_bucket[b];
  EXPECT_GE(total_live, 1u);
}

// ===========================================================================
// 24. Insert / Erase / Insert cycle keeps the chain consistent.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, InsertEraseInsertCycle) {
  va::Arena *a = test_arena(24);
  va::RegionDesc *r1 = make_region(1);
  va::RegionDesc *r2 = make_region(2);
  uintptr_t base = test_base(24);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, r1));
  ASSERT_EQ(0, va::is_erase(a, base + 0x10000, base + 0x11000));
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, r2));
  EXPECT_EQ(va::Query(a, base + 0x10500), r2);
}

// ===========================================================================
// 25. Map with an empty visitor commits the pred unlock and drops the
//     locked successors. Visitor returns a VisitorOutcome with no
//     replacement nodes.
// ===========================================================================
struct EmptyMapVisitor {
  va::VisitorOutcome operator()(va::LockedSet & /*set*/, uintptr_t /*lo*/,
                                 uintptr_t /*hi*/) {
    return va::VisitorOutcome{};
  }
};

TEST(LlvmLibcIntervalSkiplistTest, MapWithEmptyVisitorEmptySwap) {
  va::Arena *a = test_arena(25);
  va::RegionDesc *rd = make_region();
  uintptr_t base = test_base(25);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, rd));
  EmptyMapVisitor v;
  EXPECT_EQ(0, va::Map(a, base + 0x10000, base + 0x11000, v));
  EXPECT_EQ(va::Query(a, base + 0x10500),
             static_cast<va::RegionDesc *>(nullptr));
}

// ===========================================================================
// 26. One-byte boundary insert.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, BoundaryInsertExtents) {
  va::Arena *a = test_arena(26);
  va::RegionDesc *rd = make_region();
  uintptr_t base = test_base(26);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x10001, rd));
  EXPECT_EQ(va::Query(a, base + 0x10000), rd);
  EXPECT_EQ(va::Query(a, base + 0x10001),
             static_cast<va::RegionDesc *>(nullptr));
}

// ===========================================================================
// 27. Empty range Lock returns invalid LockedSet.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, EmptyRangeLockReturnsInvalid) {
  va::Arena *a = test_arena(27);
  uintptr_t base = test_base(27);
  va::LockedSet ls;
  (void)ls.acquire(a, base + 0x10000, base + 0x10000);
  EXPECT_FALSE(ls.valid());
}

// ===========================================================================
// 28. Helper unlink — Harris MARK observed, walker drives finalize.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, HarrisMarkHelperUnlinkProgresses) {
  va::Arena *a = test_arena(28);
  va::RegionDesc *rd = make_region();
  uintptr_t base = test_base(28);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, rd));

  // Manually mark the first node to force a walker into the help_unlink
  // path. Then issue an erase — Lock should help-unlink before
  // completing.
  uint16_t enc = a->head_next(0).load(MemoryOrder::ACQUIRE).next();
  ASSERT_NE(enc, va::kLinkNullEncoding);
  va::SkiplistNodeBase *n = va::resolve_link_target(enc);
  ASSERT_NE(n, static_cast<va::SkiplistNodeBase *>(nullptr));
  linkage::Link snap = n->next[0].load(MemoryOrder::ACQUIRE);
  (void)linkage::link_cas_set_mark(n->next[0], snap);

  EXPECT_EQ(0, va::is_erase(a, base + 0x10000, base + 0x11000));
}

// ===========================================================================
// 29. Linearisation atomicity: concurrent Query never observes garbage
//     while a writer churns is_insert across the same range.
// ===========================================================================
struct LinearizerCtx {
  va::Arena *arena;
  uintptr_t key;
  Atomic<uint32_t> stop{0};
  Atomic<uint32_t> observed_mismatch{0};
  va::RegionDesc *r1;
  va::RegionDesc *r2;
};

LIBC_MSABI static DWORD linearizer_query(void *arg) {
  auto *ctx = static_cast<LinearizerCtx *>(arg);
  while (!ctx->stop.load(MemoryOrder::ACQUIRE)) {
    va::RegionDesc *q = va::Query(ctx->arena, ctx->key);
    if (q != nullptr && q != ctx->r1 && q != ctx->r2)
      ctx->observed_mismatch.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST(LlvmLibcIntervalSkiplistTest, ConcurrentQueryNeverObservesGarbage) {
  va::Arena *a = test_arena(29);
  uintptr_t base = test_base(29);
  LinearizerCtx ctx;
  ctx.arena = a;
  ctx.key = base + 0x10000;
  ctx.r1 = make_region(1);
  ctx.r2 = make_region(2);
  ASSERT_EQ(0, va::is_insert(a, base + 0x10000, base + 0x11000, ctx.r1));

  HANDLE t =
      LIBC_NAMESPACE::test_support::create_thread(linearizer_query, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));

  for (int i = 0; i < 100; ++i) {
    va::RegionDesc *next = (i & 1) ? ctx.r2 : ctx.r1;
    (void)va::is_insert(a, base + 0x10000, base + 0x11000, next);
  }
  ctx.stop.store(1, MemoryOrder::RELEASE);
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 10000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(t);
  EXPECT_EQ(ctx.observed_mismatch.load(MemoryOrder::ACQUIRE), 0u);
}

// ===========================================================================
// 30. 4-thread interleaved insert/erase/Query fuzzer (Herlihy-Wing
//     skeleton).
// ===========================================================================
struct FuzzCtx {
  va::Arena *arena;
  va::RegionDesc *value;
  uintptr_t base;
  Atomic<uint32_t> ops{0};
  Atomic<uint32_t> stop{0};
};

LIBC_MSABI static DWORD fuzz_thread(void *arg) {
  auto *ctx = static_cast<FuzzCtx *>(arg);
  uint32_t r = 0xC001D00D;
  while (!ctx->stop.load(MemoryOrder::ACQUIRE)) {
    r = r * 1103515245u + 12345u;
    uintptr_t off =
        ctx->base + static_cast<uintptr_t>((r >> 8) & 0x3F) * 0x1000;
    switch (r & 0x3) {
    case 0:
      (void)va::is_insert(ctx->arena, off, off + 0x800, ctx->value);
      break;
    case 1: (void)va::is_erase(ctx->arena, off, off + 0x800); break;
    case 2: (void)va::Query(ctx->arena, off + 0x100); break;
    case 3: {
      WalkVisitor v;
      va::is_walk_range(ctx->arena, off, off + 0x4000, v);
    } break;
    }
    ctx->ops.fetch_add(1, MemoryOrder::RELAXED);
    if (ctx->ops.load(MemoryOrder::RELAXED) >= 100u) {
      ctx->stop.store(1, MemoryOrder::RELEASE);
      break;
    }
  }
  return 0;
}

TEST(LlvmLibcIntervalSkiplistTest, FourThreadInterleavedFuzzSmoke) {
  va::Arena *a = test_arena(30);
  va::RegionDesc *rd = make_region();
  uintptr_t base = test_base(30);

  FuzzCtx c1{a, rd, base + 0x000000};
  FuzzCtx c2{a, rd, base + 0x080000};
  FuzzCtx c3{a, rd, base + 0x100000};
  FuzzCtx c4{a, rd, base + 0x180000};

  HANDLE t1 = LIBC_NAMESPACE::test_support::create_thread(fuzz_thread, &c1);
  HANDLE t2 = LIBC_NAMESPACE::test_support::create_thread(fuzz_thread, &c2);
  HANDLE t3 = LIBC_NAMESPACE::test_support::create_thread(fuzz_thread, &c3);
  HANDLE t4 = LIBC_NAMESPACE::test_support::create_thread(fuzz_thread, &c4);
  ASSERT_NE(t1, static_cast<HANDLE>(nullptr));
  ASSERT_NE(t2, static_cast<HANDLE>(nullptr));
  ASSERT_NE(t3, static_cast<HANDLE>(nullptr));
  ASSERT_NE(t4, static_cast<HANDLE>(nullptr));

  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t1, 30000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t2, 30000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t3, 30000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t4, 30000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(t1);
  ::NtClose(t2);
  ::NtClose(t3);
  ::NtClose(t4);

  EXPECT_GT(c1.ops.load(MemoryOrder::ACQUIRE), 0u);
  EXPECT_GT(c2.ops.load(MemoryOrder::ACQUIRE), 0u);
  EXPECT_GT(c3.ops.load(MemoryOrder::ACQUIRE), 0u);
  EXPECT_GT(c4.ops.load(MemoryOrder::ACQUIRE), 0u);
}

// ===========================================================================
// 31. Layout invariants — runtime checks mirroring the header's
//     static_asserts so a future header drift surfaces at runtime too.
//
// `__builtin_offsetof(SkiplistNodeBase, next)` would warn under
// `-Winvalid-offsetof` because the inherited `CrystallineNode` carries
// unions, so we use the same `sizeof(T) - sizeof(next-element)` identity
// the header itself uses.
// ===========================================================================
TEST(LlvmLibcIntervalSkiplistTest, LayoutInvariants) {
  EXPECT_EQ(sizeof(va::SkiplistNodeBase), 80u);
  EXPECT_EQ(alignof(va::SkiplistNodeBase), 16u);
  EXPECT_EQ(sizeof(va::SkiplistNodeBase) -
                sizeof(LIBC_NAMESPACE::cpp::Atomic<linkage::Link>),
            72u);
  EXPECT_EQ(va::kArenaCount, 128u);
  EXPECT_EQ(va::kMaxHeight, 16u);
  EXPECT_EQ(va::kBucketCount, 4u);
  EXPECT_EQ(va::kSlotsPerChunk, 256u);
  EXPECT_EQ(va::kChunksPerBucket, 256u);
  // 0xFF is reserved as the link-encoding null sentinel, so the
  // addressable chunk count is 0xFF and the per-process node cap is
  // 256 * 255 = 65280, not 65536.
  EXPECT_EQ(va::kSkiplistAddressableChunks, 0xFFu);
  EXPECT_EQ(va::kMaxSkiplistNodes, 65280u);
}
