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
              const vt::AcquireMeta &m) {
  auto *sb = static_cast<SinkBuffer *>(ctx);
  if (sb->count >= SinkBuffer::kCap)
    return -ENOSPC;
  sb->entries[sb->count].range = r;
  sb->entries[sb->count].kind = k;
  sb->entries[sb->count].meta = m;
  sb->count++;
  return 0;
}

} // namespace

// --- 1. Acquire + resolve round-trip --------------------------------------
TEST(LlvmLibcVaTrackerTest, AcquireResolveRoundTrip) {
  uintptr_t base = kTestBase + 1 * kSpacing;
  auto ar = vt::acquire(make_range(base, kAllocGran),
                        vt::RegionKind::AnonPrivate, make_meta());
  ASSERT_FALSE(ar.has_error());

  auto rr = vt::resolve(reinterpret_cast<void *>(base + 0x1000));
  ASSERT_FALSE(rr.has_error());
  EXPECT_NE(rr.value().desc, static_cast<vt::RegionDesc *>(nullptr));

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// --- 2. Overlapping acquire fails with EEXIST -----------------------------
TEST(LlvmLibcVaTrackerTest, AcquireOverlappingFailsWithEEXIST) {
  uintptr_t base = kTestBase + 2 * kSpacing;
  auto first = vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta());
  ASSERT_FALSE(first.has_error());

  auto second = vt::acquire(make_range(base, kAllocGran),
                            vt::RegionKind::AnonPrivate, make_meta());
  ASSERT_TRUE(second.has_error());
  EXPECT_EQ(EEXIST, second.error());

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// --- 3. Release frees backing state ---------------------------------------
TEST(LlvmLibcVaTrackerTest, ReleaseFreesBackingState) {
  uintptr_t base = kTestBase + 3 * kSpacing;
  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonShared, make_meta())
                   .has_error());

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));

  auto rr = vt::resolve(reinterpret_cast<void *>(base + 0x1000));
  ASSERT_TRUE(rr.has_error());
  EXPECT_EQ(ENOENT, rr.error());
}

// --- 4. Replace overwrites desc state -------------------------------------
TEST(LlvmLibcVaTrackerTest, ReplaceUpdatesBackingState) {
  uintptr_t base = kTestBase + 4 * kSpacing;
  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate,
                           make_meta(PAGE_READWRITE))
                   .has_error());

  EXPECT_EQ(0, vt::replace(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate,
                           make_meta(PAGE_READONLY)));

  auto rr = vt::resolve(reinterpret_cast<void *>(base));
  ASSERT_FALSE(rr.has_error());
  ASSERT_NE(rr.value().desc, static_cast<vt::RegionDesc *>(nullptr));
  EXPECT_EQ(static_cast<DWORD>(PAGE_READONLY), rr.value().desc->view_prot);

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}

// --- 5. Mutate updates flags via DescMutator ------------------------------
TEST(LlvmLibcVaTrackerTest, MutateUpdatesFlagsViaDescMutator) {
  uintptr_t base = kTestBase + 5 * kSpacing;
  ASSERT_FALSE(vt::acquire(make_range(base, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_error());

  EXPECT_EQ(0, vt::mutate(make_range(base, kAllocGran),
                          &set_committed_and_shared, nullptr));

  auto rr = vt::resolve(reinterpret_cast<void *>(base));
  ASSERT_FALSE(rr.has_error());
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
                   .has_error());

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
                   .has_error());
  ASSERT_FALSE(vt::acquire(make_range(b1, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_error());
  ASSERT_FALSE(vt::acquire(make_range(b3, kAllocGran),
                           vt::RegionKind::AnonPrivate, make_meta())
                   .has_error());

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
                   .has_error());

  // Pre-mutate: COMMITTED must not yet be set.
  auto rr_pre = vt::resolve(reinterpret_cast<void *>(base));
  ASSERT_FALSE(rr_pre.has_error());
  ASSERT_NE(rr_pre.value().desc, static_cast<vt::RegionDesc *>(nullptr));
  EXPECT_EQ(0u, static_cast<uint32_t>(
                    rr_pre.value().desc->flags_load() &
                    static_cast<uint16_t>(vt::region_flag::COMMITTED)));

  EXPECT_EQ(0, vt::mutate(make_range(base, kAllocGran),
                          &set_committed_and_shared, nullptr));

  // Post-mutate: a fresh resolve sees the Swap-published clone.
  auto rr_post = vt::resolve(reinterpret_cast<void *>(base));
  ASSERT_FALSE(rr_post.has_error());
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
                   .has_error());
  ASSERT_FALSE(vt::acquire(make_range(b2, 2 * kAllocGran),
                           vt::RegionKind::AnonShared, make_meta())
                   .has_error());

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
                   .has_error());

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
  EXPECT_FALSE(rr.has_error());

  EXPECT_EQ(0, vt::release(make_range(base, kAllocGran)));
}
