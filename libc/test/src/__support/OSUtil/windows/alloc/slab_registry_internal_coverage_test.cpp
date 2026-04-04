//===-- SlabRegistry directory page LIBC_INTERNAL coverage -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Phase 3 of the SlabPool→VaSubstrate migration moved SlabRegistry's
// L1/L2/L3 radix backing from raw `page_alloc` onto substrate-served
// Small slots (with L1 routed through a class picked by VA width via
// `pick_class_for_directory`). The deliverable: every directory page is
// stamped LIBC_INTERNAL through substrate's per-arena registration.
//
// This test walks the live registry from L1 down through one populated
// L2/L3 chain and snapshots each level against `g_mapping_table`. A
// failure means a directory page escaped substrate ownership — the
// migration's Phase 3 regressed.
//
// The walk relies on three public surfaces of `SlabRegistry`:
//
//   * `slab_registry.l1_` — atomic pointer to the L1 root, public
//     mutable.
//   * `slab_registry.decompose(addr)` — public Key extractor; produces
//     the L1 / L2 / L3 indices for a given slab base.
//   * `SlabRegistry::L2Entry` / `Bitmap` typedefs — public.
//
// `SlabRegistry::pick_class_for_directory` is also exercised here as a
// pure unit test, because CI only runs on whichever VA width the host
// reports. A 48-bit VA host will only ever see the Small branch at
// runtime; a 57-bit VA host will only ever see the XLarge branch. The
// branch-coverage test pins both.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/OSUtil/windows/alloc/va_substrate.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/region_desc.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_helpers.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_types.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/stdlib/free.h"
#include "src/stdlib/malloc.h"
#include "test/UnitTest/Test.h"

#include <stddef.h>
#include <stdint.h>

namespace {

using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::internal::slab_registry;
using LIBC_NAMESPACE::internal::SlabRegistry;
using LIBC_NAMESPACE::windows::alloc::SubSlotClass;
using LIBC_NAMESPACE::windows::g_mapping_table;
using LIBC_NAMESPACE::windows::SlotSnapshot;
using LIBC_NAMESPACE::windows::memory::RegionShape;

// Walks `p` back to its OS-level allocation base, then snapshots the
// mapping table at that base. The substrate registers each arena keyed
// on its base, and `g_mapping_table.snapshot` matches only at the exact
// registered key — so any address inside the arena needs MBI lookup
// first. See slab_body_internal_coverage_test.cpp for the longer
// rationale.
LIBC_INLINE bool snapshot_is_libc_internal(void *p) {
  MEMORY_BASIC_INFORMATION mbi{};
  NTSTATUS s = LIBC_NAMESPACE::nt_helpers::query_basic_info(p, mbi);
  if (s < 0)
    return false;
  if (mbi.AllocationBase == nullptr)
    return false;
  SlotSnapshot snap{};
  if (!g_mapping_table.snapshot(mbi.AllocationBase, &snap))
    return false;
  if (snap.region == nullptr)
    return false;
  return snap.region->current_shape() == RegionShape::LIBC_INTERNAL;
}

} // namespace

// ---------------------------------------------------------------------------
// L1 directory base is LIBC_INTERNAL after first allocation.
// SlabRegistry::ensure_init runs lazily on first SlabPool::alloc — a
// single warmup malloc forces the code path. After init, `slab_registry.l1_`
// is the substrate-served L1 root; its base must carry the stamp.
// ---------------------------------------------------------------------------

TEST(LlvmLibcSlabRegistryCoverage, L1DirectoryBaseIsLibcInternal) {
  // Force ensure_init by routing one allocation through SlabPool. Most
  // tests in this binary will already have done this transitively, but
  // we don't rely on test ordering — the warmup is cheap and idempotent.
  void *warmup = LIBC_NAMESPACE::malloc(48);
  ASSERT_NE(warmup, nullptr);

  auto *l1 = slab_registry.l1_.load(MemoryOrder::ACQUIRE);
  ASSERT_NE(l1, static_cast<SlabRegistry::L1Entry *>(nullptr));
  EXPECT_TRUE(snapshot_is_libc_internal(static_cast<void *>(l1)));

  LIBC_NAMESPACE::free(warmup);
}

// ---------------------------------------------------------------------------
// L2 + L3 pages reachable from a live slab are LIBC_INTERNAL. We pick the
// slab base for our own malloc, decompose it into L1/L2/L3 indices, walk
// the radix to obtain each backing page address, and snapshot each.
// ---------------------------------------------------------------------------

TEST(LlvmLibcSlabRegistryCoverage, L2AndL3DirectoryPagesAreLibcInternal) {
  void *p = LIBC_NAMESPACE::malloc(48);
  ASSERT_NE(p, nullptr);

  // Default slab arena is 64 KB (mask = 0xFFFF on the low bits).
  const uintptr_t slab_base =
      reinterpret_cast<uintptr_t>(p) & ~uintptr_t{0xFFFF};
  SlabRegistry::Key key = slab_registry.decompose(slab_base);

  // L1 must be initialized (we just malloc'd) and its key.l1 entry must
  // resolve to a live L2 page (insert() populates this lazily on slab
  // birth, and our slab is alive).
  auto *l1 = slab_registry.l1_.load(MemoryOrder::ACQUIRE);
  ASSERT_NE(l1, static_cast<SlabRegistry::L1Entry *>(nullptr));

  SlabRegistry::L2Entry *l2 = l1[key.l1].load(MemoryOrder::ACQUIRE);
  ASSERT_NE(l2, static_cast<SlabRegistry::L2Entry *>(nullptr));
  EXPECT_TRUE(snapshot_is_libc_internal(static_cast<void *>(l2)));

  SlabRegistry::Bitmap *l3 = l2[key.l2].load(MemoryOrder::ACQUIRE);
  ASSERT_NE(l3, static_cast<SlabRegistry::Bitmap *>(nullptr));
  EXPECT_TRUE(snapshot_is_libc_internal(static_cast<void *>(l3)));

  LIBC_NAMESPACE::free(p);
}

// ---------------------------------------------------------------------------
// Branch coverage for `pick_class_for_directory`. CI only runs on one VA
// width per host, so the runtime path can only ever exercise one branch
// per build. This synthetic-input unit test covers the full ladder
// (Small / Medium / Large / XLarge / Huge) so a regression in the dispatch
// table — e.g. swapping a boundary condition — surfaces immediately
// regardless of the host's VA width.
//
// Bytes inputs chosen at and around each tier boundary: Small 16 KB,
// Medium 64 KB, Large 128 KB, XLarge 256 KB. Anything strictly past
// 256 KB routes to Huge.
// ---------------------------------------------------------------------------

TEST(LlvmLibcSlabRegistryCoverage, PickClassForDirectoryCoversAllBranches) {
  using LIBC_NAMESPACE::windows::alloc::SubSlotClass;

  // Small ≤ 16 KB — covers the 48-bit VA L1 (512 B) and the L2/L3 (8 KB)
  // pages.
  EXPECT_EQ(static_cast<unsigned>(SlabRegistry::pick_class_for_directory(512)),
            static_cast<unsigned>(SubSlotClass::Small));
  EXPECT_EQ(
      static_cast<unsigned>(SlabRegistry::pick_class_for_directory(8 * 1024)),
      static_cast<unsigned>(SubSlotClass::Small));
  EXPECT_EQ(
      static_cast<unsigned>(SlabRegistry::pick_class_for_directory(16 * 1024)),
      static_cast<unsigned>(SubSlotClass::Small));

  // Medium (16 KB, 64 KB].
  EXPECT_EQ(
      static_cast<unsigned>(SlabRegistry::pick_class_for_directory(48 * 1024)),
      static_cast<unsigned>(SubSlotClass::Medium));
  EXPECT_EQ(
      static_cast<unsigned>(SlabRegistry::pick_class_for_directory(64 * 1024)),
      static_cast<unsigned>(SubSlotClass::Medium));

  // Large (64 KB, 128 KB].
  EXPECT_EQ(
      static_cast<unsigned>(SlabRegistry::pick_class_for_directory(128 * 1024)),
      static_cast<unsigned>(SubSlotClass::Large));

  // XLarge (128 KB, 256 KB] — what the 57-bit VA L1 actually selects.
  EXPECT_EQ(
      static_cast<unsigned>(SlabRegistry::pick_class_for_directory(256 * 1024)),
      static_cast<unsigned>(SubSlotClass::XLarge));

  // Huge (> 256 KB).
  EXPECT_EQ(
      static_cast<unsigned>(SlabRegistry::pick_class_for_directory(257 * 1024)),
      static_cast<unsigned>(SubSlotClass::Huge));
  EXPECT_EQ(static_cast<unsigned>(
                SlabRegistry::pick_class_for_directory(1 * 1024 * 1024)),
            static_cast<unsigned>(SubSlotClass::Huge));
}
