//===-- Tests for va_tracker typed-ops public API ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// End-to-end tests for the Phase 1 va_tracker public API
// (`memory/va_tracker.h`). Exercises the typed-ops surface
// `acquire` / `release` / `replace` / `mutate` / `split` / `resolve` /
// `walk_range` plus `serialize_for_fork` / `replay_in_child`, composed
// over the full ART + interval skiplist + RegionDesc partition stack.
//
// Test coverage:
//   1.  AcquireResolveRoundTrip          — acquire + resolve sees a desc.
//   2.  AcquireOverlappingFailsWithEEXIST — second acquire on the same VA
//                                            returns EEXIST.
//   3.  ReleaseFreesBackingState         — release frees the VA;
//                                            resolve afterwards returns
//                                            ENOENT.
//   4.  ReplaceUpdatesBackingState       — replace overwrites the desc
//                                            with a fresh view_prot.
//   5.  MutateUpdatesFlagsViaDescMutator — mutate(callback) clones with
//                                            new flags published via Swap.
//   6.  SplitFragmentsRegion             — split at an interior boundary
//                                            yields two compatible descs.
//   7.  WalkRangeYieldsOrderedFragments  — walk_range over a multi-
//                                            region interval emits
//                                            ascending VA order.
//   8.  ResolveAcrossMutateProducesUpdatedView — resolve after mutate
//                                            observes the published clone.
//   9.  SerializeForForkEmitsAcquiredEntries   — serialize emits
//                                            entries for every acquired
//                                            POSIX-visible region.
//  10.  ReplayInChildReturnsZeroOnSuccess      — replay_in_child accepts
//                                            a serialized entry and
//                                            re-acquires it.
//
// VA bases are spaced 64 MiB apart in the 96 TiB region so concurrent
// runs of the same test EXE on one host do not collide. The kernel
// honours `MEM_RESERVE` at these addresses because the high quarter of
// user VA is otherwise unmapped.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"

namespace {

namespace vt = LIBC_NAMESPACE::windows::va_tracker;
using LIBC_NAMESPACE::cpp::MemoryOrder;

// 96 TiB. Well above any loader / heap / stack region on Windows x64 user
// VA. Per-test bases are kSpacing apart so a test's residue (in the rare
// case `release` partially fails) does not poison subsequent tests.
constexpr uintptr_t kTestBase = 0x600000000000ULL;
constexpr size_t kAllocGran = 64u * 1024u;
constexpr size_t kSpacing = 64u * 1024u * 1024u;

[[nodiscard]] vt::VaRange make_range(uintptr_t base, size_t bytes) {
  return vt::VaRange{reinterpret_cast<void *>(base), bytes};
}

[[nodiscard]] vt::AcquireMeta make_meta(DWORD view_prot = PAGE_READWRITE,
                                        uint16_t flags = 0) {
  vt::AcquireMeta m;
  m.view_prot = view_prot;
  m.flags = flags;
  return m;
}

// ------------------------- callbacks -------------------------------------

void set_committed_and_shared(vt::RegionDesc *desc, void * /*ctx*/) {
  desc->flags.store(static_cast<uint16_t>(vt::region_flag::COMMITTED |
                                          vt::region_flag::SHARED),
                    MemoryOrder::RELEASE);
}

struct WalkBuffer {
  static constexpr size_t kCap = 64;
  vt::VaRange ranges[kCap];
  size_t count{0};
};

void walk_collect(vt::VaRange covered, vt::RegionDesc * /*desc*/, void *ctx) {
  auto *wb = static_cast<WalkBuffer *>(ctx);
  if (wb->count >= WalkBuffer::kCap)
    return;
  wb->ranges[wb->count++] = covered;
}

struct SinkBuffer {
  static constexpr size_t kCap = 64;
  vt::ForkSnapshot::Entry entries[kCap];
  size_t count{0};
};

int sink_emit(void *ctx, vt::VaRange r, vt::RegionKind k,
              const vt::AcquireMeta &m,
              const vt::ProtectionRun *runs, uint32_t run_count) {
  auto *sb = static_cast<SinkBuffer *>(ctx);
  if (sb->count >= SinkBuffer::kCap)
    return -ENOSPC;
  auto &e = sb->entries[sb->count];
  e.range = r;
  e.kind = k;
  e.meta = m;
  const uint32_t cap = vt::kMaxProtectionRunsPerEntry;
  uint32_t n = run_count < cap ? run_count : cap;
  for (uint32_t i = 0; i < n; ++i)
    e.protection_runs[i] = runs[i];
  e.protection_count = n;
  sb->count++;
  return 0;
}

} // namespace

// --- 1. Acquire + resolve round-trip --------------------------------------
TEST(LlvmLibcVaTrackerTest, AcquireResolveRoundTrip) {
  uintptr_t base = kTestBase + 1 * kSpacing;
  auto ar = vt::acquire(make_range(base, kAllocGran),
                        vt::RegionKind::AnonPrivate, make_meta());
  ASSERT_FALSE(ar.has_value() == false);

  auto rr = vt::resolve(reinterpret_cast<void *>(base + 0x1000));
  ASSERT_FALSE(rr.has_value() == false);
  EXPECT_NE(rr.value().desc, static_cast<vt::RegionDesc *>(nullptr));

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// --- 2. Overlapping acquire fails with EEXIST -----------------------------
TEST(LlvmLibcVaTrackerTest, AcquireOverlappingFailsWithEEXIST) {
  uintptr_t base = kTestBase + 2 * kSpacing;
  auto first = vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta());
  ASSERT_FALSE(first.has_value() == false);

  auto second = vt::acquire(make_range(base, kAllocGran),
                            vt::RegionKind::AnonPrivate, make_meta());
  ASSERT_TRUE(second.has_value() == false);
  EXPECT_EQ(EEXIST, second.error());

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// --- 3. Release frees backing state ---------------------------------------
TEST(LlvmLibcVaTrackerTest, ReleaseFreesBackingState) {
  uintptr_t base = kTestBase + 3 * kSpacing;
  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonShared, make_meta())
                   .has_value() == false);

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));

  auto rr = vt::resolve(reinterpret_cast<void *>(base + 0x1000));
  ASSERT_TRUE(rr.has_value() == false);
  EXPECT_EQ(ENOENT, rr.error());
}

// --- 4. Replace overwrites desc state -------------------------------------
TEST(LlvmLibcVaTrackerTest, ReplaceUpdatesBackingState) {
  uintptr_t base = kTestBase + 4 * kSpacing;
  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate,
                           make_meta(PAGE_READWRITE))
                   .has_value() == false);

  EXPECT_EQ(0, vt::replace(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate,
                           make_meta(PAGE_READONLY)));

  auto rr = vt::resolve(reinterpret_cast<void *>(base));
  ASSERT_FALSE(rr.has_value() == false);
  ASSERT_NE(rr.value().desc, static_cast<vt::RegionDesc *>(nullptr));
  EXPECT_EQ(static_cast<uint32_t>(PAGE_READONLY),
            rr.value().desc->view_prot);

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// --- 5. Mutate updates flags via DescMutator ------------------------------
TEST(LlvmLibcVaTrackerTest, MutateUpdatesFlagsViaDescMutator) {
  uintptr_t base = kTestBase + 5 * kSpacing;
  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);

  EXPECT_EQ(0, vt::mutate(make_range(base, kAllocGran),
                          &set_committed_and_shared, nullptr));

  auto rr = vt::resolve(reinterpret_cast<void *>(base));
  ASSERT_FALSE(rr.has_value() == false);
  ASSERT_NE(rr.value().desc, static_cast<vt::RegionDesc *>(nullptr));
  uint16_t flags = rr.value().desc->flags_load();
  EXPECT_NE(0u,
            static_cast<uint32_t>(flags &
                                  static_cast<uint16_t>(
                                      vt::region_flag::COMMITTED)));
  EXPECT_NE(0u, static_cast<uint32_t>(
                    flags & static_cast<uint16_t>(vt::region_flag::SHARED)));

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// --- 6. Split fragments region into two halves ---------------------------
TEST(LlvmLibcVaTrackerTest, SplitFragmentsRegion) {
  uintptr_t base = kTestBase + 6 * kSpacing;
  size_t total = 4 * kAllocGran;
  ASSERT_FALSE(vt::acquire(make_range(base, total),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);

  uintptr_t boundary = base + 2 * kAllocGran;
  EXPECT_EQ(0, vt::split(reinterpret_cast<void *>(boundary)));

  WalkBuffer wb;
  vt::walk_range(make_range(base, total), &walk_collect, &wb);
  ASSERT_EQ(static_cast<size_t>(2), wb.count);
  EXPECT_EQ(base, reinterpret_cast<uintptr_t>(wb.ranges[0].start));
  EXPECT_EQ(static_cast<size_t>(2 * kAllocGran), wb.ranges[0].bytes);
  EXPECT_EQ(boundary, reinterpret_cast<uintptr_t>(wb.ranges[1].start));
  EXPECT_EQ(static_cast<size_t>(2 * kAllocGran), wb.ranges[1].bytes);

  // Single release over the whole post-split extent — both fragments
  // are fully inside the released range, so the survivor walk drops
  // both descs.
  EXPECT_EQ(0, vt::release(make_range(base, total)));
}

// --- 7. Walk_range yields ascending VA order -----------------------------
TEST(LlvmLibcVaTrackerTest, WalkRangeYieldsOrderedFragments) {
  uintptr_t b1 = kTestBase + 7 * kSpacing;
  uintptr_t b2 = b1 + 0x20000;
  uintptr_t b3 = b1 + 0x40000;

  // Non-monotonic acquire order. walk_range must still emit ascending.
  ASSERT_FALSE(vt::acquire(make_range(b2, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);
  ASSERT_FALSE(vt::acquire(make_range(b1, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);
  ASSERT_FALSE(vt::acquire(make_range(b3, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);

  WalkBuffer wb;
  vt::walk_range(make_range(b1, b3 + kAllocGran - b1), &walk_collect, &wb);
  ASSERT_EQ(static_cast<size_t>(3), wb.count);
  EXPECT_EQ(b1, reinterpret_cast<uintptr_t>(wb.ranges[0].start));
  EXPECT_EQ(b2, reinterpret_cast<uintptr_t>(wb.ranges[1].start));
  EXPECT_EQ(b3, reinterpret_cast<uintptr_t>(wb.ranges[2].start));

  EXPECT_EQ(0, vt::release(make_range(b1, kAllocGran)));
  EXPECT_EQ(0, vt::release(make_range(b2, kAllocGran)));
  EXPECT_EQ(0, vt::release(make_range(b3, kAllocGran)));
}

// --- 8. Resolve after mutate observes the new desc -----------------------
TEST(LlvmLibcVaTrackerTest, ResolveAcrossMutateProducesUpdatedView) {
  uintptr_t base = kTestBase + 8 * kSpacing;
  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);

  // Pre-mutate: COMMITTED must not yet be set.
  auto rr_pre = vt::resolve(reinterpret_cast<void *>(base));
  ASSERT_FALSE(rr_pre.has_value() == false);
  ASSERT_NE(rr_pre.value().desc, static_cast<vt::RegionDesc *>(nullptr));
  EXPECT_EQ(0u, static_cast<uint32_t>(
                    rr_pre.value().desc->flags_load() &
                    static_cast<uint16_t>(vt::region_flag::COMMITTED)));

  EXPECT_EQ(0, vt::mutate(make_range(base, kAllocGran),
                          &set_committed_and_shared, nullptr));

  // Post-mutate: a fresh resolve sees the Swap-published clone.
  auto rr_post = vt::resolve(reinterpret_cast<void *>(base));
  ASSERT_FALSE(rr_post.has_value() == false);
  ASSERT_NE(rr_post.value().desc, static_cast<vt::RegionDesc *>(nullptr));
  EXPECT_NE(0u, static_cast<uint32_t>(
                    rr_post.value().desc->flags_load() &
                    static_cast<uint16_t>(vt::region_flag::COMMITTED)));

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// --- 9. serialize_for_fork emits acquired entries ------------------------
TEST(LlvmLibcVaTrackerTest, SerializeForForkEmitsAcquiredEntries) {
  uintptr_t b1 = kTestBase + 9 * kSpacing;
  uintptr_t b2 = b1 + 0x40000;

  ASSERT_FALSE(vt::acquire(make_range(b1, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);
  ASSERT_FALSE(vt::acquire(make_range(b2, 2 * kAllocGran),
                           vt::RegionKind::AnonShared, make_meta())
                   .has_value() == false);

  SinkBuffer sb;
  vt::ForkSink sink;
  sink.emit = &sink_emit;
  sink.ctx = &sb;

  EXPECT_EQ(0, vt::serialize_for_fork(sink));

  // Lower-bound assertion: libc bootstrap may have its own
  // POSIX-visible regions; we only require that ours are present.
  bool saw_b1 = false;
  bool saw_b2 = false;
  for (size_t i = 0; i < sb.count; ++i) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(sb.entries[i].range.start);
    if (addr == b1 && sb.entries[i].range.bytes == kAllocGran)
      saw_b1 = true;
    if (addr == b2 && sb.entries[i].range.bytes == 2 * kAllocGran)
      saw_b2 = true;
  }
  EXPECT_TRUE(saw_b1);
  EXPECT_TRUE(saw_b2);

  EXPECT_EQ(0, vt::release(make_range(b1, kAllocGran)));
  EXPECT_EQ(0, vt::release(make_range(b2, 2 * kAllocGran)));
}

// --- 10. replay_in_child returns 0 on success ----------------------------
TEST(LlvmLibcVaTrackerTest, ReplayInChildReturnsZeroOnSuccess) {
  uintptr_t base = kTestBase + 10 * kSpacing;

  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);

  // Serialize, then build a single-entry snapshot containing just our
  // region so replay does not try to reinstall libc-bootstrap state on
  // top of the surviving live regions.
  SinkBuffer sb;
  vt::ForkSink sink;
  sink.emit = &sink_emit;
  sink.ctx = &sb;
  EXPECT_EQ(0, vt::serialize_for_fork(sink));

  vt::ForkSnapshot::Entry one_entry{};
  bool found = false;
  for (size_t i = 0; i < sb.count; ++i) {
    if (reinterpret_cast<uintptr_t>(sb.entries[i].range.start) == base) {
      one_entry = sb.entries[i];
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);

  // Release the original so the snapshot's VA is MEM_FREE for replay.
  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));

  vt::ForkSnapshot snap;
  snap.entries = &one_entry;
  snap.count = 1;
  EXPECT_EQ(0, vt::replay_in_child(snap));

  auto rr = vt::resolve(reinterpret_cast<void *>(base));
  EXPECT_FALSE(rr.has_value() == false);

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// --- 11. Fork serialization captures sub-region mprotect divergence -----
// A desc whose kernel state has been split by `mprotect` emits multiple
// `ProtectionRun` entries — one per uniform-protection sub-run — so the
// child reproduces the parent's exact protection layout.
TEST(LlvmLibcVaTrackerTest, SerializeEmitsProtectionRunsForDivergence) {
  // Use a 64 KiB anon-private region so we can directly mprotect a
  // sub-range without going through va_tracker's mprotect path
  // (which we don't want to test here — just the serialization
  // behavior given a kernel-side divergent VAD layout).
  auto chosen = vt::acquire_kernel_chosen(64u * 1024u,
                                           vt::RegionKind::AnonPrivate,
                                           make_meta(PAGE_READWRITE));
  ASSERT_FALSE(chosen.has_value() == false);
  uintptr_t base = reinterpret_cast<uintptr_t>(chosen.value());

  // Carve a 4 KiB read-only window at offset +4 KiB. After this the
  // 64 KiB VAD splits into three: [base, +4 KiB) RW, [+4 KiB, +8 KiB) R,
  // [+8 KiB, +64 KiB) RW.
  void *ro_addr = reinterpret_cast<void *>(base + 4u * 1024u);
  ULONG old_prot = 0;
  PVOID p = ro_addr;
  SIZE_T sz = 4u * 1024u;
  ASSERT_TRUE(NT_SUCCESS(::NtProtectVirtualMemory(
      ::NtCurrentProcess(), &p, &sz, PAGE_READONLY, &old_prot)));

  SinkBuffer sb;
  vt::ForkSink sink;
  sink.emit = &sink_emit;
  sink.ctx = &sb;
  EXPECT_EQ(0, vt::serialize_for_fork(sink));

  // Find the entry for our region.
  size_t idx = sb.count;
  for (size_t i = 0; i < sb.count; ++i) {
    if (reinterpret_cast<uintptr_t>(sb.entries[i].range.start) == base) {
      idx = i;
      break;
    }
  }
  ASSERT_LT(idx, sb.count);
  const auto &e = sb.entries[idx];

  // Expect 3 runs: RW / R / RW.
  EXPECT_EQ(static_cast<uint32_t>(3), e.protection_count);
  EXPECT_EQ(static_cast<uint32_t>(0),
            e.protection_runs[0].offset_from_range_lo);
  EXPECT_EQ(static_cast<uint32_t>(4u * 1024u),
            e.protection_runs[0].bytes);
  EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE),
            e.protection_runs[0].prot);

  EXPECT_EQ(static_cast<uint32_t>(4u * 1024u),
            e.protection_runs[1].offset_from_range_lo);
  EXPECT_EQ(static_cast<uint32_t>(4u * 1024u),
            e.protection_runs[1].bytes);
  EXPECT_EQ(static_cast<DWORD>(PAGE_READONLY),
            e.protection_runs[1].prot);

  EXPECT_EQ(static_cast<uint32_t>(8u * 1024u),
            e.protection_runs[2].offset_from_range_lo);
  EXPECT_EQ(static_cast<uint32_t>(56u * 1024u),
            e.protection_runs[2].bytes);
  EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE),
            e.protection_runs[2].prot);

  // First run's prot drives meta.view_prot.
  EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE), e.meta.view_prot);

  EXPECT_EQ(0, vt::release(make_range(base, 64u * 1024u)));
}

// ---------------------------------------------------------------------------
// Phase 2 — 4 KiB-granular split / release on already-tracked ranges.
//
// NT supports placeholder splits at any page-aligned offset (see
// `Placeholders.md` §2 empirical probes; the kernel rejects only sub-page
// offsets with STATUS_INVALID_PARAMETER_1). The substrate's typed-op API
// gates fresh-VA acquisition on 64 KiB allocation granularity but accepts
// page-aligned interior operations (`range_valid_interior` in
// `va_tracker_transaction.cpp`). These tests pin that contract.
// ---------------------------------------------------------------------------

namespace {
constexpr size_t kPage = 4u * 1024u;
} // namespace

// --- 11. Split at a 4 KiB-aligned boundary inside a 64 KiB desc ----------
TEST(LlvmLibcVaTrackerTest, SplitAtPageBoundaryFragmentsRegion) {
  uintptr_t base = kTestBase + 11 * kSpacing;
  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);

  uintptr_t boundary = base + kPage;
  EXPECT_EQ(0, vt::split(reinterpret_cast<void *>(boundary)));

  WalkBuffer wb;
  vt::walk_range(make_range(base, kAllocGran), &walk_collect, &wb);
  ASSERT_EQ(static_cast<size_t>(2), wb.count);
  EXPECT_EQ(base, reinterpret_cast<uintptr_t>(wb.ranges[0].start));
  EXPECT_EQ(kPage, wb.ranges[0].bytes);
  EXPECT_EQ(boundary, reinterpret_cast<uintptr_t>(wb.ranges[1].start));
  EXPECT_EQ(kAllocGran - kPage, wb.ranges[1].bytes);

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// --- 12. Split near the end (+60 KiB) ------------------------------------
TEST(LlvmLibcVaTrackerTest, SplitNearEndPageBoundary) {
  uintptr_t base = kTestBase + 12 * kSpacing;
  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);

  uintptr_t boundary = base + (kAllocGran - kPage);
  EXPECT_EQ(0, vt::split(reinterpret_cast<void *>(boundary)));

  WalkBuffer wb;
  vt::walk_range(make_range(base, kAllocGran), &walk_collect, &wb);
  ASSERT_EQ(static_cast<size_t>(2), wb.count);
  EXPECT_EQ(kAllocGran - kPage, wb.ranges[0].bytes);
  EXPECT_EQ(kPage, wb.ranges[1].bytes);

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// --- 13. Three-way split: release the middle 32 KiB slice ---------------
TEST(LlvmLibcVaTrackerTest, ThreeWaySplitReleasesMiddle) {
  uintptr_t base = kTestBase + 13 * kSpacing;
  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);

  uintptr_t mid_lo = base + (16u * 1024u);
  uintptr_t mid_hi = base + (48u * 1024u);
  EXPECT_EQ(0, vt::split(reinterpret_cast<void *>(mid_lo)));
  EXPECT_EQ(0, vt::split(reinterpret_cast<void *>(mid_hi)));

  // Release the middle 32 KiB slice. Page-aligned base + page-aligned
  // length — accepted by `range_valid_interior`.
  EXPECT_EQ(0, vt::release(make_range(mid_lo,
                                       static_cast<size_t>(mid_hi - mid_lo))));

  // Two surviving descs: [base, mid_lo) and [mid_hi, base + 64 KiB).
  WalkBuffer wb;
  vt::walk_range(make_range(base, kAllocGran), &walk_collect, &wb);
  ASSERT_EQ(static_cast<size_t>(2), wb.count);
  EXPECT_EQ(base, reinterpret_cast<uintptr_t>(wb.ranges[0].start));
  EXPECT_EQ(static_cast<size_t>(16u * 1024u), wb.ranges[0].bytes);
  EXPECT_EQ(mid_hi, reinterpret_cast<uintptr_t>(wb.ranges[1].start));
  EXPECT_EQ(static_cast<size_t>(16u * 1024u), wb.ranges[1].bytes);

  // The middle hole resolves to ENOENT.
  auto rr = vt::resolve(reinterpret_cast<void *>(base + (24u * 1024u)));
  ASSERT_TRUE(rr.has_value() == false);
  EXPECT_EQ(ENOENT, rr.error());

  // Cleanup: release surviving head and tail. Each is 16 KiB (page-
  // aligned but not alloc-granularity-aligned).
  EXPECT_EQ(0, vt::release(make_range(base, 16u * 1024u)));
  EXPECT_EQ(0, vt::release(make_range(mid_hi, 16u * 1024u)));
}

// --- 14. Split rejects sub-page boundary --------------------------------
TEST(LlvmLibcVaTrackerTest, SplitRejectsSubPageBoundary) {
  uintptr_t base = kTestBase + 14 * kSpacing;
  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);

  // Sub-page boundaries are rejected by `range_valid_interior`.
  EXPECT_EQ(EINVAL, vt::split(reinterpret_cast<void *>(base + 1)));
  EXPECT_EQ(EINVAL, vt::split(reinterpret_cast<void *>(base + 1024)));

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// --- 15. Release rejects sub-page range --------------------------------
TEST(LlvmLibcVaTrackerTest, ReleaseRejectsSubPageRange) {
  uintptr_t base = kTestBase + 15 * kSpacing;
  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_value() == false);

  // Page-aligned base but sub-page length.
  EXPECT_EQ(EINVAL, vt::release(make_range(base, 1024)));
  // Non-page-aligned base.
  EXPECT_EQ(EINVAL, vt::release(make_range(base + 1, kPage)));

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// ---------------------------------------------------------------------------
// Phase 9 — Sub-64 KiB acquire shrinks the placeholder pad.
//
// NT reserves placeholders at allocation granularity (64 KiB). When the
// caller asks for fewer bytes, the envelope's NT phase splits the
// placeholder at the user boundary and releases the pad to MEM_FREE
// before the commit step. Applies uniformly to all three entry points
// — `acquire` (hint path), `acquire_kernel_chosen`, and
// `acquire_kernel_chosen_32bit` — because the shrink lives inside
// `run_envelope` (see the `op.reserve_bytes > identity_bytes` branch
// in `va_tracker_transaction.cpp`). The user-visible desc covers
// exactly the page-aligned request; the rest of the 64 KiB granule is
// returned to the OS, not held committed for the life of the mapping.
// ---------------------------------------------------------------------------

namespace {
// Read MBI to verify that the VA at `addr` is MEM_FREE — i.e. the
// kernel reclaimed the pad. Kept inline rather than reaching into
// nt_pal so this test stays self-contained.
[[nodiscard]] bool va_is_free(void *addr) {
  ::MEMORY_BASIC_INFORMATION mbi{};
  ::SIZE_T ret = 0;
  ::NTSTATUS st = ::NtQueryVirtualMemory(
      ::NtCurrentProcess(), addr, ::MemoryBasicInformation, &mbi,
      sizeof(mbi), &ret);
  if (!NT_SUCCESS(st))
    return false;
  return mbi.State == static_cast<DWORD>(MEM_FREE);
}
} // namespace

// --- 16. acquire_kernel_chosen(4 KiB) shrinks the placeholder ------------
TEST(LlvmLibcVaTrackerTest, AcquireKernelChosenShrinksSubGranule) {
  auto chosen = vt::acquire_kernel_chosen(kPage, vt::RegionKind::AnonPrivate,
                                          make_meta());
  ASSERT_FALSE(chosen.has_value() == false);
  void *base = chosen.value();
  ASSERT_NE(base, static_cast<void *>(nullptr));

  // The user's 4 KiB is tracked and resolvable.
  auto rr = vt::resolve(base);
  ASSERT_FALSE(rr.has_value() == false);
  EXPECT_NE(rr.value().desc, static_cast<vt::RegionDesc *>(nullptr));

  // The pad at base + 4 KiB through base + 64 KiB is MEM_FREE — the
  // kernel reclaimed it via `nt_pal::pad_release_if_uncommitted`.
  void *pad = static_cast<char *>(base) + kPage;
  EXPECT_TRUE(va_is_free(pad));

  EXPECT_EQ(0, vt::release(make_range(reinterpret_cast<uintptr_t>(base),
                                       kPage)));
}

// --- 17. acquire_kernel_chosen(64 KiB) leaves the placeholder intact -----
TEST(LlvmLibcVaTrackerTest, AcquireKernelChosenExactGranuleSkipsShrink) {
  auto chosen = vt::acquire_kernel_chosen(kAllocGran,
                                           vt::RegionKind::AnonPrivate,
                                           make_meta());
  ASSERT_FALSE(chosen.has_value() == false);
  void *base = chosen.value();
  ASSERT_NE(base, static_cast<void *>(nullptr));

  // No shrink needed: bytes == kAllocGranularity. The placeholder
  // covers the full request; no MEM_FREE region adjacent.
  auto rr = vt::resolve(static_cast<char *>(base) + kPage);
  ASSERT_FALSE(rr.has_value() == false);
  EXPECT_NE(rr.value().desc, static_cast<vt::RegionDesc *>(nullptr));

  EXPECT_EQ(0, vt::release(make_range(reinterpret_cast<uintptr_t>(base),
                                       kAllocGran)));
}

// --- 18. acquire_kernel_chosen rejects sub-page bytes -------------------
TEST(LlvmLibcVaTrackerTest, AcquireKernelChosenRejectsSubPageBytes) {
  auto chosen = vt::acquire_kernel_chosen(1024, vt::RegionKind::AnonPrivate,
                                           make_meta());
  ASSERT_TRUE(chosen.has_value() == false);
  EXPECT_EQ(EINVAL, chosen.error());
}

// --- 19. acquire(hint, 4 KiB) shrinks the placeholder -------------------
TEST(LlvmLibcVaTrackerTest, AcquireHintShrinksSubGranule) {
  uintptr_t base = kTestBase + 19 * kSpacing;
  auto ref = vt::acquire(make_range(base, kPage),
                         vt::RegionKind::AnonPrivate, make_meta());
  ASSERT_FALSE(ref.has_value() == false);
  EXPECT_NE(ref.value().desc, static_cast<vt::RegionDesc *>(nullptr));

  // Pad at base + kPage through base + kAllocGran returned to MEM_FREE
  // by the envelope's shrink step.
  void *pad = reinterpret_cast<void *>(base + kPage);
  EXPECT_TRUE(va_is_free(pad));

  EXPECT_EQ(0, vt::release(make_range(base, kPage)));
}

// --- 20. acquire(hint, page-aligned > 64 KiB) shrinks the tail ----------
TEST(LlvmLibcVaTrackerTest, AcquireHintShrinksOverGranuleTail) {
  uintptr_t base = kTestBase + 20 * kSpacing;
  // 68 KiB = 64 KiB + 4 KiB. NT reserves 128 KiB; envelope shrinks to
  // 68 KiB, leaving 60 KiB of MEM_FREE adjacent.
  const size_t user_bytes = kAllocGran + kPage;
  auto ref = vt::acquire(make_range(base, user_bytes),
                         vt::RegionKind::AnonPrivate, make_meta());
  ASSERT_FALSE(ref.has_value() == false);

  // The user's full range is committed and tracked.
  void *interior_hi = reinterpret_cast<void *>(base + user_bytes - kPage);
  auto rr = vt::resolve(interior_hi);
  ASSERT_FALSE(rr.has_value() == false);
  EXPECT_NE(rr.value().desc, static_cast<vt::RegionDesc *>(nullptr));

  // Pad just past the user bytes is MEM_FREE.
  void *pad = reinterpret_cast<void *>(base + user_bytes);
  EXPECT_TRUE(va_is_free(pad));

  EXPECT_EQ(0, vt::release(make_range(base, user_bytes)));
}

// --- 21. acquire rejects sub-page lengths --------------------------------
TEST(LlvmLibcVaTrackerTest, AcquireHintRejectsSubPageSize) {
  uintptr_t base = kTestBase + 21 * kSpacing;
  // Page-aligned base but sub-page length.
  auto bad_size = vt::acquire(make_range(base, 1024),
                              vt::RegionKind::AnonPrivate, make_meta());
  ASSERT_TRUE(bad_size.has_value() == false);
  EXPECT_EQ(EINVAL, bad_size.error());
}

// --- 22. acquire honours a non-alloc-aligned page-aligned hint -----------
// NT requires the placeholder base to be alloc-aligned, but the public
// surface accepts any page-aligned hint. The substrate reserves at the
// enclosing alloc granule and shaves the prefix to MEM_FREE so the
// caller observes their requested address.
TEST(LlvmLibcVaTrackerTest, AcquireHintHonoursPageAlignedNonGranule) {
  uintptr_t alloc_base = kTestBase + 22 * kSpacing;
  uintptr_t hint = alloc_base + kPage;  // page-aligned, not alloc-aligned

  auto ref = vt::acquire(make_range(hint, kPage),
                         vt::RegionKind::AnonPrivate, make_meta());
  ASSERT_FALSE(ref.has_value() == false);

  // The desc covers exactly the user's hint range.
  auto rr = vt::resolve(reinterpret_cast<void *>(hint));
  ASSERT_FALSE(rr.has_value() == false);
  EXPECT_NE(rr.value().desc, static_cast<vt::RegionDesc *>(nullptr));

  // The prefix (alloc_base..hint) was shaved to MEM_FREE.
  EXPECT_TRUE(va_is_free(reinterpret_cast<void *>(alloc_base)));
  // The suffix (hint+kPage..alloc_base+kAllocGran) was released by the
  // envelope's shrink.
  EXPECT_TRUE(va_is_free(reinterpret_cast<void *>(hint + kPage)));

  EXPECT_EQ(0, vt::release(make_range(hint, kPage)));
}

// --- 23. acquire honours a page-aligned mid-granule hint at granule end --
// Edge case: hint sits at the last page of an alloc granule. After
// prefix shave, the placeholder covers exactly the user's page.
TEST(LlvmLibcVaTrackerTest, AcquireHintHonoursLastPageOfGranule) {
  uintptr_t alloc_base = kTestBase + 23 * kSpacing;
  uintptr_t hint = alloc_base + kAllocGran - kPage;  // last page

  auto ref = vt::acquire(make_range(hint, kPage),
                         vt::RegionKind::AnonPrivate, make_meta());
  ASSERT_FALSE(ref.has_value() == false);

  auto rr = vt::resolve(reinterpret_cast<void *>(hint));
  ASSERT_FALSE(rr.has_value() == false);

  // The prefix (alloc_base..hint, 60 KiB) is MEM_FREE.
  EXPECT_TRUE(va_is_free(reinterpret_cast<void *>(alloc_base)));

  EXPECT_EQ(0, vt::release(make_range(hint, kPage)));
}
