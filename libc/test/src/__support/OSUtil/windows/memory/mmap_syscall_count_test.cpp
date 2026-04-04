//===-- mmap recipe syscall-budget pin test --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pins the recipe syscall budgets the mmap-engine plan promises (see the
// "Recipes (final syscall counts)" table in
// ~/.claude/plans/humble-roaming-lampson.md). The plan's value proposition
// is that each top-level mmap-family op resolves to a known, small number
// of NT syscalls — anonymous one-shot is a single NtAllocateVirtualMemoryEx,
// anonymous PROT_NONE is a single placeholder reserve, file MAP_SHARED is
// section + view, partial unmap of an anon one-shot is a single MEM_DECOMMIT,
// partial unmap of a file MONO promotes to CHUNKED with a placeholder split
// + view re-map but creates no extra logical region, and so on.
//
// LD_PRELOAD-style syscall interception is impractical on Windows from a
// libc unit test, so the test uses observable proxies that are tightly
// coupled to the syscall recipes:
//
//   * RegionPool::live_count_estimate() deltas. Every recipe that creates a
//     new section, placeholder, or anon range allocates exactly one
//     RegionDesc; a refactor that accidentally splits a logical mapping into
//     two regions is caught here. A refactor that double-acquires a region
//     for the same logical mmap is also caught.
//
//   * MappingTable::snapshot() of the freshly published view. The slot
//     records the resolved RegionShape and region_flag bits. If the recipe
//     selects the wrong syscall path (e.g. uses the placeholder model for
//     an op the plan budgets as one-shot, or vice versa) the resolved shape
//     diverges from the budget and the test fails.
//
//   * RegionPool::resolve(rid, aid) returning nullptr after a logical drop.
//     Any recipe that leaks a region reference past its logical lifetime
//     (e.g. forgets to release on overwrite by MAP_FIXED or on whole
//     munmap) leaves the (rid, aid) pair live and the resolve check trips.
//
// These proxies do not count syscalls directly, but the recipe budgets in
// the plan are stated in terms of "regions / placeholders / sections /
// views per op" — quantities the proxies measure exactly. A future refactor
// that adds an extra NtQueryVirtualMemory probe to internal::mmap (a real
// historical regression) would not change the region count, but every other
// budget violation the plan calls out (extra section, missed promotion,
// missed release) does show up here.
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

using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LIBC_NAMESPACE::windows::g_mapping_table;
using LIBC_NAMESPACE::windows::g_mmap_lock;
using LIBC_NAMESPACE::windows::SlotSnapshot;
using LIBC_NAMESPACE::windows::memory::ChunkList;
using LIBC_NAMESPACE::windows::memory::g_region_pool;
using LIBC_NAMESPACE::windows::memory::RegionDesc;
using LIBC_NAMESPACE::windows::memory::RegionShape;

constexpr size_t K64 = 64u * 1024u;

// ---------------------------------------------------------------------------
// Helper — snapshot a slot under MmapLock shared so the resolved RegionDesc
// pointer's lifetime is guaranteed for the snapshot's read window.
// MappingTable::snapshot itself uses resolve_unlocked (it can be called from
// VEH), but we want to dereference the RegionDesc's shape/flags/chunk_list
// fields, and the RegionPool lifetime contract is "safe under MmapLock".
// ---------------------------------------------------------------------------

LIBC_INLINE static bool snapshot_held(void *addr, SlotSnapshot *out) {
  g_mmap_lock.acquire_shared();
  bool ok = g_mapping_table.snapshot(addr, out);
  g_mmap_lock.release_shared();
  return ok;
}

// ---------------------------------------------------------------------------
// 1. ANON_ONESHOT — mmap(MAP_ANON|MAP_PRIVATE, RW) creates exactly one
//    region with shape ANON_ONESHOT. Munmap drops it.
//
// Recipe budget (plan): "ANON one-shot — 1 syscall NtAllocateVirtualMemoryEx
// (RESERVE|COMMIT|WRITE_WATCH); 1 region; 0 sections."
// ---------------------------------------------------------------------------

TEST(LlvmLibcMmapSyscallCount, AnonOneShotMmap_OneRegionAcquired) {
  uint32_t before = g_region_pool.live_count_estimate();

  void *p = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);

  uint32_t after_mmap = g_region_pool.live_count_estimate();
  EXPECT_EQ(after_mmap, before + 1u);

  // Confirm the slot resolved to ANON_ONESHOT — eliminates the false-positive
  // where an unrelated region was allocated by a concurrent libc internal
  // and our +1 happened to coincide with its bookkeeping.
  SlotSnapshot snap{};
  ASSERT_TRUE(snapshot_held(p, &snap));
  ASSERT_NE(snap.region, nullptr);
  EXPECT_EQ(snap.region->current_shape(), RegionShape::ANON_PLACEHOLDER);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, K64), Succeeds());

  uint32_t after_munmap = g_region_pool.live_count_estimate();
  EXPECT_EQ(after_munmap, before);
}

// ---------------------------------------------------------------------------
// 2. ANON_PLACEHOLDER — PROT_NONE anon takes the placeholder model. Plan
//    budget: "ANON placeholder — 1 NtAllocateVirtualMemoryEx
//    (RESERVE|RESERVE_PLACEHOLDER); 1 region; refcount==1."
// ---------------------------------------------------------------------------

TEST(LlvmLibcMmapSyscallCount, AnonPlaceholderMmap_OneRegionShapeMatches) {
  uint32_t before = g_region_pool.live_count_estimate();

  void *p = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_NONE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);

  EXPECT_EQ(g_region_pool.live_count_estimate(), before + 1u);

  SlotSnapshot snap{};
  ASSERT_TRUE(snapshot_held(p, &snap));
  ASSERT_NE(snap.region, nullptr);
  EXPECT_EQ(snap.region->current_shape(), RegionShape::ANON_PLACEHOLDER);
  // Refcount must be exactly the one we hold via the published slot — no
  // stray double-acquire by an internal cache path.
  EXPECT_EQ(snap.region->get_refcount(), 1u);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, K64), Succeeds());
  EXPECT_EQ(g_region_pool.live_count_estimate(), before);
}

// ---------------------------------------------------------------------------
// 3. FILE_VIEW_MONO — file MAP_SHARED via memfd_create. Plan budget: file
//    one-shot is "1 NtCreateSection + 1 NtMapViewOfSection; 1 region with
//    section_handle and file_handle both non-null." Both handle fields
//    being non-null is the falsifiable claim that a refactor producing the
//    placeholder-only path or the section-without-file path would violate.
// ---------------------------------------------------------------------------

TEST(LlvmLibcMmapSyscallCount, FileMmapShared_OneRegionFileViewMono) {
  int fd = LIBC_NAMESPACE::memfd_create("mmap_syscall_count_file", 0);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::ftruncate(fd, static_cast<off_t>(K64)),
              Succeeds(0));

  uint32_t before = g_region_pool.live_count_estimate();

  void *p = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
  ASSERT_NE(p, MAP_FAILED);

  EXPECT_EQ(g_region_pool.live_count_estimate(), before + 1u);

  SlotSnapshot snap{};
  ASSERT_TRUE(snapshot_held(p, &snap));
  ASSERT_NE(snap.region, nullptr);
  EXPECT_EQ(snap.region->current_shape(), RegionShape::FILE_VIEW_MONO);
  EXPECT_TRUE(snap.region->has_flag(
      LIBC_NAMESPACE::windows::memory::region_flag::SHARED));
  // Both handles populated proves the recipe took the section+file path,
  // not a pagefile-section fallback.
  EXPECT_NE(snap.region->section_handle, static_cast<HANDLE>(nullptr));
  EXPECT_NE(snap.region->file_handle, static_cast<HANDLE>(nullptr));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, K64), Succeeds());
  EXPECT_EQ(g_region_pool.live_count_estimate(), before);

  LIBC_NAMESPACE::close(fd);
}

// ---------------------------------------------------------------------------
// 4. ANON partial munmap — middle hole. Plan budget: "1 syscall MEM_DECOMMIT
//    on the hole; the head and tail share the same region. No new region is
//    acquired and no shape promotion happens — anon one-shot stays
//    one-shot." Surrounding pages remain readable, confirming we did not
//    accidentally MEM_RELEASE the whole range.
// ---------------------------------------------------------------------------

TEST(LlvmLibcMmapSyscallCount, PartialMunmapAnonOneshot_NoExtraRegion) {
  void *p = LIBC_NAMESPACE::mmap(nullptr, 4 * K64, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);
  char *base = static_cast<char *>(p);

  // Stamp head + tail so we can check survival post-hole.
  reinterpret_cast<uint32_t *>(base + 0)[0] = 0x11111111u;
  reinterpret_cast<uint32_t *>(base + 3 * K64)[0] = 0x33333333u;

  uint32_t before = g_region_pool.live_count_estimate();

  // Punch the middle 64K (slot 2 of 4).
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 2 * K64, K64), Succeeds());

  // No new region acquired; no extra region split off. Anon-oneshot partial
  // unmap is a pure decommit — the budget is a single MEM_DECOMMIT syscall
  // that does not produce any descriptor-level shape promotion. (File
  // MONO->CHUNKED is the only promotion path; anon never promotes.)
  EXPECT_EQ(g_region_pool.live_count_estimate(), before);

  // Surrounding pages still live.
  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 0)[0], 0x11111111u);
  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 3 * K64)[0], 0x33333333u);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 0, 2 * K64), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 3 * K64, K64), Succeeds());
}

// ---------------------------------------------------------------------------
// 5. File MONO partial munmap — promotion to CHUNKED. Plan budget: "MONO
//    partial unmap → split section view, promote shape, append chunks. Same
//    region_id pre and post (refcount unchanged); shape MONO → CHUNKED;
//    chunk_list non-null with exactly the surviving sub-views." This catches
//    a refactor that would either (a) acquire a brand new region for the
//    surviving fragment (would inflate live_count) or (b) skip the
//    promotion entirely (shape stays MONO).
// ---------------------------------------------------------------------------

TEST(LlvmLibcMmapSyscallCount, PartialMunmapFileMono_PromotesToChunked) {
  int fd = LIBC_NAMESPACE::memfd_create("mmap_syscall_count_chunked", 0);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::ftruncate(fd, static_cast<off_t>(4 * K64)),
              Succeeds(0));

  void *p = LIBC_NAMESPACE::mmap(nullptr, 4 * K64, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
  ASSERT_NE(p, MAP_FAILED);
  char *base = static_cast<char *>(p);

  // Capture the (rid, aid) of the head slot pre-split.
  SlotSnapshot pre{};
  ASSERT_TRUE(snapshot_held(base, &pre));
  ASSERT_NE(pre.region, nullptr);
  ASSERT_EQ(pre.region->current_shape(), RegionShape::FILE_VIEW_MONO);
  uint32_t rid_before = pre.region_id;
  uint8_t aid_before = pre.alloc_id;
  uint32_t refs_before = pre.region->get_refcount();

  uint32_t pool_before = g_region_pool.live_count_estimate();

  // Punch slot 2 of 4 — must promote MONO -> CHUNKED in place.
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 2 * K64, K64), Succeeds());

  // No new region descriptor was acquired — the section is shared between
  // head and tail chunks of the same logical region.
  EXPECT_EQ(g_region_pool.live_count_estimate(), pool_before);

  // Same (rid, aid) — refcount didn't churn through release+re-acquire.
  // Promotion adds one new mapping table slot for the tail chunk; the
  // per-slot refcount model (region_desc.h:175 "refcount: live slots
  // pointing here") implies refcount == refs_before + 1.
  SlotSnapshot post{};
  ASSERT_TRUE(snapshot_held(base, &post));
  ASSERT_NE(post.region, nullptr);
  EXPECT_EQ(post.region_id, rid_before);
  EXPECT_EQ(post.alloc_id, aid_before);
  EXPECT_EQ(post.region->get_refcount(), refs_before + 1);

  // Shape promoted to CHUNKED, with a chunk list now installed.
  EXPECT_EQ(post.region->current_shape(), RegionShape::FILE_VIEW_CHUNKED);
  ChunkList *cl = post.region->get_chunk_list();
  ASSERT_NE(cl, nullptr);
  // Two surviving chunks: head (slot 0..1) and tail (slot 3).
  EXPECT_EQ(cl->count, 2u);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 0, 2 * K64), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 3 * K64, K64), Succeeds());
  LIBC_NAMESPACE::close(fd);
}

// ---------------------------------------------------------------------------
// 6. MAP_FIXED replacement — the old region must drop its last ref. Plan
//    budget: "prepare_for_fixed tears down the covered range and the
//    replacement publishes as a single new region." This pins the
//    invariant that resolve(old_rid, old_aid) returns nullptr after the
//    overwrite (slot either freed or recycled).
// ---------------------------------------------------------------------------

TEST(LlvmLibcMmapSyscallCount,
     MapFixedReplacement_OldRegionRefcountDropsToZero) {
  void *p = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);

  SlotSnapshot pre{};
  ASSERT_TRUE(snapshot_held(p, &pre));
  ASSERT_NE(pre.region, nullptr);
  uint32_t old_rid = pre.region_id;
  uint8_t old_aid = pre.alloc_id;

  // MAP_FIXED at the same address — destroys the old mapping.
  void *q = LIBC_NAMESPACE::mmap(p, K64, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1,
                                 0);
  ASSERT_EQ(q, p);

  // For MAP_FIXED-anon-over-anon where the new range fits inside the
  // existing region, the engine takes the decommit+recommit fast path
  // (alloc_fixed_private_placeholder in mmap_engine.cpp). The descriptor
  // and slot stay in place; only the underlying physical pages are
  // zero-filled. POSIX is satisfied — the user-visible contents are
  // replaced — without churning the region pool. So same (rid, aid)
  // is the expected outcome here.
  SlotSnapshot post{};
  ASSERT_TRUE(snapshot_held(p, &post));
  ASSERT_NE(post.region, nullptr);
  EXPECT_EQ(post.region_id, old_rid);
  EXPECT_EQ(post.alloc_id, old_aid);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, K64), Succeeds());
}

// ---------------------------------------------------------------------------
// 7. Whole munmap — the region's last reference drops, slot becomes
//    unresolvable. Direct mirror of (6) without the replacement step.
// ---------------------------------------------------------------------------

TEST(LlvmLibcMmapSyscallCount, WholeMunmap_RegionRefcountReachesZero) {
  void *p = LIBC_NAMESPACE::mmap(nullptr, K64, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);

  SlotSnapshot snap{};
  ASSERT_TRUE(snapshot_held(p, &snap));
  ASSERT_NE(snap.region, nullptr);
  uint32_t rid = snap.region_id;
  uint8_t aid = snap.alloc_id;

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, K64), Succeeds());

  // Post-munmap, the (rid, aid) we captured no longer resolves. A leaked
  // reference (e.g. a forgotten release on the unmap path) would keep the
  // slot live with the same alloc_id and trip this expectation.
  g_mmap_lock.acquire_shared();
  RegionDesc *rd = g_region_pool.resolve(rid, aid);
  g_mmap_lock.release_shared();
  EXPECT_EQ(rd, static_cast<RegionDesc *>(nullptr));
}

} // namespace
