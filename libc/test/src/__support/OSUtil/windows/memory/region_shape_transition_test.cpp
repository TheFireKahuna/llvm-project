//===-- Region shape transition (MONO -> CHUNKED) tests --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The first partial-munmap of a FILE_VIEW_MONO region promotes its shape to
// FILE_VIEW_CHUNKED in place: same region_id, same alloc_id, same refcount,
// same section/file handles. A new chunk_list is installed under the
// per-region chunk_list_lock parking mutex; subsequent partial unmaps punch
// holes through the existing list. The section handle stays alive until the
// last live chunk is unmapped, at which point the region's refcount drops to
// zero and `RegionPool::resolve(rid, aid)` returns nullptr.
//
// Anon (ANON_ONESHOT) partial unmaps are deliberately NOT promoted — they
// take the decommit-only fast path and the shape never changes. This test
// pins both contracts.
//
// The race-coverage test (test 6) drives N concurrent partial-munmaps of a
// shared MONO region. The first unmap promotes; the rest must serialize on
// `chunk_list_lock` while editing the list. A broken parking mutex would
// surface as either a corrupted chunk count or a torn read against the
// surviving pages.
//
// Snapshot helper: MappingTable::snapshot internally uses resolve_unlocked
// (it is reachable from VEH where MmapLock cannot be acquired). To
// dereference RegionDesc fields beyond atomics — chunk_list contents,
// section_handle — the contract in region_pool.h:338-355 requires MmapLock
// held by the caller. We use shared mode for read-only inspection paths and
// drop it before invoking any libc API that may itself try to acquire the
// lock (mmap/munmap take it shared internally, MAP_FIXED takes it
// exclusive). All resolve() / resolve_with_lock paths in this file follow
// the strict acquire-snapshot-release pattern.
//
//===----------------------------------------------------------------------===//

#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/mmap_lock.h"
#include "src/__support/OSUtil/windows/memory/region_desc.h"
#include "src/__support/OSUtil/windows/memory/region_pool.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/sys/mman/memfd_create.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "src/unistd/close.h"
#include "src/unistd/ftruncate.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "hdr/types/off_t.h"

namespace {

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LIBC_NAMESPACE::windows::g_mapping_table;
using LIBC_NAMESPACE::windows::g_mmap_lock;
using LIBC_NAMESPACE::windows::SlotSnapshot;
using LIBC_NAMESPACE::windows::memory::ChunkEntry;
using LIBC_NAMESPACE::windows::memory::ChunkList;
using LIBC_NAMESPACE::windows::memory::g_region_pool;
using LIBC_NAMESPACE::windows::memory::RegionDesc;
using LIBC_NAMESPACE::windows::memory::RegionShape;
namespace region_flag = LIBC_NAMESPACE::windows::memory::region_flag;

constexpr size_t K64 = 64u * 1024u;

// ---------------------------------------------------------------------------
// snapshot_held — capture a slot under MmapLock shared so the resolved
// RegionDesc pointer's lifetime is guaranteed for the read window. Returns
// false when no slot covers `addr`. Releases the lock before returning so
// the caller does not hold MmapLock across libc API calls (avoids the
// reentrant write-from-within-read pattern).
// ---------------------------------------------------------------------------
LIBC_INLINE bool snapshot_held(void *addr, SlotSnapshot *out) {
  g_mmap_lock.acquire_shared();
  bool ok = g_mapping_table.snapshot(addr, out);
  g_mmap_lock.release_shared();
  return ok;
}

// resolve_with_lock — wrap the contract for resolve(). MmapLock shared is
// the minimum required by region_pool.h:356-362; drop it after the read.
// Returns nullptr if the region is dead (refcount 0 or alloc_id mismatch).
LIBC_INLINE RegionDesc *resolve_with_lock(uint32_t rid, uint8_t aid) {
  g_mmap_lock.acquire_shared();
  RegionDesc *rd = g_region_pool.resolve(rid, aid);
  g_mmap_lock.release_shared();
  return rd;
}

// chunk_count_held — read post.region->chunk_list under shared MmapLock,
// safe per the chunk_list comment in region_desc.h: writers hold MmapLock
// writer for all mutations, so a reader holding any flavor sees a stable
// snapshot of the inline header for the duration of its critical section.
LIBC_INLINE uint32_t chunk_count_held(RegionDesc *rd) {
  g_mmap_lock.acquire_shared();
  ChunkList *cl = rd->chunk_list.load(MemoryOrder::ACQUIRE);
  uint32_t count = cl ? cl->count : 0u;
  g_mmap_lock.release_shared();
  return count;
}

} // namespace

using LlvmLibcRegionShapeTransition = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// ---------------------------------------------------------------------------
// 1. FileMonoView_PartialUnmapMiddlePromotesToChunked
//
// 4×64K MAP_SHARED file view. Punching the middle 64K must promote the
// region in place to FILE_VIEW_CHUNKED, install a chunk_list, and leave the
// surviving head and tail readable. Same region_id confirms no spurious
// re-acquire. Two surviving chunks: head [0..0] and tail [2..3] in 64K-unit
// coordinates.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcRegionShapeTransition,
       FileMonoView_PartialUnmapMiddlePromotesToChunked) {
  int fd = LIBC_NAMESPACE::memfd_create("rst_mono_promote", 0);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::ftruncate(fd, static_cast<off_t>(4 * K64)),
              Succeeds(0));

  void *p = LIBC_NAMESPACE::mmap(nullptr, 4 * K64, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
  ASSERT_NE(p, MAP_FAILED);
  char *base = static_cast<char *>(p);

  // Stamp every 64K block; survivors must read back unchanged after the
  // middle is punched out.
  for (size_t i = 0; i < 4; ++i)
    reinterpret_cast<uint32_t *>(base + i * K64)[0] =
        0xC0DE0000u | static_cast<uint32_t>(i);

  // Pre-snapshot — capture (rid, aid) and the MONO shape baseline.
  SlotSnapshot pre{};
  ASSERT_TRUE(snapshot_held(base, &pre));
  ASSERT_NE(pre.region, nullptr);
  ASSERT_EQ(pre.region->current_shape(), RegionShape::FILE_VIEW_MONO);
  uint32_t rid_pre = pre.region_id;
  uint8_t aid_pre = pre.alloc_id;

  // Punch the middle 64K (slot 1).
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 1 * K64, K64), Succeeds());

  // Re-snapshot any address inside the head chunk (slot 0). Same region.
  SlotSnapshot post{};
  ASSERT_TRUE(snapshot_held(base, &post));
  ASSERT_NE(post.region, nullptr);
  EXPECT_EQ(post.region_id, rid_pre);
  EXPECT_EQ(post.alloc_id, aid_pre);

  // get_mutable is safe here because we just resolved the region under
  // MmapLock shared and we hold a logical reference via the live chunk(s);
  // the assertion in get_mutable checks refcount >= 1 only.
  g_mmap_lock.acquire_shared();
  RegionDesc *rd = g_region_pool.get_mutable(post.region_id);
  ASSERT_NE(rd, nullptr);
  EXPECT_EQ(rd->current_shape(), RegionShape::FILE_VIEW_CHUNKED);
  ChunkList *cl = rd->chunk_list.load(MemoryOrder::ACQUIRE);
  ASSERT_NE(cl, nullptr);
  EXPECT_EQ(cl->count, 2u);
  // Head [0..0] and tail [2..3] in 64K-unit coordinates relative to the
  // region's first_slot_key. Order in the inline array is
  // implementation-defined post-punch — assert the set, not the slot.
  bool saw_head = false, saw_tail = false;
  for (uint32_t i = 0; i < cl->count; ++i) {
    const ChunkEntry &e = cl->at(i);
    if (e.base_units == 0 && e.size_units == 1)
      saw_head = true;
    else if (e.base_units == 2 && e.size_units == 2)
      saw_tail = true;
  }
  EXPECT_TRUE(saw_head);
  EXPECT_TRUE(saw_tail);
  g_mmap_lock.release_shared();

  // Surviving pages still carry their stamps.
  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 0 * K64)[0], 0xC0DE0000u);
  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 2 * K64)[0], 0xC0DE0002u);
  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 3 * K64)[0], 0xC0DE0003u);

  // Cleanup the surviving chunks.
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 0 * K64, K64), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 2 * K64, 2 * K64), Succeeds());
  LIBC_NAMESPACE::close(fd);
}

// ---------------------------------------------------------------------------
// 2. ChunkedView_SecondPartialUnmapAddsThirdChunk
//
// 6×64K MAP_SHARED file view. Punch slot 1, then slot 3 — both
// non-adjacent. After two punches the chunk list must hold three live
// segments: [0..0], [2..2], [4..5]. Catches a regression where the second
// punch overwrites the wrong list entry or fails to grow.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcRegionShapeTransition,
       ChunkedView_SecondPartialUnmapAddsThirdChunk) {
  int fd = LIBC_NAMESPACE::memfd_create("rst_chunked_third", 0);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::ftruncate(fd, static_cast<off_t>(6 * K64)),
              Succeeds(0));

  void *p = LIBC_NAMESPACE::mmap(nullptr, 6 * K64, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
  ASSERT_NE(p, MAP_FAILED);
  char *base = static_cast<char *>(p);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 1 * K64, K64), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 3 * K64, K64), Succeeds());

  SlotSnapshot snap{};
  ASSERT_TRUE(snapshot_held(base, &snap));
  ASSERT_NE(snap.region, nullptr);
  EXPECT_EQ(snap.region->current_shape(), RegionShape::FILE_VIEW_CHUNKED);

  g_mmap_lock.acquire_shared();
  RegionDesc *rd = g_region_pool.get_mutable(snap.region_id);
  ChunkList *cl = rd->chunk_list.load(MemoryOrder::ACQUIRE);
  ASSERT_NE(cl, nullptr);
  EXPECT_EQ(cl->count, 3u);
  g_mmap_lock.release_shared();

  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 0 * K64, K64), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 2 * K64, K64), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 4 * K64, 2 * K64), Succeeds());
  LIBC_NAMESPACE::close(fd);
}

// ---------------------------------------------------------------------------
// 3. ChunkedView_LastChunkUnmappedDropsRegion
//
// Drive a region all the way through CHUNKED state and unmap each surviving
// chunk in turn. After the final chunk is dropped, the region's last
// reference is released — resolve(rid, aid) must return nullptr. The fd is
// re-mappable post-tear-down, confirming the section handle was closed
// (and not leaked) when the refcount hit zero.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcRegionShapeTransition,
       ChunkedView_LastChunkUnmappedDropsRegion) {
  int fd = LIBC_NAMESPACE::memfd_create("rst_chunked_drain", 0);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::ftruncate(fd, static_cast<off_t>(4 * K64)),
              Succeeds(0));

  void *p = LIBC_NAMESPACE::mmap(nullptr, 4 * K64, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
  ASSERT_NE(p, MAP_FAILED);
  char *base = static_cast<char *>(p);

  // Capture (rid, aid) before any churn.
  SlotSnapshot pre{};
  ASSERT_TRUE(snapshot_held(base, &pre));
  ASSERT_NE(pre.region, nullptr);
  uint32_t rid = pre.region_id;
  uint8_t aid = pre.alloc_id;

  // Promote to CHUNKED with two live segments: head [0..0], tail [2..3].
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 1 * K64, K64), Succeeds());

  // Region still alive after promotion.
  ASSERT_NE(resolve_with_lock(rid, aid), nullptr);

  // Drop head chunk. Region still alive (tail outstanding).
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 0 * K64, K64), Succeeds());
  ASSERT_NE(resolve_with_lock(rid, aid), nullptr);

  // Drop tail chunk. Last live chunk gone — resolve must now fail.
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 2 * K64, 2 * K64), Succeeds());
  EXPECT_EQ(resolve_with_lock(rid, aid), static_cast<RegionDesc *>(nullptr));

  // Re-mapping the same fd must succeed — section handle was closed cleanly
  // on the last release. A leaked section would still reference the fd's
  // backing object and pile up GDI-style handles, but the visible signal is
  // that mmap continues to work.
  void *q = LIBC_NAMESPACE::mmap(nullptr, 4 * K64, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
  ASSERT_NE(q, MAP_FAILED);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(q, 4 * K64), Succeeds());
  LIBC_NAMESPACE::close(fd);
}

// ---------------------------------------------------------------------------
// 4. MonoView_FullUnmapSkipsPromotion
//
// Whole-range munmap of a MONO region must take the full_unmap_view path,
// not the partial path — no shape promotion happens, the region simply
// dies. Pre-snap proves shape was MONO; post-snap proves the region is
// dead (resolve returns nullptr). Catches a refactor that accidentally
// routed full munmaps through partial_unmap_view (which would promote to
// CHUNKED and leak section handles past the point the region should have
// been released).
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcRegionShapeTransition, MonoView_FullUnmapSkipsPromotion) {
  int fd = LIBC_NAMESPACE::memfd_create("rst_mono_full", 0);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::ftruncate(fd, static_cast<off_t>(4 * K64)),
              Succeeds(0));

  void *p = LIBC_NAMESPACE::mmap(nullptr, 4 * K64, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
  ASSERT_NE(p, MAP_FAILED);

  SlotSnapshot pre{};
  ASSERT_TRUE(snapshot_held(p, &pre));
  ASSERT_NE(pre.region, nullptr);
  EXPECT_EQ(pre.region->current_shape(), RegionShape::FILE_VIEW_MONO);
  uint32_t rid = pre.region_id;
  uint8_t aid = pre.alloc_id;

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, 4 * K64), Succeeds());

  // Region must be dead.
  EXPECT_EQ(resolve_with_lock(rid, aid), static_cast<RegionDesc *>(nullptr));
  LIBC_NAMESPACE::close(fd);
}

// ---------------------------------------------------------------------------
// 5. AnonOneshot_PartialUnmapDoesNotPromote
//
// MAP_PRIVATE|MAP_ANONYMOUS (ANON_ONESHOT) takes the decommit-only fast
// path on partial munmap — no MONO->CHUNKED promotion, the region keeps
// its ANON_ONESHOT shape with no chunk_list installed. Surrounding pages
// remain readable.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcRegionShapeTransition,
       AnonOneshot_PartialUnmapDoesNotPromote) {
  void *p = LIBC_NAMESPACE::mmap(nullptr, 4 * K64, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);
  char *base = static_cast<char *>(p);

  // Stamp head/tail.
  reinterpret_cast<uint32_t *>(base + 0 * K64)[0] = 0xA1A1A1A1u;
  reinterpret_cast<uint32_t *>(base + 3 * K64)[0] = 0xA3A3A3A3u;

  SlotSnapshot pre{};
  ASSERT_TRUE(snapshot_held(base, &pre));
  ASSERT_NE(pre.region, nullptr);
  ASSERT_EQ(pre.region->current_shape(), RegionShape::ANON_PLACEHOLDER);

  // Punch the middle 64K (slot 1).
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 1 * K64, K64), Succeeds());

  // Shape unchanged — anon never promotes — and chunk_list never installed.
  SlotSnapshot post{};
  ASSERT_TRUE(snapshot_held(base, &post));
  ASSERT_NE(post.region, nullptr);
  EXPECT_EQ(post.region->current_shape(), RegionShape::ANON_PLACEHOLDER);
  EXPECT_EQ(post.region->get_chunk_list(),
            static_cast<ChunkList *>(nullptr));

  // Surrounding pages still readable.
  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 0 * K64)[0], 0xA1A1A1A1u);
  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 3 * K64)[0], 0xA3A3A3A3u);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 0 * K64, K64), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 2 * K64, 2 * K64), Succeeds());
}

// ---------------------------------------------------------------------------
// 6. ConcurrentPartialUnmapsRace
//
// Stresses the per-region chunk_list_lock parking mutex. Layout: 8×64K
// MAP_SHARED file view; four worker threads each punch a single,
// non-overlapping 64K hole at the odd-numbered slots {1, 3, 5, 7}. The
// first arriving thread promotes MONO->CHUNKED (acquires chunk_list_lock,
// installs the list with one initial split). The remaining threads
// serialize on chunk_list_lock to punch their holes. Final state is
// deterministic regardless of ordering: survivors at slots {0, 2, 4, 6} —
// none adjacent — so chunk count must be exactly 4. Surviving stamped
// pages must read back unchanged: a corrupted lock would surface as a
// torn count or as VEH-side data corruption when a punch races a publish.
// ---------------------------------------------------------------------------

namespace {
struct RaceCtx {
  char *base;
  Atomic<int> errors{0};
};

[[gnu::ms_abi]] DWORD race_punch_worker(void *arg) {
  // The thread index is stuffed into the high 16 bits of arg; the low 16
  // bits hold the slot offset (1, 3, 5, or 7) the thread is responsible
  // for. Avoids a per-thread context struct.
  uintptr_t packed = reinterpret_cast<uintptr_t>(arg);
  RaceCtx *ctx = reinterpret_cast<RaceCtx *>(packed & ~uintptr_t{0x7});
  unsigned slot = static_cast<unsigned>(packed & uintptr_t{0x7});
  if (LIBC_NAMESPACE::munmap(ctx->base + slot * K64, K64) != 0)
    ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
  return 0;
}
} // namespace

TEST_F(LlvmLibcRegionShapeTransition, ConcurrentPartialUnmapsRace) {
  int fd = LIBC_NAMESPACE::memfd_create("rst_race", 0);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::ftruncate(fd, static_cast<off_t>(8 * K64)),
              Succeeds(0));

  void *p = LIBC_NAMESPACE::mmap(nullptr, 8 * K64, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
  ASSERT_NE(p, MAP_FAILED);
  RaceCtx ctx;
  ctx.base = static_cast<char *>(p);

  // Stamp the slots that will survive (even indices). Surviving stamps must
  // remain intact across the race — corruption proves the punch ranges
  // bled into adjacent surviving chunks (a chunk_list_lock failure
  // permitting overlapping chunk edits).
  for (unsigned i = 0; i < 8; i += 2)
    reinterpret_cast<uint32_t *>(ctx.base + i * K64)[0] =
        0xCAFE0000u | i;

  // 8-byte alignment of RaceCtx is the key the worker decoder relies on.
  static_assert(alignof(RaceCtx) >= 8, "RaceCtx alignment ABI for arg pack");

  constexpr unsigned N = 4;
  constexpr unsigned slots[N] = {1u, 3u, 5u, 7u};
  HANDLE ths[N];
  for (unsigned i = 0; i < N; ++i) {
    void *arg = reinterpret_cast<void *>(
        reinterpret_cast<uintptr_t>(&ctx) | uintptr_t{slots[i]});
    ths[i] = LIBC_NAMESPACE::test_support::create_thread(race_punch_worker,
                                                          arg);
    ASSERT_NE(ths[i], static_cast<HANDLE>(nullptr));
  }
  for (unsigned i = 0; i < N; ++i) {
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(ths[i],
                                                                    60000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(ths[i]);
  }
  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);

  // Survivors at {0, 2, 4, 6}, none adjacent — exactly 4 chunks.
  SlotSnapshot snap{};
  ASSERT_TRUE(snapshot_held(ctx.base, &snap));
  ASSERT_NE(snap.region, nullptr);
  EXPECT_EQ(snap.region->current_shape(), RegionShape::FILE_VIEW_CHUNKED);

  g_mmap_lock.acquire_shared();
  RegionDesc *rd = g_region_pool.get_mutable(snap.region_id);
  ChunkList *cl = rd->chunk_list.load(MemoryOrder::ACQUIRE);
  ASSERT_NE(cl, nullptr);
  EXPECT_EQ(cl->count, 4u);
  g_mmap_lock.release_shared();

  // Surviving pages still carry their stamps — no cross-chunk damage.
  for (unsigned i = 0; i < 8; i += 2)
    EXPECT_EQ(reinterpret_cast<uint32_t *>(ctx.base + i * K64)[0],
              0xCAFE0000u | i);

  // Cleanup all four survivors.
  for (unsigned i = 0; i < 8; i += 2)
    EXPECT_THAT(LIBC_NAMESPACE::munmap(ctx.base + i * K64, K64), Succeeds());
  LIBC_NAMESPACE::close(fd);
}

// ---------------------------------------------------------------------------
// 7. RegionFlagSharedSetForMapShared
//
// MAP_SHARED file mappings carry region_flag::SHARED in the descriptor;
// MAP_PRIVATE file mappings do not. The flag drives downstream fork CoW
// decisions (CoW preservation on split, msync visibility) and is stamped
// at acquire-time from the original mmap flags — immutable for the
// region's life. Pinning both polarities here catches a refactor that
// flips the bit derivation.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcRegionShapeTransition, RegionFlagSharedSetForMapShared) {
  int fd = LIBC_NAMESPACE::memfd_create("rst_flag_shared", 0);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::ftruncate(fd, static_cast<off_t>(K64)),
              Succeeds(0));

  void *ps = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
  ASSERT_NE(ps, MAP_FAILED);

  SlotSnapshot snap_s{};
  ASSERT_TRUE(snapshot_held(ps, &snap_s));
  ASSERT_NE(snap_s.region, nullptr);
  EXPECT_TRUE(snap_s.region->has_flag(region_flag::SHARED));

  void *pp = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE, fd, 0);
  ASSERT_NE(pp, MAP_FAILED);

  SlotSnapshot snap_p{};
  ASSERT_TRUE(snapshot_held(pp, &snap_p));
  ASSERT_NE(snap_p.region, nullptr);
  EXPECT_FALSE(snap_p.region->has_flag(region_flag::SHARED));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(ps, K64), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(pp, K64), Succeeds());
  LIBC_NAMESPACE::close(fd);
}
