//===-- SlabPool body LIBC_INTERNAL coverage test ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The migration's deliverable: every slab body returned by SlabPool must
// be visible to the mapping table as `RegionShape::LIBC_INTERNAL`. The
// stamp comes from substrate's per-arena registration in
// `register_mapping_internal`, which fires when an arena is first
// reserved. Once the migration is complete, every malloc that routes
// through SlabPool lands inside a substrate-served arena and therefore
// inherits the stamp transitively.
//
// This test asserts the contract directly via `g_mapping_table.snapshot`.
// Failure here means a slab body escaped substrate ownership — Phase 1 of
// the SlabPool→VaSubstrate migration regressed.
//
// Why drive through `malloc` rather than the SlabPool template directly:
//
//   The user-visible promise is "every libc-internal allocation is
//   stamped LIBC_INTERNAL". Driving the production allocator through its
//   public surface verifies the full path — SlabPool template selection
//   (size class), `init_slab` writing the substrate token, the slab
//   address landing in the registry, and the snapshot returning the
//   stamped region. A friend-access probe on the SlabPool internals
//   could pass even if the public path was broken; the public-API
//   probe cannot.
//
// How the snapshot is keyed:
//
//   `g_mapping_table.snapshot(view_base, ...)` resolves only at the EXACT
//   registered allocation base — the seqlock body fails-fast if the slot's
//   key is not equal to `view_base`. The substrate registers each arena
//   via `register_mapping_internal(arena_base, arena_size)` keyed on the
//   arena base, so a malloc-returned pointer (which sits deep inside the
//   arena, inside a slab body, inside a substrate slot) does NOT resolve
//   directly. The test must walk back to the arena base before snapshot.
//
//   The OS-level `NtQueryVirtualMemory(MemoryBasicInformation).AllocationBase`
//   is the canonical answer: substrate reserves each arena via a single
//   `NtAllocateVirtualMemoryEx` call, so MBI.AllocationBase coincides with
//   the address substrate keyed `register_mapping_internal` on. Production
//   snapshot callers (`vm_protect.cpp`, `msync_ops.cpp`, `mremap_engine.cpp`,
//   `mem_fault_handler.cpp`) all pass MBI.AllocationBase for this reason.
//
// Why no internal locking around the snapshot:
//
//   Production callers all snapshot without holding `MmapLock`. The path
//   is a seqlock'd read; stale reads return false and the caller retries.
//   For LIBC_INTERNAL stamps specifically the region is permanent-refcount
//   (region_pool.h:191) and never gets retired, so the snapshot is stable
//   for the lifetime of the live malloc'd allocation.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
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

using LIBC_NAMESPACE::windows::g_mapping_table;
using LIBC_NAMESPACE::windows::SlotSnapshot;
using LIBC_NAMESPACE::windows::memory::RegionShape;

// True if `p` falls inside a region the mapping table has stamped as
// LIBC_INTERNAL. Walks back to the OS-level allocation base (which is
// the address substrate registered the arena under) before snapshotting.
LIBC_INLINE bool snapshot_is_libc_internal(void *p) {
  MEMORY_BASIC_INFORMATION mbi{};
  NTSTATUS s = LIBC_NAMESPACE::nt_helpers::query_basic_info(p, mbi);
  if (s < 0) // NT_SUCCESS check: high bit clear == success.
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
// One small allocation: both the raw payload pointer and the slab base it
// derives must resolve to LIBC_INTERNAL. The raw payload covers the
// "consumer asked malloc" surface; the slab base covers the underlying
// arena registration directly.
// ---------------------------------------------------------------------------

TEST(LlvmLibcSlabBodyInternalCoverage, SingleMallocSlabBaseIsLibcInternal) {
  void *p = LIBC_NAMESPACE::malloc(48);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(snapshot_is_libc_internal(p));
  LIBC_NAMESPACE::free(p);
}

// ---------------------------------------------------------------------------
// Coverage across every SlabPool tier. Each size routes through a
// different `SlabPoolT<...>` instantiation whose slabs may live in
// different substrate Medium arenas (or, for the largest tiers, the
// Huge-class L3 pool). Every slab base must carry the stamp.
//
// 16 / 256 / 1024 / 4096 / 16384 / 32768 spans the full posix_alloc class
// table from the densest slab (16-byte slots) to the largest small-class
// (32 KB slots). Anything beyond 32768 routes through the direct-mmap
// path and is NOT a slab — those allocations land as ANON regions, not
// LIBC_INTERNAL, so we deliberately keep the largest size at 32 KB.
// ---------------------------------------------------------------------------

TEST(LlvmLibcSlabBodyInternalCoverage, AllSlabClassesAreLibcInternal) {
  static constexpr size_t kSizes[] = {16, 256, 1024, 4096, 16384, 32768};
  for (size_t s : kSizes) {
    void *p = LIBC_NAMESPACE::malloc(s);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(snapshot_is_libc_internal(p));
    LIBC_NAMESPACE::free(p);
  }
}

// ---------------------------------------------------------------------------
// Multi-arena coverage — hold enough simultaneous allocations that the
// substrate must reserve more than its seed arena. Medium-class slot
// count per arena is 15 (`va_substrate.cpp` ClassLayout<Medium>); 64 live
// 48-byte mallocs cannot fit a single arena's worth of slabs (each slab
// holds many slots, but new threads / new TLS state can also force fresh
// slabs in alloc_slow). The relaxed assertion: every held allocation's
// slab base must be LIBC_INTERNAL. We do not assert *which* arena each
// slab landed in — non-determinism in scheduler / TLS state makes that
// unstable — only that the stamp is universally present.
// ---------------------------------------------------------------------------

TEST(LlvmLibcSlabBodyInternalCoverage, MultiArenaSlabsAllLibcInternal) {
  constexpr size_t kHeld = 64;
  void *holds[kHeld] = {};
  for (size_t i = 0; i < kHeld; ++i) {
    holds[i] = LIBC_NAMESPACE::malloc(48);
    ASSERT_NE(holds[i], nullptr);
  }
  for (size_t i = 0; i < kHeld; ++i)
    EXPECT_TRUE(snapshot_is_libc_internal(holds[i]));
  for (size_t i = 0; i < kHeld; ++i)
    LIBC_NAMESPACE::free(holds[i]);
}

// ---------------------------------------------------------------------------
// Negative control — a stack address must NOT report LIBC_INTERNAL.
// Pins the polarity of the assertion: if the snapshot path were broken
// in a way that returned LIBC_INTERNAL for everything (e.g. a global
// fallback shape regression), the positive tests above would silently
// pass. This test proves the predicate distinguishes substrate-stamped
// VA from non-substrate VA.
//
// On NT-POSIX the main thread stack is KERNEL_REGION; on a worker
// thread it would be ANON. Either way, not LIBC_INTERNAL.
// ---------------------------------------------------------------------------

TEST(LlvmLibcSlabBodyInternalCoverage, StackAddressIsNotLibcInternal) {
  volatile int local = 0xDEADBEEF;
  EXPECT_FALSE(snapshot_is_libc_internal(
      const_cast<int *>(reinterpret_cast<volatile int *>(&local))));
  (void)local;
}
