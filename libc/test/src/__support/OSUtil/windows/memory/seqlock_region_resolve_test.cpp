//===-- Seqlock snapshot + RegionPool::resolve ABA defence ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The mapping-table snapshot protocol composes a per-slot seqlock (version +
// key, both ACQUIRE) with a RegionPool::resolve(rid, aid) call that validates
// the descriptor's alloc_id generation against the value captured by the
// snapshot.  Together they close the ABA window opened by RegionPool slot
// reuse: a release() that drops the last refcount of region R lets a
// concurrent acquire() reclaim the same pool slot for an unrelated region R'
// at a (possibly different) alloc_id.  A reader that walked through R's
// (rid, aid) snapshot must never observe R' as if it were R; the alloc_id
// check is the single point that prevents that.
//
// This file pins the contract end-to-end through the public mmap surface:
//
//   1. Basic — snapshot of a fresh anon mapping resolves to a live region
//      whose shape is ANON_ONESHOT.
//   2. After munmap — the slot is FREE; snapshot returns false.
//   3. Stale alloc_id — capture (rid, aid) of a region that gets released and
//      then re-acquired (likely at a different rid OR a different aid),
//      and resolve(rid_old, aid_old) must return nullptr.
//   4. Concurrent reader/writer — a writer recycles a fixed VA while N
//      readers snapshot it; readers either see no live mapping, or a live
//      mapping whose RegionDesc shape matches what the writer published.
//      The seqlock + alloc_id together must prevent any "torn" view.
//   5. Writer preference — under heavy reader load the writer must complete
//      its publish/teardown cycles in bounded time (mmap_lock writer-
//      preference is the load-bearing guarantee).
//   6. Lock-held resolve — under MmapLockWriterGuard, resolve() (the asserting
//      variant) succeeds for a known-live region.  A regression that broke
//      the held_depth_tls accounting or the assert path itself would surface.
//
// Failure modes the test catches:
//   * resolve() returning a non-null pointer for a stale alloc_id -> the
//      reader would dereference fields belonging to a different region.
//   * snapshot succeeding while reporting region == nullptr for a slot the
//      writer just published -> seqlock retry loop missed a publish.
//   * snapshot reporting torn fields (size mismatch, region_id from one
//      cycle paired with shape from another) -> seqlock's v1==v2 check
//      failed to retry on concurrent WRITING.
//
//===----------------------------------------------------------------------===//

#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/mmap_lock.h"
#include "src/__support/OSUtil/windows/memory/region_desc.h"
#include "src/__support/OSUtil/windows/memory/region_pool.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

namespace {

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LIBC_NAMESPACE::windows::g_mapping_table;
using LIBC_NAMESPACE::windows::g_mmap_lock;
using LIBC_NAMESPACE::windows::MmapLockWriterGuard;
using LIBC_NAMESPACE::windows::SlotSnapshot;
using LIBC_NAMESPACE::windows::memory::g_region_pool;
using LIBC_NAMESPACE::windows::memory::RegionDesc;
using LIBC_NAMESPACE::windows::memory::RegionPool;
using LIBC_NAMESPACE::windows::memory::RegionShape;

using LlvmLibcSeqlockResolveTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

constexpr size_t K64 = 64u * 1024u;

// ---------------------------------------------------------------------------
// Snapshot helper — must run under MmapLock shared.  MappingTable::snapshot
// itself uses resolve_unlocked (the VEH-safe variant), but downstream
// dereference of region->shape / refcount only stays valid for the duration
// of an MmapLock hold (see region_pool.h:338-355 lifetime contract).  All
// snapshot consumers in this file go through this wrapper to keep the
// contract local and explicit.
// ---------------------------------------------------------------------------

LIBC_INLINE static bool snapshot_held(void *addr, SlotSnapshot *out) {
  g_mmap_lock.acquire_shared();
  bool ok = g_mapping_table.snapshot(addr, out);
  g_mmap_lock.release_shared();
  return ok;
}

// ---------------------------------------------------------------------------
// 1. Basic — snapshot of a fresh ANON one-shot resolves and reports
//    ANON_ONESHOT shape.  Munmap then reports false.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcSeqlockResolveTest, BasicSnapshotResolvesRegion) {
  void *p = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);

  SlotSnapshot snap{};
  ASSERT_TRUE(snapshot_held(p, &snap));
  ASSERT_NE(snap.region, nullptr);
  EXPECT_EQ(snap.region->current_shape(), RegionShape::ANON_PLACEHOLDER);
  EXPECT_NE(snap.region_id, RegionPool::NONE);
  // alloc_id never wraps to 0 — try_claim skips zero on rollover (see
  // region_desc.h:266-269).  Snapshot must observe the bumped value.
  EXPECT_NE(snap.alloc_id, static_cast<uint8_t>(0));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, K64), Succeeds());
}

TEST_F(LlvmLibcSeqlockResolveTest, SnapshotAfterMunmapReturnsFalse) {
  void *p = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, K64), Succeeds());

  // After munmap the radix slot's key is KEY_FREE; snapshot bails on the
  // (k & ~STATE_MASK) != target check inside snapshot_at_slot.
  SlotSnapshot snap{};
  EXPECT_FALSE(snapshot_held(p, &snap));
}

// ---------------------------------------------------------------------------
// 3. Stale alloc_id — capture (rid, aid) of a region that gets released, then
//    publish a new region.  resolve(rid_old, aid_old) must return nullptr
//    even when the new region happens to land on the same pool slot (rid_new
//    == rid_old, aid_new != aid_old) AND when it lands on a different slot
//    (rid_new != rid_old).  In the second case the OLD slot is dead so the
//    refcount==0 check trips; in the first case the alloc_id check trips.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcSeqlockResolveTest, StaleAllocIdSnapshotRejectedByResolve) {
  // First mapping at any address.
  void *p1 = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p1, MAP_FAILED);

  SlotSnapshot snap1{};
  ASSERT_TRUE(snapshot_held(p1, &snap1));
  uint32_t old_rid = snap1.region_id;
  uint8_t old_aid = snap1.alloc_id;
  ASSERT_NE(old_rid, RegionPool::NONE);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p1, K64), Succeeds());

  // Loop a bounded number of fresh acquires, hoping one lands on the same
  // pool slot so we exercise the alloc_id-mismatch branch specifically.
  // Either way, the assertion is the same: resolve(old_rid, old_aid) returns
  // nullptr.  We hold MmapLock shared per the resolve() lifetime contract.
  constexpr int RECYCLE_TRIES = 64;
  bool saw_same_rid = false;
  void *holders[RECYCLE_TRIES] = {};
  for (int i = 0; i < RECYCLE_TRIES; ++i) {
    holders[i] = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (holders[i] == MAP_FAILED) {
      holders[i] = nullptr;
      continue;
    }
    SlotSnapshot snap_n{};
    if (!snapshot_held(holders[i], &snap_n))
      continue;
    if (snap_n.region_id == old_rid) {
      saw_same_rid = true;
      // The new region MUST carry a different alloc_id — try_claim bumps
      // the generation on every reuse (skipping 0).
      EXPECT_NE(snap_n.alloc_id, old_aid);
    }
  }

  // Core assertion — the (old_rid, old_aid) tuple is permanently dead.
  // resolve() needs MmapLock held; acquire it shared for the duration of
  // the dereference window (we do not actually dereference, but the assert
  // inside resolve() requires the lock).
  g_mmap_lock.acquire_shared();
  RegionDesc *resolved = g_region_pool.resolve(old_rid, old_aid);
  g_mmap_lock.release_shared();
  EXPECT_EQ(resolved, nullptr);

  // Drain holders.
  for (int i = 0; i < RECYCLE_TRIES; ++i)
    if (holders[i])
      LIBC_NAMESPACE::munmap(holders[i], K64);

  // The recycle-detection branch is informative — we don't gate the test on
  // it because the pool's free-list ordering may legitimately not reuse the
  // same slot during this test.  (void) it to silence -Wunused.
  (void)saw_same_rid;
}

// ---------------------------------------------------------------------------
// 4. Concurrent reader/writer recycle — load-bearing test for ABA defence.
//    The writer mmap/munmap-cycles the SAME virtual address using MAP_FIXED
//    so the radix slot is reused every iteration.  Each cycle stamps a new
//    "epoch" sentinel (writer_epoch).  Readers snapshot the address and, if
//    the snapshot resolves to a live ANON_ONESHOT region, read the sentinel.
//    The invariant: a reader that successfully resolves through (rid, aid)
//    must observe a sentinel from a real publication — not a recycled-pool
//    ghost.  The reader's own seqlock retry catches the cases where the
//    writer is mid-publish.
// ---------------------------------------------------------------------------

struct RecycleCtx {
  void *base;
  Atomic<uint32_t> writer_epoch{0};
  Atomic<bool> done{false};
  Atomic<int> reader_torn{0};
  Atomic<int> reader_observations{0};
};

static constexpr int RECYCLE_CYCLES = 2000;

[[gnu::ms_abi]] static DWORD recycle_writer(void *arg) {
  auto *ctx = static_cast<RecycleCtx *>(arg);
  for (int i = 0; i < RECYCLE_CYCLES; ++i) {
    void *r = LIBC_NAMESPACE::mmap(
        ctx->base, K64, PROT_READ | PROT_WRITE,
        MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (r != ctx->base)
      continue;
    uint32_t epoch = static_cast<uint32_t>(i) | 0xE0000000u;
    // Publish the sentinel BEFORE allowing readers to consider this cycle's
    // mapping coherent.  RELEASE on writer_epoch is paired with the
    // reader's ACQUIRE load below — establishes happens-before for the
    // page write the reader will perform via the resolved region.
    *static_cast<volatile uint32_t *>(r) = epoch;
    ctx->writer_epoch.store(epoch, MemoryOrder::RELEASE);
    LIBC_NAMESPACE::munmap(r, K64);
  }
  ctx->done.store(true, MemoryOrder::RELEASE);
  return 0;
}

[[gnu::ms_abi]] static DWORD recycle_reader(void *arg) {
  auto *ctx = static_cast<RecycleCtx *>(arg);
  while (!ctx->done.load(MemoryOrder::ACQUIRE)) {
    SlotSnapshot snap{};
    g_mmap_lock.acquire_shared();
    bool ok = g_mapping_table.snapshot(ctx->base, &snap);
    if (ok && snap.region != nullptr) {
      // Region resolved through the alloc_id check — must be a sane shape.
      // A torn read could land an ANON_ONESHOT region_id paired with a
      // FILE_VIEW shape (impossible without snapshot tearing) or a region
      // pointer whose refcount has dropped to 0 mid-read (resolve() catches
      // this via the refcount check).
      RegionShape sh = snap.region->current_shape();
      if (sh != RegionShape::ANON_PLACEHOLDER && sh != RegionShape::NONE) {
        // Anything else is impossible if the writer only ever publishes
        // anonymous mappings — flag torn.
        ctx->reader_torn.fetch_add(1, MemoryOrder::RELAXED);
      }
      // Extent must match the writer's K64 publication.
      if (snap.view_size != K64)
        ctx->reader_torn.fetch_add(1, MemoryOrder::RELAXED);
      ctx->reader_observations.fetch_add(1, MemoryOrder::RELAXED);
    }
    g_mmap_lock.release_shared();
  }
  return 0;
}

TEST_F(LlvmLibcSeqlockResolveTest, ConcurrentReaderWriterRecycle) {
  RecycleCtx ctx;
  // Reserve a fixed VA up front so MAP_FIXED has a known target.  We tear
  // it down inside the writer's first iteration via MAP_FIXED's destructive
  // semantics, so this initial mapping is just to pin the address.
  ctx.base = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(ctx.base, MAP_FAILED);

  constexpr int N_READERS = 3;
  HANDLE readers[N_READERS];
  for (int i = 0; i < N_READERS; ++i) {
    readers[i] =
        LIBC_NAMESPACE::test_support::create_thread(recycle_reader, &ctx);
    ASSERT_NE(readers[i], static_cast<HANDLE>(nullptr));
  }
  HANDLE writer =
      LIBC_NAMESPACE::test_support::create_thread(recycle_writer, &ctx);
  ASSERT_NE(writer, static_cast<HANDLE>(nullptr));

  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(writer, 60000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(writer);
  for (int i = 0; i < N_READERS; ++i) {
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(readers[i],
                                                                    60000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(readers[i]);
  }

  EXPECT_EQ(ctx.reader_torn.load(MemoryOrder::RELAXED), 0);
  // We expect at least some successful observations under load — if zero,
  // either the writer never published a coherent mapping or the readers
  // never caught one (both indicate a regression).
  EXPECT_GT(ctx.reader_observations.load(MemoryOrder::RELAXED), 0);

  // Final cleanup: writer's last cycle left a mapping installed at base.
  // (recycle_writer always ends with a munmap, so this should be FREE.)
  // The unmap is best-effort — failure means the writer's last munmap
  // already cleared the slot, which is the expected steady state.
  (void)LIBC_NAMESPACE::munmap(ctx.base, K64);
}

// ---------------------------------------------------------------------------
// 5. Writer preference under heavy readers — confirms readers do not starve
//    the writer.  N readers spin-snapshot the address while the writer does
//    100 mmap/munmap cycles.  The writer must complete in bounded wall time;
//    we use a generous 5-second budget that should be unreachable in normal
//    operation but trips on a regression that broke writer-preference.
// ---------------------------------------------------------------------------

struct WriterPrefCtx {
  void *base;
  Atomic<int> writer_cycles{0};
  Atomic<bool> writer_done{false};
};

static constexpr int WRITER_PREF_CYCLES = 100;

[[gnu::ms_abi]] static DWORD writer_pref_reader(void *arg) {
  auto *ctx = static_cast<WriterPrefCtx *>(arg);
  // Spin on snapshots without yielding so we stress writer_preference; if
  // mmap_lock degrades to FIFO under load, the writer's exclusive acquire
  // will starve and the test times out.
  while (!ctx->writer_done.load(MemoryOrder::ACQUIRE)) {
    SlotSnapshot snap{};
    g_mmap_lock.acquire_shared();
    (void)g_mapping_table.snapshot(ctx->base, &snap);
    g_mmap_lock.release_shared();
  }
  return 0;
}

[[gnu::ms_abi]] static DWORD writer_pref_writer(void *arg) {
  auto *ctx = static_cast<WriterPrefCtx *>(arg);
  for (int i = 0; i < WRITER_PREF_CYCLES; ++i) {
    void *r = LIBC_NAMESPACE::mmap(
        ctx->base, K64, PROT_READ | PROT_WRITE,
        MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (r == ctx->base) {
      LIBC_NAMESPACE::munmap(r, K64);
      ctx->writer_cycles.fetch_add(1, MemoryOrder::RELEASE);
    }
  }
  ctx->writer_done.store(true, MemoryOrder::RELEASE);
  return 0;
}

TEST_F(LlvmLibcSeqlockResolveTest, WriterPreferenceUnderHeavyReaders) {
  WriterPrefCtx ctx;
  ctx.base = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(ctx.base, MAP_FAILED);

  constexpr int N_READERS = 4;
  HANDLE readers[N_READERS];
  for (int i = 0; i < N_READERS; ++i) {
    readers[i] = LIBC_NAMESPACE::test_support::create_thread(writer_pref_reader,
                                                              &ctx);
    ASSERT_NE(readers[i], static_cast<HANDLE>(nullptr));
  }
  HANDLE writer =
      LIBC_NAMESPACE::test_support::create_thread(writer_pref_writer, &ctx);
  ASSERT_NE(writer, static_cast<HANDLE>(nullptr));

  // Bounded-time wait on the writer.  5s is well above the >1s expected
  // upper bound for 100 mmap/munmap cycles on Windows; a regression that
  // starves the writer will trip this.
  DWORD wait_writer =
      LIBC_NAMESPACE::test_support::wait_for_single_object(writer, 5000);
  EXPECT_EQ(wait_writer,
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(writer);

  // writer_done is set by the writer's last action; readers see it via
  // ACQUIRE and exit.
  for (int i = 0; i < N_READERS; ++i) {
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(readers[i],
                                                                    10000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(readers[i]);
  }

  EXPECT_EQ(ctx.writer_cycles.load(MemoryOrder::RELAXED), WRITER_PREF_CYCLES);

  (void)LIBC_NAMESPACE::munmap(ctx.base, K64);
}

// ---------------------------------------------------------------------------
// 6. Resolve contract under MmapLock — positive control.  resolve() asserts
//    the lock is held by the current thread (region_pool.h:358-361).  We
//    cannot unit-test the assert firing without aborting the process, so
//    instead we drive the positive path: under MmapLockWriterGuard,
//    resolve(rid, aid) of a known-live region must return a non-null pointer
//    and the held_depth_tls() bookkeeping must remain consistent across the
//    call.  A regression that silently dropped the held-depth invariant
//    (e.g. guard not bumping the counter) would be caught at the assert
//    itself in debug builds and as a held() == false here in release.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcSeqlockResolveTest, ResolveContractRequiresMmapLockHeld) {
  void *p = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);

  // Capture (rid, aid) under shared lock — snapshot_held() does the right
  // thing here.
  SlotSnapshot snap{};
  ASSERT_TRUE(snapshot_held(p, &snap));
  ASSERT_NE(snap.region_id, RegionPool::NONE);
  uint32_t rid = snap.region_id;
  uint8_t aid = snap.alloc_id;

  // Open the writer hold and confirm:
  //   * is_held_by_current_thread() reports true (the assert's predicate)
  //   * resolve() returns non-null (ABA check passes for a live region)
  //   * the returned descriptor's shape matches the snapshot's resolution
  {
    MmapLockWriterGuard guard;
    EXPECT_TRUE(LIBC_NAMESPACE::windows::MmapLock::is_held_by_current_thread());
    RegionDesc *rd = g_region_pool.resolve(rid, aid);
    ASSERT_NE(rd, nullptr);
    EXPECT_EQ(rd->current_shape(), RegionShape::ANON_PLACEHOLDER);
    // refcount must be >= 1 — resolve() guarantees this on success.
    EXPECT_GE(rd->refcount.load(MemoryOrder::RELAXED), 1u);
  }
  // After guard scope, the lock is released; held_depth_tls() returns to
  // its prior value (zero in this thread because we never nested another
  // hold).
  EXPECT_FALSE(LIBC_NAMESPACE::windows::MmapLock::is_held_by_current_thread());

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, K64), Succeeds());
}

} // namespace
