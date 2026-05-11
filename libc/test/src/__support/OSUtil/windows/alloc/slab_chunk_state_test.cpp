//===-- alloc::slab_chunk_state hermetic suite ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for the §9.3.10.1 sub-slab decommit substrate. The header is
// pure-arithmetic / pure-atomic with no syscall dependencies, so the test
// runs hermetic with no Tier-A bring-up requirements beyond the standard
// LibcTest hermetic frame.
//
// Targets:
//   * Geometry constants align (kSlabPageBytes == 16 * kSubPageBytes).
//   * init_subpage_mask_all_committed produces 0xFFFF.
//   * mark_subpage_committed / mark_subpage_decommitted round-trip every
//     sub-page index, idempotency reported in the return value.
//   * Cross-bit isolation: setting bit i never disturbs bit j ≠ i.
//   * subpage_index_for_offset / subpage_range_for_slot edge cases at
//     16 B / 8 KiB / 16 KiB / 32 KiB / 64 KiB slot sizes.
//   * subpage_range_to_mask matches expected bit pattern for several
//     spans, including the degenerate full-page case.
//   * subpage_address_for produces the correct VA offset for every idx.
//   * compute_idle_subpages excludes any bit set in the live mask; sanity
//     on the all-committed / all-live boundary cases.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/alloc/slab_chunk_state.h"
#include "test/UnitTest/Test.h"

#include <stddef.h>
#include <stdint.h>

namespace {

using LIBC_NAMESPACE::windows::alloc::compute_idle_subpages;
using LIBC_NAMESPACE::windows::alloc::init_subpage_mask_all_committed;
using LIBC_NAMESPACE::windows::alloc::is_subpage_committed;
using LIBC_NAMESPACE::windows::alloc::kSlabPageBytes;
using LIBC_NAMESPACE::windows::alloc::kSubPageBytes;
using LIBC_NAMESPACE::windows::alloc::kSubPageMaskAllCommitted;
using LIBC_NAMESPACE::windows::alloc::kSubPageMaskAllDecommitted;
using LIBC_NAMESPACE::windows::alloc::kSubPagesPerSlab;
using LIBC_NAMESPACE::windows::alloc::mark_subpage_committed;
using LIBC_NAMESPACE::windows::alloc::mark_subpage_decommitted;
using LIBC_NAMESPACE::windows::alloc::snapshot_subpage_mask;
using LIBC_NAMESPACE::windows::alloc::subpage_address_for;
using LIBC_NAMESPACE::windows::alloc::subpage_index_for_offset;
using LIBC_NAMESPACE::windows::alloc::subpage_range_for_slot;
using LIBC_NAMESPACE::windows::alloc::subpage_range_to_mask;
using LIBC_NAMESPACE::windows::alloc::SubPageCommitMask;
using LIBC_NAMESPACE::windows::alloc::SubPageRange;

} // namespace

TEST(LlvmLibcSlabChunkStateTest, GeometryConstants) {
  EXPECT_EQ(kSubPageBytes, static_cast<size_t>(4 * 1024));
  EXPECT_EQ(kSlabPageBytes, static_cast<size_t>(64 * 1024));
  EXPECT_EQ(static_cast<size_t>(kSubPagesPerSlab), static_cast<size_t>(16));
  EXPECT_EQ(static_cast<size_t>(kSubPagesPerSlab) * kSubPageBytes,
            kSlabPageBytes);
  EXPECT_EQ(kSubPageMaskAllCommitted, static_cast<uint16_t>(0xFFFFu));
  EXPECT_EQ(kSubPageMaskAllDecommitted, static_cast<uint16_t>(0u));
}

TEST(LlvmLibcSlabChunkStateTest, InitProducesAllCommitted) {
  SubPageCommitMask m;
  init_subpage_mask_all_committed(m);
  EXPECT_EQ(snapshot_subpage_mask(m), kSubPageMaskAllCommitted);
  for (uint8_t i = 0; i < kSubPagesPerSlab; ++i)
    EXPECT_TRUE(is_subpage_committed(m, i));
}

TEST(LlvmLibcSlabChunkStateTest, MarkDecommittedRoundTripEveryIndex) {
  SubPageCommitMask m;
  init_subpage_mask_all_committed(m);
  for (uint8_t i = 0; i < kSubPagesPerSlab; ++i) {
    // First decommit reports the prior set bit (true => transition).
    EXPECT_TRUE(mark_subpage_decommitted(m, i));
    EXPECT_FALSE(is_subpage_committed(m, i));
    // Second decommit is idempotent (false => no transition).
    EXPECT_FALSE(mark_subpage_decommitted(m, i));
    EXPECT_FALSE(is_subpage_committed(m, i));
    // Re-arm.
    EXPECT_TRUE(mark_subpage_committed(m, i));
    EXPECT_TRUE(is_subpage_committed(m, i));
    // Idempotent re-arm.
    EXPECT_FALSE(mark_subpage_committed(m, i));
    EXPECT_TRUE(is_subpage_committed(m, i));
  }
  // After full round-trip, mask should be back to all-committed.
  EXPECT_EQ(snapshot_subpage_mask(m), kSubPageMaskAllCommitted);
}

TEST(LlvmLibcSlabChunkStateTest, CrossBitIsolation) {
  SubPageCommitMask m;
  init_subpage_mask_all_committed(m);
  // Decommit even-indexed sub-pages; verify odd ones stay set.
  for (uint8_t i = 0; i < kSubPagesPerSlab; i += 2)
    EXPECT_TRUE(mark_subpage_decommitted(m, i));
  uint16_t snap = snapshot_subpage_mask(m);
  EXPECT_EQ(snap, static_cast<uint16_t>(0xAAAAu));
  for (uint8_t i = 0; i < kSubPagesPerSlab; ++i) {
    if (i % 2 == 0)
      EXPECT_FALSE(is_subpage_committed(m, i));
    else
      EXPECT_TRUE(is_subpage_committed(m, i));
  }
}

TEST(LlvmLibcSlabChunkStateTest, SubPageIndexForOffset) {
  EXPECT_EQ(subpage_index_for_offset(0u), static_cast<uint8_t>(0));
  EXPECT_EQ(subpage_index_for_offset(kSubPageBytes - 1),
            static_cast<uint8_t>(0));
  EXPECT_EQ(subpage_index_for_offset(kSubPageBytes), static_cast<uint8_t>(1));
  EXPECT_EQ(subpage_index_for_offset(kSlabPageBytes - 1),
            static_cast<uint8_t>(15));
}

TEST(LlvmLibcSlabChunkStateTest, SubPageRangeForSmallSlots) {
  // 16 B class, slot 0 fits in sub-page 0.
  SubPageRange r = subpage_range_for_slot(16, 0);
  EXPECT_EQ(r.first, static_cast<uint8_t>(0));
  EXPECT_EQ(r.last, static_cast<uint8_t>(0));

  // 16 B class, last slot (slot 4095) is at offset 65520 → sub-page 15.
  r = subpage_range_for_slot(16, 4095);
  EXPECT_EQ(r.first, static_cast<uint8_t>(15));
  EXPECT_EQ(r.last, static_cast<uint8_t>(15));

  // 4 KiB slot exactly fills one sub-page.
  r = subpage_range_for_slot(kSubPageBytes, 0);
  EXPECT_EQ(r.first, static_cast<uint8_t>(0));
  EXPECT_EQ(r.last, static_cast<uint8_t>(0));
  r = subpage_range_for_slot(kSubPageBytes, 15);
  EXPECT_EQ(r.first, static_cast<uint8_t>(15));
  EXPECT_EQ(r.last, static_cast<uint8_t>(15));
}

TEST(LlvmLibcSlabChunkStateTest, SubPageRangeForMultiSubpageSlots) {
  // 8 KiB slot spans two sub-pages.
  SubPageRange r = subpage_range_for_slot(8 * 1024, 0);
  EXPECT_EQ(r.first, static_cast<uint8_t>(0));
  EXPECT_EQ(r.last, static_cast<uint8_t>(1));
  r = subpage_range_for_slot(8 * 1024, 7);
  EXPECT_EQ(r.first, static_cast<uint8_t>(14));
  EXPECT_EQ(r.last, static_cast<uint8_t>(15));

  // 16 KiB slot spans four sub-pages.
  r = subpage_range_for_slot(16 * 1024, 0);
  EXPECT_EQ(r.first, static_cast<uint8_t>(0));
  EXPECT_EQ(r.last, static_cast<uint8_t>(3));
  r = subpage_range_for_slot(16 * 1024, 3);
  EXPECT_EQ(r.first, static_cast<uint8_t>(12));
  EXPECT_EQ(r.last, static_cast<uint8_t>(15));

  // 32 KiB slot spans eight sub-pages.
  r = subpage_range_for_slot(32 * 1024, 0);
  EXPECT_EQ(r.first, static_cast<uint8_t>(0));
  EXPECT_EQ(r.last, static_cast<uint8_t>(7));
  r = subpage_range_for_slot(32 * 1024, 1);
  EXPECT_EQ(r.first, static_cast<uint8_t>(8));
  EXPECT_EQ(r.last, static_cast<uint8_t>(15));

  // 64 KiB single-slot page covers all sub-pages.
  r = subpage_range_for_slot(kSlabPageBytes, 0);
  EXPECT_EQ(r.first, static_cast<uint8_t>(0));
  EXPECT_EQ(r.last, static_cast<uint8_t>(15));
}

TEST(LlvmLibcSlabChunkStateTest, SubPageRangeToMask) {
  // Single-bit ranges.
  EXPECT_EQ(subpage_range_to_mask(SubPageRange{0, 0}),
            static_cast<uint16_t>(0x0001u));
  EXPECT_EQ(subpage_range_to_mask(SubPageRange{15, 15}),
            static_cast<uint16_t>(0x8000u));

  // Multi-bit spans.
  EXPECT_EQ(subpage_range_to_mask(SubPageRange{2, 5}),
            static_cast<uint16_t>(0x003Cu)); // bits 2,3,4,5 → 0b111100
  EXPECT_EQ(subpage_range_to_mask(SubPageRange{0, 7}),
            static_cast<uint16_t>(0x00FFu));
  EXPECT_EQ(subpage_range_to_mask(SubPageRange{8, 15}),
            static_cast<uint16_t>(0xFF00u));

  // Full-page span.
  EXPECT_EQ(subpage_range_to_mask(SubPageRange{0, 15}),
            static_cast<uint16_t>(0xFFFFu));
}

TEST(LlvmLibcSlabChunkStateTest, SubPageAddressFor) {
  alignas(kSubPageBytes) unsigned char buf[kSlabPageBytes] = {};
  void *base = buf;
  for (uint8_t i = 0; i < kSubPagesPerSlab; ++i) {
    void *expected =
        static_cast<void *>(buf + static_cast<size_t>(i) * kSubPageBytes);
    EXPECT_EQ(subpage_address_for(base, i), expected);
  }
}

TEST(LlvmLibcSlabChunkStateTest, ComputeIdleSubpagesBoundaries) {
  // All committed, nothing live → every bit is idle.
  EXPECT_EQ(compute_idle_subpages(kSubPageMaskAllCommitted, 0u),
            kSubPageMaskAllCommitted);

  // All committed, every sub-page has a live slot → no idle bits.
  EXPECT_EQ(compute_idle_subpages(kSubPageMaskAllCommitted,
                                  kSubPageMaskAllCommitted),
            static_cast<uint16_t>(0u));

  // Already-decommitted sub-page never re-emits decommit.
  EXPECT_EQ(compute_idle_subpages(0u, 0u), static_cast<uint16_t>(0u));

  // Mixed: even-indexed committed, every other sub-page live.
  uint16_t committed = 0x5555u;        // bits 0,2,4,...,14
  uint16_t live = 0xAAAAu;             // bits 1,3,5,...,15
  // Idle = committed AND NOT live = 0x5555 AND 0x5555 = 0x5555.
  EXPECT_EQ(compute_idle_subpages(committed, live),
            static_cast<uint16_t>(0x5555u));

  // Live overlaps committed: any overlap excludes the bit from idle.
  committed = 0xFFFFu;
  live = 0x000Fu;                      // sub-pages 0..3 hold a live slot
  EXPECT_EQ(compute_idle_subpages(committed, live),
            static_cast<uint16_t>(0xFFF0u));
}
