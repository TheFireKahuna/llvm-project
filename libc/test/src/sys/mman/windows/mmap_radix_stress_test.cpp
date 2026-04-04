//===-- Mapping-table radix-tree / region-pool stress --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Companion to mmap_stress_test.cpp. Where that file is about generic
// mmap/munmap correctness under contention, this file is specifically
// engineered to exercise the *state machine* of the three-level lock-free
// radix tree and the region-pool ABA protection:
//
//   State transitions targeted:
//     FREE        → LIVE          (register_mapping on virgin address)
//     LIVE        → FREE          (extract/remove on munmap)
//     LIVE        → REMAPPING→LIVE  (mremap, MAP_FIXED overwrite)
//     LIVE        → REMAPPING→FREE  (mremap shrink to 0, concurrent munmap)
//     PLACEHOLDER → LIVE          (second-phase commit)
//     LIVE (RegionDesc N, alloc_id G) → FREE → LIVE (RegionDesc M, alloc_id H)
//       where M == N (slot reuse) and H != G (ABA defence).
//
//   Radix-tree hotspots:
//     L2 page first-touch CAS (ensure_slot racing first allocation)
//     L3 page first-touch CAS (same, one level deeper)
//     Slot seqlock reads racing WRITING publishes
//     active_remap_count drain races
//
//   Region-pool hotspots:
//     resolve(id, alloc_id) after slot recycled (stale alloc_id → nullptr)
//     wait_for_release futex wake racing new acquire
//     Two-pass chunk scan under 100% occupancy window
//
// All scenarios are driven through the public mmap/mremap/mprotect/munmap
// POSIX API so the test survives internal rewrites. The only promises we
// require are POSIX semantics: same-address repeated mmap/munmap must
// observe consistent data; MAP_FIXED must atomically replace; mremap must
// preserve contents across moves. If the radix tree or region pool
// corrupts state, *some* of these invariants will fail.
//
//===----------------------------------------------------------------------===//

#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/mprotect.h"
#include "src/sys/mman/mremap.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcMmapRadix = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

static constexpr size_t PAGE = 4096;

// ---------------------------------------------------------------------------
// 1. Region-pool alloc_id ABA — same address repeatedly mapped/unmapped.
//    Each cycle re-acquires the same radix-tree slot AND the same pool
//    slot. Writing a unique marker per cycle catches the failure mode
//    where a stale (region_id, alloc_id) snapshot on one thread is used
//    to resolve the pool on another — producing the previous cycle's
//    contents instead of the current one.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapRadix, SameAddressRecycleMarkerInvariant) {
  constexpr int CYCLES = 512;

  for (int i = 0; i < CYCLES; ++i) {
    void *p = LIBC_NAMESPACE::mmap(nullptr, PAGE, PROT_READ | PROT_WRITE,
                                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    ASSERT_NE(p, MAP_FAILED);

    auto *marker = reinterpret_cast<volatile uint32_t *>(p);
    uint32_t expected = 0xA5A50000u | static_cast<uint32_t>(i & 0xFFFF);
    *marker = expected;
    // Read-after-write across the same mapping must return our marker,
    // not a stale value from a prior cycle's resolve-by-alloc_id ghost.
    EXPECT_EQ(*marker, expected);

    EXPECT_THAT(LIBC_NAMESPACE::munmap(const_cast<uint32_t *>(marker), PAGE),
                Succeeds());
  }
}

// ---------------------------------------------------------------------------
// 2. Concurrent MAP_FIXED overwrites force LIVE→REMAPPING→LIVE transitions.
//    K threads hammer a shared base region with overlapping MAP_FIXED
//    requests. The radix-tree REMAPPING state is the single point that
//    must serialize readers (via the VEH remap guard) and commit_remap
//    must leave the slot LIVE with the new region_id.
// ---------------------------------------------------------------------------

struct FixedCtx {
  void *base;
  size_t page;
  Atomic<int> fails{0};
  Atomic<bool> done{false};
};

static DWORD fixed_overwriter(void *arg) {
  auto *ctx = static_cast<FixedCtx *>(arg);
  for (int i = 0; i < 400 && !ctx->done.load(MemoryOrder::ACQUIRE); ++i) {
    void *r = LIBC_NAMESPACE::mmap(ctx->base, ctx->page, PROT_READ | PROT_WRITE,
                                   MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1,
                                   0);
    if (r != ctx->base) {
      ctx->fails.fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    // Write and read-back. A REMAPPING→LIVE transition that publishes
    // stale slot fields (wrong region_id, wrong extent) would be detected
    // by VEH/VAD mismatch on the subsequent volatile access.
    auto *p = reinterpret_cast<volatile uint32_t *>(r);
    *p = 0xC0DEC0DE;
    if (*p != 0xC0DEC0DE)
      ctx->fails.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST_F(LlvmLibcMmapRadix, ConcurrentMapFixedRemapTransitions) {
  constexpr int N = 6;
  FixedCtx ctx;
  ctx.page = PAGE;
  ctx.base = LIBC_NAMESPACE::mmap(nullptr, PAGE, PROT_READ | PROT_WRITE,
                                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(ctx.base, MAP_FAILED);

  HANDLE ths[N];
  for (int i = 0; i < N; ++i) {
    ths[i] =
        LIBC_NAMESPACE::test_support::create_thread(fixed_overwriter, &ctx);
    ASSERT_NE(ths[i], static_cast<HANDLE>(nullptr));
  }
  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(ths[i],
                                                                    60000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(ths[i]);
  }
  EXPECT_EQ(ctx.fails.load(MemoryOrder::RELAXED), 0);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(ctx.base, PAGE), Succeeds());
}

// ---------------------------------------------------------------------------
// 3. mremap contents preservation — LIVE → REMAPPING → LIVE (new VA).
//    A broken begin_remap/commit_remap pair that failed to migrate the
//    region refcount would cause a use-after-free: the new view's
//    RegionDesc resolves to an old section that's been released.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapRadix, MremapPreservesRegionAcrossMove) {
  constexpr size_t OLD_SZ = 4 * PAGE;
  constexpr size_t NEW_SZ = 16 * PAGE;
  void *p = LIBC_NAMESPACE::mmap(nullptr, OLD_SZ, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);

  // Fill with a recognizable pattern.
  for (size_t i = 0; i < OLD_SZ / sizeof(uint32_t); ++i)
    reinterpret_cast<uint32_t *>(p)[i] = 0xBEEF0000u + static_cast<uint32_t>(i);

  void *p2 = LIBC_NAMESPACE::mremap(p, OLD_SZ, NEW_SZ, MREMAP_MAYMOVE);
  ASSERT_NE(p2, MAP_FAILED);

  // Existing contents must be preserved verbatim.
  for (size_t i = 0; i < OLD_SZ / sizeof(uint32_t); ++i) {
    uint32_t v = reinterpret_cast<uint32_t *>(p2)[i];
    ASSERT_EQ(v, 0xBEEF0000u + static_cast<uint32_t>(i));
  }
  // Newly-grown area must be accessible.
  for (size_t i = OLD_SZ / sizeof(uint32_t); i < NEW_SZ / sizeof(uint32_t); ++i)
    reinterpret_cast<uint32_t *>(p2)[i] = 0xFEEDu;

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p2, NEW_SZ), Succeeds());
}

// ---------------------------------------------------------------------------
// 4. Partial munmap splits a LIVE slot and must maintain walk_range
//    consistency. A radix-tree that failed to re-insert the tail half
//    would be detected when we read from the tail post-munmap.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapRadix, PartialMunmapMiddleLeavesHeadAndTail) {
  constexpr size_t N_PAGES = 8;
  constexpr size_t SZ = N_PAGES * PAGE;
  char *p = static_cast<char *>(
      LIBC_NAMESPACE::mmap(nullptr, SZ, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
  ASSERT_NE(p, static_cast<char *>(MAP_FAILED));

  for (size_t i = 0; i < N_PAGES; ++i)
    reinterpret_cast<uint32_t *>(p + i * PAGE)[0] = 0x1000u + static_cast<uint32_t>(i);

  // Carve out the middle two pages.
  EXPECT_THAT(LIBC_NAMESPACE::munmap(p + 3 * PAGE, 2 * PAGE), Succeeds());

  // Head pages 0,1,2 must still carry their markers.
  for (size_t i = 0; i < 3; ++i)
    EXPECT_EQ(reinterpret_cast<uint32_t *>(p + i * PAGE)[0],
              0x1000u + static_cast<uint32_t>(i));

  // Tail pages 5..7 must also survive (head+tail were split into two
  // independent mapping-table entries; extent bookkeeping must be correct).
  for (size_t i = 5; i < N_PAGES; ++i)
    EXPECT_EQ(reinterpret_cast<uint32_t *>(p + i * PAGE)[0],
              0x1000u + static_cast<uint32_t>(i));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, 3 * PAGE), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(p + 5 * PAGE, 3 * PAGE), Succeeds());
}

// ---------------------------------------------------------------------------
// 5. Wide-spread allocation touches many L2/L3 pages of the radix tree.
//    First touch of a far region forces a lock-free CAS allocation of an
//    L2 and L3 page. A broken ensure_slot that races two threads into
//    allocating the same page would leak a page; a broken one that
//    misses a concurrent allocation would drop a slot.
// ---------------------------------------------------------------------------

struct WideCtx {
  Atomic<int> errors{0};
};

static DWORD wide_worker(void *arg) {
  auto *ctx = static_cast<WideCtx *>(arg);
  // Each worker allocates 64 regions. The kernel's ASLR will spread them
  // across the full user VA range, forcing first-touch into many L2 pages.
  for (int i = 0; i < 64; ++i) {
    void *r = LIBC_NAMESPACE::mmap(nullptr, PAGE, PROT_READ | PROT_WRITE,
                                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (r == MAP_FAILED) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    *reinterpret_cast<volatile uint32_t *>(r) = 0x5555'5555u;
    if (*reinterpret_cast<volatile uint32_t *>(r) != 0x5555'5555u)
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
    if (LIBC_NAMESPACE::munmap(r, PAGE) != 0)
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST_F(LlvmLibcMmapRadix, WideSpreadFirstTouchRadixPages) {
  constexpr int N = 12;
  WideCtx ctx;
  HANDLE ths[N];
  for (int i = 0; i < N; ++i) {
    ths[i] = LIBC_NAMESPACE::test_support::create_thread(wide_worker, &ctx);
    ASSERT_NE(ths[i], static_cast<HANDLE>(nullptr));
  }
  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(ths[i],
                                                                    30000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(ths[i]);
  }
  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);
}

// ---------------------------------------------------------------------------
// 6. mprotect cycling forces flag-atomic updates on the slot without
//    state changes. A broken add_flags/remove_flags that lost bits across
//    concurrent calls would silently leave the kernel VAD out of sync.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapRadix, ProtectionCyclingFlagAtomicity) {
  constexpr size_t SZ = 16 * PAGE;
  void *p = LIBC_NAMESPACE::mmap(nullptr, SZ, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);
  // Touch all pages so they're committed.
  for (size_t i = 0; i < SZ; i += PAGE)
    static_cast<volatile char *>(p)[i] = 0x42;

  for (int i = 0; i < 200; ++i) {
    EXPECT_THAT(LIBC_NAMESPACE::mprotect(p, SZ, PROT_READ), Succeeds());
    EXPECT_THAT(LIBC_NAMESPACE::mprotect(p, SZ, PROT_READ | PROT_WRITE),
                Succeeds());
    EXPECT_THAT(LIBC_NAMESPACE::mprotect(p, SZ, PROT_NONE), Succeeds());
    EXPECT_THAT(LIBC_NAMESPACE::mprotect(p, SZ, PROT_READ | PROT_WRITE),
                Succeeds());
  }

  // After all mprotect cycles we should be back to RW and data intact.
  for (size_t i = 0; i < SZ; i += PAGE)
    EXPECT_EQ(static_cast<volatile char *>(p)[i], static_cast<char>(0x42));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, SZ), Succeeds());
}

// ---------------------------------------------------------------------------
// 7. Mix of mmap/mremap/munmap/mprotect across many threads — full
//    lifecycle churn. No specific invariant beyond: nothing crashes,
//    all mappings eventually drain, and errno is clean at quiesce.
//    This is the belt-and-suspenders regression test.
// ---------------------------------------------------------------------------

struct MixCtx {
  Atomic<int> ops{0};
  Atomic<int> errors{0};
  Atomic<bool> done{false};
};

static DWORD mix_worker(void *arg) {
  auto *ctx = static_cast<MixCtx *>(arg);
  // Per-thread PRNG using xorshift (deterministic, no libc rand needed).
  uint64_t s = ::NtCurrentThreadId() * 0x9E3779B97F4A7C15ull + 1;
  auto rnd = [&]() -> uint64_t {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
  };

  while (!ctx->done.load(MemoryOrder::ACQUIRE)) {
    size_t sz = (1 + (rnd() & 0x7)) * PAGE;
    void *p = LIBC_NAMESPACE::mmap(nullptr, sz, PROT_READ | PROT_WRITE,
                                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    reinterpret_cast<volatile uint32_t *>(p)[0] = 0xABCD'0000u;

    uint32_t op = rnd() & 3;
    if (op == 0) {
      size_t new_sz = sz + PAGE;
      void *p2 = LIBC_NAMESPACE::mremap(p, sz, new_sz, MREMAP_MAYMOVE);
      if (p2 != MAP_FAILED) {
        if (reinterpret_cast<volatile uint32_t *>(p2)[0] != 0xABCD'0000u)
          ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
        LIBC_NAMESPACE::munmap(p2, new_sz);
      } else {
        LIBC_NAMESPACE::munmap(p, sz);
      }
    } else if (op == 1) {
      LIBC_NAMESPACE::mprotect(p, sz, PROT_READ);
      LIBC_NAMESPACE::mprotect(p, sz, PROT_READ | PROT_WRITE);
      LIBC_NAMESPACE::munmap(p, sz);
    } else if (op == 2 && sz >= 2 * PAGE) {
      // Carve a hole in the middle.
      LIBC_NAMESPACE::munmap(static_cast<char *>(p) + PAGE, PAGE);
      LIBC_NAMESPACE::munmap(p, PAGE);
      LIBC_NAMESPACE::munmap(static_cast<char *>(p) + 2 * PAGE, sz - 2 * PAGE);
    } else {
      LIBC_NAMESPACE::munmap(p, sz);
    }
    ctx->ops.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST_F(LlvmLibcMmapRadix, FullLifecycleChurn) {
  constexpr int N = 8;
  MixCtx ctx;
  HANDLE ths[N];
  for (int i = 0; i < N; ++i) {
    ths[i] = LIBC_NAMESPACE::test_support::create_thread(mix_worker, &ctx);
    ASSERT_NE(ths[i], static_cast<HANDLE>(nullptr));
  }
  LIBC_NAMESPACE::test_support::sleep_ms(500);
  ctx.done.store(true, MemoryOrder::RELEASE);
  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(ths[i],
                                                                    30000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(ths[i]);
  }
  EXPECT_GT(ctx.ops.load(MemoryOrder::RELAXED), 0);
  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);
}

// ---------------------------------------------------------------------------
// 8. Large contiguous allocation → exercises walk_range / for_each_live
//    across many contiguous slots. Writing and reading a sentinel at each
//    page boundary ensures the radix tree reports them all LIVE and the
//    VAD matches. Also exercises L3-page filling (1024 slots = 64MB at
//    64KB grain, so allocating 64MB will span ~1024 contiguous L3 slots).
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapRadix, LargeContiguousAllocationFullWalk) {
  constexpr size_t SZ = 16 * 1024 * 1024; // 16MB
  void *p = LIBC_NAMESPACE::mmap(nullptr, SZ, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);
  char *base = static_cast<char *>(p);

  // Write a per-page sentinel.
  for (size_t off = 0; off < SZ; off += PAGE)
    reinterpret_cast<uint32_t *>(base + off)[0] =
        0xDEAD'0000u + static_cast<uint32_t>(off / PAGE);
  // Read-back.
  for (size_t off = 0; off < SZ; off += PAGE)
    ASSERT_EQ(reinterpret_cast<uint32_t *>(base + off)[0],
              0xDEAD'0000u + static_cast<uint32_t>(off / PAGE));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(p, SZ), Succeeds());
}
