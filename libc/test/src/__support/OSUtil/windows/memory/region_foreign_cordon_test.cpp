//===-- Foreign-placeholder cordon protocol tests -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Exercises the FOREIGN slot cordon path documented in
// ~/.claude/plans/humble-roaming-lampson.md ("Why foreign placeholders are
// a first-class state, not an error") and implemented across:
//
//   * mapping_table.h        — register_foreign / mark_foreign_stale /
//                              revalidate_foreign + state_aux probe-lock
//   * region_reconcile.{h,cpp} — cordon_foreigners_in_range
//   * region_desc.h          — RegionShape::FOREIGN_SENTINEL
//
// Foreign reservations are placeholders / views the libc did not allocate
// (loader image headers, CRT heap segments, NT thread-pool ranges, raw
// VirtualAlloc2 from user code). The libc must detect them and stamp
// FOREIGN slots at 64 KB granularity so:
//
//   1. mmap hint scans skip the cordoned range,
//   2. MAP_FIXED_NOREPLACE refuses to overlap (EEXIST), and
//   3. MAP_FIXED prepare_for_fixed sees a non-FREE slot and routes the
//      destructive teardown (or aborts safely) instead of treating the
//      foreign placeholder as our own.
//
// Each test reserves a real foreign placeholder via NtAllocateVirtualMemoryEx
// (bypassing the libc), drives the cordon protocol, then verifies the
// mapping-table state via snapshot() and the public mmap surface.
//
//===----------------------------------------------------------------------===//

#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/mmap_lock.h"
#include "src/__support/OSUtil/windows/memory/region_desc.h"
#include "src/__support/OSUtil/windows/memory/region_pool.h"
#include "src/__support/OSUtil/windows/memory/region_reconcile.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

namespace {

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LIBC_NAMESPACE::windows::g_mapping_table;
using LIBC_NAMESPACE::windows::get_alloc_granularity;
using LIBC_NAMESPACE::windows::MmapLockWriterGuard;
using LIBC_NAMESPACE::windows::SlotSnapshot;
using LIBC_NAMESPACE::windows::memory::cordon_foreigners_in_range;
using LIBC_NAMESPACE::windows::memory::RegionPool;
using LIBC_NAMESPACE::windows::memory::RegionShape;

using LlvmLibcRegionForeignCordon = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// ---------------------------------------------------------------------------
// Helpers — reserve / free a real NT placeholder bypassing the libc engine.
// These mimic the behaviour of the loader, CRT heap, or user VirtualAlloc2:
// a MEM_RESERVE | MEM_RESERVE_PLACEHOLDER PE region the libc has no record
// of and must therefore treat as foreign.
// ---------------------------------------------------------------------------

struct ForeignReservation {
  void *base{nullptr};
  SIZE_T size{0};

  bool reserve(SIZE_T requested, void *desired_base = nullptr) {
    PVOID b = desired_base;
    SIZE_T actual = requested;
    NTSTATUS st = ::NtAllocateVirtualMemoryEx(
        NtCurrentProcess(), &b, &actual,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!NT_SUCCESS(st))
      return false;
    base = b;
    size = actual;
    return true;
  }

  void release() {
    if (base == nullptr)
      return;
    PVOID b = base;
    SIZE_T region_size = 0;
    (void)::NtFreeVirtualMemory(NtCurrentProcess(), &b, &region_size,
                                MEM_RELEASE);
    base = nullptr;
    size = 0;
  }
};

// Snapshot under MmapLock shared so the resolved RegionDesc pointer is
// guaranteed live for the read window (matches mmap_syscall_count_test.cpp).
LIBC_INLINE bool snapshot_held(void *addr, SlotSnapshot *out) {
  LIBC_NAMESPACE::windows::g_mmap_lock.acquire_shared();
  bool ok = g_mapping_table.snapshot(addr, out);
  LIBC_NAMESPACE::windows::g_mmap_lock.release_shared();
  return ok;
}

LIBC_INLINE bool ranges_overlap(void *a, SIZE_T as, void *b, SIZE_T bs) {
  auto a_lo = reinterpret_cast<uintptr_t>(a);
  auto a_hi = a_lo + as;
  auto b_lo = reinterpret_cast<uintptr_t>(b);
  auto b_hi = b_lo + bs;
  return a_lo < b_hi && b_lo < a_hi;
}

// ---------------------------------------------------------------------------
// 1. ForeignReservation_StampedByCordon
//    A vanilla NtAllocateVirtualMemoryEx placeholder is not in our table.
//    cordon_foreigners_in_range walks NT VAD over the range and stamps a
//    FOREIGN slot; subsequent snapshot() must resolve to a region whose
//    shape is FOREIGN_SENTINEL.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcRegionForeignCordon, ForeignReservation_StampedByCordon) {
  const SIZE_T gran = get_alloc_granularity();

  ForeignReservation res;
  // Let NT pick the address — we want a fresh, isolated placeholder.
  ASSERT_TRUE(res.reserve(gran * 4u));
  ASSERT_NE(res.base, nullptr);

  unsigned stamped;
  {
    // Hold MmapLock writer for the duration of the cordon call: the
    // contract on cordon_foreigners_in_range explicitly requires it
    // (the helper does not take the lock itself).
    MmapLockWriterGuard guard;
    stamped = cordon_foreigners_in_range(res.base, res.size);
  }

  EXPECT_GT(stamped, 0u);

  // Walk our reservation at granularity stride and verify each slot
  // resolves to a FOREIGN cordon.
  for (SIZE_T off = 0; off < res.size; off += gran) {
    void *p = static_cast<char *>(res.base) + off;
    SlotSnapshot snap{};
    ASSERT_TRUE(snapshot_held(p, &snap));
    // A FOREIGN stamp may carry region_id == NONE (the sentinel "no
    // descriptor" form). When a descriptor is attached it must report
    // FOREIGN_SENTINEL shape.
    if (snap.region != nullptr) {
      EXPECT_EQ(snap.region->current_shape(), RegionShape::FOREIGN_SENTINEL);
    } else {
      EXPECT_EQ(snap.region_id, RegionPool::NONE);
    }
  }

  res.release();
}

// ---------------------------------------------------------------------------
// 2. MmapHintAvoidsCordonedForeign
//    With a foreign placeholder cordoned, mmap(hint = inside_foreign_range,
//    no MAP_FIXED) must not crash and must not return an address that
//    overlaps the foreign reservation. POSIX makes the hint advisory, so
//    the libc may pick a different address; the load-bearing invariant is
//    only "no overlap with cordoned VA".
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcRegionForeignCordon, MmapHintAvoidsCordonedForeign) {
  const SIZE_T gran = get_alloc_granularity();

  ForeignReservation res;
  ASSERT_TRUE(res.reserve(gran * 4u));

  {
    MmapLockWriterGuard guard; // cordon helper requires writer.
    (void)cordon_foreigners_in_range(res.base, res.size);
  }

  void *hint = static_cast<char *>(res.base) + gran; // inside foreign range
  void *p = LIBC_NAMESPACE::mmap(hint, gran, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);
  EXPECT_FALSE(ranges_overlap(p, gran, res.base, res.size));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, gran), Succeeds());
  res.release();
}

// ---------------------------------------------------------------------------
// 3. MapFixedNoreplaceFailsAtForeignVa
//    A direct MAP_FIXED_NOREPLACE attempt at the cordoned address must fail
//    with EEXIST. The cordon's whole purpose is to make this path detect
//    the foreign owner without ever issuing a destructive NT op.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcRegionForeignCordon, MapFixedNoreplaceFailsAtForeignVa) {
  const SIZE_T gran = get_alloc_granularity();

  ForeignReservation res;
  ASSERT_TRUE(res.reserve(gran * 2u));

  {
    MmapLockWriterGuard guard; // cordon helper requires writer.
    (void)cordon_foreigners_in_range(res.base, res.size);
  }

  void *p = LIBC_NAMESPACE::mmap(res.base, gran, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE |
                                     MAP_FIXED_NOREPLACE,
                                 -1, 0);
  EXPECT_THAT(p, Fails(EEXIST, MAP_FAILED));

  res.release();
}

// ---------------------------------------------------------------------------
// 4. RevalidateClearsCordonAfterForeignFreed
//    After we free the underlying NT placeholder and mark the slot stale,
//    revalidate_foreign() must observe the new MEM_FREE NT state and clear
//    the cordon. A fresh snapshot at the same VA then shows no FOREIGN
//    slot, and ordinary mmap (without a hint at the address) succeeds.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcRegionForeignCordon, RevalidateClearsCordonAfterForeignFreed) {
  const SIZE_T gran = get_alloc_granularity();

  ForeignReservation res;
  ASSERT_TRUE(res.reserve(gran * 2u));

  {
    MmapLockWriterGuard guard; // cordon helper requires writer.
    // stamped counts NEWLY-stamped slots only; if a previous test in this
    // suite reserved at the same VA NT recycled to us, the slots are
    // already FOREIGN — stamp_foreign_holes marks them stale instead.
    // Either way the postcondition is "every slot in the range is
    // FOREIGN", which we verify below.
    (void)cordon_foreigners_in_range(res.base, res.size);
  }

  // Confirm the cordon is in place before tear-down. After the
  // FOREIGN_SENTINEL design landed, cordoned slots carry region_id ==
  // FOREIGN_SENTINEL_REGION_ID (not NONE).
  {
    SlotSnapshot snap{};
    ASSERT_TRUE(snapshot_held(res.base, &snap));
    ASSERT_NE(snap.region, nullptr);
    EXPECT_EQ(snap.region->current_shape(), RegionShape::FOREIGN_SENTINEL);
  }

  void *foreign_base = res.base;
  res.release(); // NT VA at foreign_base is now MEM_FREE.

  // Mark + revalidate. Both mutate radix-table state, so hold the writer
  // lock for the duration to satisfy the cordon protocol's contract:
  // FOREIGN→FREE is published via the slot's WRITING window, and
  // concurrent register_foreign on the same VA would otherwise race.
  bool cleared;
  {
    MmapLockWriterGuard guard;
    g_mapping_table.mark_foreign_stale(foreign_base);
    cleared = g_mapping_table.revalidate_foreign(foreign_base);
  }
  EXPECT_TRUE(cleared);

  // Snapshot must now report no occupied slot at foreign_base — a freshly
  // FREE slot returns false from snapshot().
  {
    SlotSnapshot snap{};
    EXPECT_FALSE(snapshot_held(foreign_base, &snap));
  }

  // mmap (no MAP_FIXED) must succeed. We do not assert the returned address
  // equals foreign_base — the kernel may have re-allocated that VA before
  // we got here. The only invariant is "subsystem is healthy and serves a
  // new mapping somewhere".
  void *p = LIBC_NAMESPACE::mmap(nullptr, gran, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, gran), Succeeds());
}

// ---------------------------------------------------------------------------
// 5. ConcurrentRevalidationSerializesProbeOnce
//    N=4 threads call revalidate_foreign() on the same stale FOREIGN slot.
//    The state_aux probe lock guarantees by construction that only one
//    MRI_Ex syscall fires; we cannot count syscalls portably from the test
//    process, so the structural proxies are:
//
//      * all calls return the same (true/false) result, AND
//      * total wall time stays bounded (no deadlock spin or repeated
//        full-range probes).
//
//    The single-flight invariant lives in the implementation
//    (FOREIGN_PROBE_LOCK CAS + futex_addr park on &state_aux); see
//    mapping_table.h:1380 (revalidate_foreign).
// ---------------------------------------------------------------------------

struct RevalCtx {
  void *target;
  Atomic<int> true_count{0};
  Atomic<int> false_count{0};
  Atomic<int> started{0};
  Atomic<bool> go{false};
};

[[gnu::ms_abi]] static DWORD revalidate_worker(void *arg) {
  auto *ctx = static_cast<RevalCtx *>(arg);
  ctx->started.fetch_add(1, MemoryOrder::RELAXED);
  // Spin until all threads have started so they hit the probe lock at
  // (approximately) the same time — maximising contention on the CAS.
  while (!ctx->go.load(MemoryOrder::ACQUIRE))
    LIBC_NAMESPACE::spin_wait::relax_processor();
  bool result = g_mapping_table.revalidate_foreign(ctx->target);
  if (result)
    ctx->true_count.fetch_add(1, MemoryOrder::RELAXED);
  else
    ctx->false_count.fetch_add(1, MemoryOrder::RELAXED);
  return 0;
}

TEST_F(LlvmLibcRegionForeignCordon,
       ConcurrentRevalidationSerializesProbeOnce) {
  const SIZE_T gran = get_alloc_granularity();
  constexpr int N = 4;

  ForeignReservation res;
  ASSERT_TRUE(res.reserve(gran * 2u));

  {
    MmapLockWriterGuard guard; // cordon helper requires writer.
    (void)cordon_foreigners_in_range(res.base, res.size);
  }
  void *foreign_base = res.base;
  res.release(); // make NT VA actually MEM_FREE so revalidate can clear

  // Mark stale once; all workers race the same probe.
  {
    MmapLockWriterGuard guard;
    g_mapping_table.mark_foreign_stale(foreign_base);
  }

  RevalCtx ctx;
  ctx.target = foreign_base;

  HANDLE ths[N];
  for (int i = 0; i < N; ++i) {
    ths[i] = LIBC_NAMESPACE::test_support::create_thread(revalidate_worker,
                                                          &ctx);
    ASSERT_NE(ths[i], static_cast<HANDLE>(nullptr));
  }
  // Wait for all workers to reach the spin barrier before releasing.
  while (ctx.started.load(MemoryOrder::ACQUIRE) < N)
    LIBC_NAMESPACE::test_support::sleep_ms(1);
  ctx.go.store(true, MemoryOrder::RELEASE);

  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(ths[i],
                                                                    60000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(ths[i]);
  }

  // Every thread saw the same outcome: the slot is either cleared (true)
  // or still cordoned (false), but not a mix. A mix would mean two
  // independent probes saw different NT states — exactly what the
  // probe-lock single-flight guarantee forbids.
  const int t = ctx.true_count.load(MemoryOrder::RELAXED);
  const int f = ctx.false_count.load(MemoryOrder::RELAXED);
  EXPECT_EQ(t + f, N);
  EXPECT_TRUE(t == N || f == N);
}

// ---------------------------------------------------------------------------
// 6. CordonRangeWithMixedOwnership
//    Reserve two foreign placeholders flanking a libc-owned mmap. Cordon
//    over the union range. Only the foreign slots are stamped; the
//    libc-owned slot's region_id must be preserved (i.e. its descriptor
//    is NOT replaced by a FOREIGN sentinel).
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcRegionForeignCordon, CordonRangeWithMixedOwnership) {
  const SIZE_T gran = get_alloc_granularity();

  ForeignReservation left;
  ForeignReservation right;
  ASSERT_TRUE(left.reserve(gran * 2u));
  ASSERT_TRUE(right.reserve(gran * 2u));

  // libc-owned middle: address picked by the engine.
  void *middle = LIBC_NAMESPACE::mmap(nullptr, gran, PROT_READ | PROT_WRITE,
                                       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(middle, MAP_FAILED);

  // Capture middle's pre-cordon region_id so we can confirm cordon left
  // it untouched.
  uint32_t middle_rid_before;
  uint8_t middle_aid_before;
  {
    SlotSnapshot snap{};
    ASSERT_TRUE(snapshot_held(middle, &snap));
    middle_rid_before = snap.region_id;
    middle_aid_before = snap.alloc_id;
    ASSERT_NE(middle_rid_before, RegionPool::NONE);
  }

  // Compute a single union range that brackets all three regions.
  uintptr_t lo = reinterpret_cast<uintptr_t>(left.base);
  uintptr_t hi_left = lo + left.size;
  uintptr_t mid_lo = reinterpret_cast<uintptr_t>(middle);
  uintptr_t mid_hi = mid_lo + gran;
  uintptr_t r_lo = reinterpret_cast<uintptr_t>(right.base);
  uintptr_t r_hi = r_lo + right.size;
  uintptr_t union_lo = lo;
  if (mid_lo < union_lo) union_lo = mid_lo;
  if (r_lo < union_lo) union_lo = r_lo;
  uintptr_t union_hi = hi_left;
  if (mid_hi > union_hi) union_hi = mid_hi;
  if (r_hi > union_hi) union_hi = r_hi;

  {
    MmapLockWriterGuard guard; // cordon helper requires writer.
    (void)cordon_foreigners_in_range(reinterpret_cast<void *>(union_lo),
                                     union_hi - union_lo);
  }

  // Foreign slots: at least one slot in each foreign range carries a
  // FOREIGN cordon. Post-FOREIGN_SENTINEL design these slots resolve to
  // a real RegionDesc with shape FOREIGN_SENTINEL (region_id ==
  // FOREIGN_SENTINEL_REGION_ID), not the legacy region_id == NONE form.
  auto expect_cordoned = [&](void *base, SIZE_T size) {
    bool any_cordoned = false;
    for (SIZE_T off = 0; off < size; off += gran) {
      void *p = static_cast<char *>(base) + off;
      SlotSnapshot snap{};
      if (g_mapping_table.snapshot(p, &snap) && snap.region != nullptr &&
          snap.region->current_shape() == RegionShape::FOREIGN_SENTINEL)
        any_cordoned = true;
    }
    EXPECT_TRUE(any_cordoned);
  };
  expect_cordoned(left.base, left.size);
  expect_cordoned(right.base, right.size);

  // Owned middle: region_id and alloc_id must be unchanged. cordon must
  // never overwrite an owned slot — the contract is "stamp FREE slots
  // only, leave LIVE/PLACEHOLDER alone".
  {
    SlotSnapshot snap{};
    ASSERT_TRUE(snapshot_held(middle, &snap));
    EXPECT_EQ(snap.region_id, middle_rid_before);
    EXPECT_EQ(snap.alloc_id, middle_aid_before);
    ASSERT_NE(snap.region, nullptr);
    EXPECT_NE(snap.region->current_shape(), RegionShape::FOREIGN_SENTINEL);
  }

  EXPECT_THAT(LIBC_NAMESPACE::munmap(middle, gran), Succeeds());
  left.release();
  right.release();
}

} // namespace
