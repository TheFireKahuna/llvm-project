//===-- Stress tests for Windows mmap/munmap/mremap/mprotect --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Exercises the crash-prone paths in the Windows mmap implementation:
//
//   1. Concurrent mmap/munmap — stresses MappingTable contention.
//   2. Thread stack pattern — mmap(PROT_NONE) + mprotect.
//   3. Partial munmap — placeholder split/coalesce.
//   4. Rapid alloc/free churn — slot reuse and placeholder lifecycle.
//   5. Protection transitions — PROT_NONE→RW→R→NONE→RW.
//   6. MAP_FIXED replacement — single-thread overlay.
//   7. Concurrent MAP_FIXED on adjacent pages.
//   8. Concurrent MAP_FIXED on same address — true contention.
//   9. MAP_FIXED_NOREPLACE vs concurrent free — atomic placeholder claim.
//  10. Concurrent mmap + mremap + munmap — full lifecycle contention.
//  11. Tombstone accumulation — distinct-address churn.
//
// NOTE: This test still drives the legacy mmap engines under
// `memory/legacy/` — the per-TEST comment blocks below name legacy
// machinery verbatim (`MmapLockWriterGuard`, `MappingTable`, region_id
// state-machine slots). Those comments are intentionally not rewritten
// in this sweep; they will be reworded when the Layer 8 P3 cutover
// rewires the underlying mmap implementations to typed va_tracker ops,
// at which point both the test bodies and the comments need to land
// together.
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mmap.h"
#include "src/sys/mman/mprotect.h"
#include "src/sys/mman/mremap.h"
#include "src/sys/mman/munmap.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcMmapStressTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// ---------------------------------------------------------------------------
// 1. Concurrent mmap/munmap from multiple threads.
//    Each thread allocates, writes, reads, and frees. Tests MappingTable
//    slot allocation contention and VirtualAlloc2 under load.
// ---------------------------------------------------------------------------

struct MmapThreadCtx {
  Atomic<int> errors{0};
  Atomic<int> success{0};
};

static DWORD mmap_worker(void *arg) {
  auto *ctx = static_cast<MmapThreadCtx *>(arg);
  for (int i = 0; i < 200; i++) {
    size_t size = 4096;
    void *addr = LIBC_NAMESPACE::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (addr == MAP_FAILED) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }

    // Write and verify.
    volatile int *p = reinterpret_cast<volatile int *>(addr);
    *p = 0xDEADBEEF;
    if (*p != static_cast<int>(0xDEADBEEF)) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
    }

    if (LIBC_NAMESPACE::munmap(addr, size) != 0)
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
    else
      ctx->success.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST_F(LlvmLibcMmapStressTest, ConcurrentAllocFree) {
  MmapThreadCtx ctx;
  constexpr int N = 8;
  HANDLE threads[N];

  for (int i = 0; i < N; i++)
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(mmap_worker, &ctx);

  for (int i = 0; i < N; i++) {
    DWORD r =
        LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 30000);
    EXPECT_EQ(r,
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(threads[i]);
  }

  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);
  EXPECT_EQ(ctx.success.load(MemoryOrder::RELAXED), N * 200);
}

// ---------------------------------------------------------------------------
// 2. Thread stack pattern: mmap(PROT_NONE) + mprotect(PROT_READ|WRITE)
//    This is what Thread::run does for every new thread. Must not crash.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapStressTest, ThreadStackPattern) {
  constexpr int ITERATIONS = 100;
  constexpr size_t STACK_SIZE = 64 * 1024; // 64KB
  constexpr size_t GUARD_SIZE = 4096;

  for (int i = 0; i < ITERATIONS; i++) {
    // Reserve entire range with PROT_NONE (placeholder).
    void *base = LIBC_NAMESPACE::mmap(nullptr, STACK_SIZE + GUARD_SIZE,
                                      PROT_NONE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    ASSERT_NE(base, MAP_FAILED);

    // Commit the usable stack portion (skip guard page).
    char *stack_start = reinterpret_cast<char *>(base) + GUARD_SIZE;
    int ret = LIBC_NAMESPACE::mprotect(stack_start, STACK_SIZE,
                                       PROT_READ | PROT_WRITE);
    ASSERT_EQ(ret, 0);

    // Write to first and last pages of the committed region.
    stack_start[0] = 'A';
    stack_start[STACK_SIZE - 1] = 'Z';
    EXPECT_EQ(stack_start[0], 'A');
    EXPECT_EQ(stack_start[STACK_SIZE - 1], 'Z');

    // Free everything.
    EXPECT_THAT(LIBC_NAMESPACE::munmap(base, STACK_SIZE + GUARD_SIZE),
                Succeeds());
  }
}

// ---------------------------------------------------------------------------
// 3. Partial munmap stress: allocate a large region, then unmap interior
//    pages. Tests placeholder split/coalesce logic.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapStressTest, PartialMunmapChurn) {
  constexpr int ITERATIONS = 50;
  constexpr size_t PAGE = 4096;
  constexpr size_t TOTAL = 8 * PAGE;

  for (int i = 0; i < ITERATIONS; i++) {
    void *base = LIBC_NAMESPACE::mmap(nullptr, TOTAL, PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    ASSERT_NE(base, MAP_FAILED);

    char *p = reinterpret_cast<char *>(base);

    // Write sentinels across all pages.
    for (size_t j = 0; j < 8; j++)
      p[j * PAGE] = static_cast<char>('A' + j);

    // Unmap middle 2 pages (pages 3 and 4).
    EXPECT_THAT(LIBC_NAMESPACE::munmap(p + 3 * PAGE, 2 * PAGE), Succeeds());

    // Surrounding pages should still be accessible.
    EXPECT_EQ(p[0], 'A');
    EXPECT_EQ(p[2 * PAGE], 'C');
    EXPECT_EQ(p[5 * PAGE], 'F');
    EXPECT_EQ(p[7 * PAGE], 'H');

    // Unmap the remaining parts.
    EXPECT_THAT(LIBC_NAMESPACE::munmap(p, 3 * PAGE), Succeeds());
    EXPECT_THAT(LIBC_NAMESPACE::munmap(p + 5 * PAGE, 3 * PAGE), Succeeds());
  }
}

// ---------------------------------------------------------------------------
// 4. Rapid alloc/free churn: single-page allocations freed immediately.
//    Stresses MappingTable slot reuse and VirtualAlloc2 placeholder recycling.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapStressTest, RapidChurn) {
  constexpr int ITERATIONS = 1000;
  for (int i = 0; i < ITERATIONS; i++) {
    void *addr = LIBC_NAMESPACE::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    ASSERT_NE(addr, MAP_FAILED);
    // Quick write to ensure the page is committed.
    *reinterpret_cast<volatile int *>(addr) = i;
    EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, 4096), Succeeds());
  }
}

// ---------------------------------------------------------------------------
// 5. Protection transitions: tests the full lifecycle of a region through
//    multiple mprotect calls. Catches bugs in placeholder→commit→decommit
//    state tracking.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapStressTest, ProtectionTransitions) {
  constexpr size_t SIZE = 4 * 4096;

  for (int round = 0; round < 50; round++) {
    // Start with PROT_NONE (reserved placeholder).
    void *addr = LIBC_NAMESPACE::mmap(nullptr, SIZE, PROT_NONE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    ASSERT_NE(addr, MAP_FAILED);

    // PROT_NONE → PROT_READ|WRITE (commit).
    ASSERT_EQ(LIBC_NAMESPACE::mprotect(addr, SIZE, PROT_READ | PROT_WRITE), 0);
    char *p = reinterpret_cast<char *>(addr);
    p[0] = 'X';
    p[SIZE - 1] = 'Y';

    // PROT_READ|WRITE → PROT_READ (downgrade).
    ASSERT_EQ(LIBC_NAMESPACE::mprotect(addr, SIZE, PROT_READ), 0);
    EXPECT_EQ(p[0], 'X');
    EXPECT_EQ(p[SIZE - 1], 'Y');

    // PROT_READ → PROT_NONE (decommit-like).
    ASSERT_EQ(LIBC_NAMESPACE::mprotect(addr, SIZE, PROT_NONE), 0);

    // PROT_NONE → PROT_READ|WRITE (recommit).
    ASSERT_EQ(LIBC_NAMESPACE::mprotect(addr, SIZE, PROT_READ | PROT_WRITE), 0);
    // Data may be zero-filled after decommit/recommit.
    p[0] = 'Z';
    EXPECT_EQ(p[0], 'Z');

    EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, SIZE), Succeeds());
  }
}

// ---------------------------------------------------------------------------
// 6. MAP_FIXED replacement: allocate, then MAP_FIXED at the same address
//    to replace. Exercises MmapLockWriterGuard + MEM_REPLACE_PLACEHOLDER
//    atomic VA swap + RegionPool refcount=0 release of the displaced region.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapStressTest, MapFixedReplacement) {
  constexpr size_t SIZE = 4096;

  for (int round = 0; round < 50; round++) {
    void *addr = LIBC_NAMESPACE::mmap(nullptr, SIZE, PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    ASSERT_NE(addr, MAP_FAILED);

    int *p = reinterpret_cast<int *>(addr);
    p[0] = 0x11111111;

    // MAP_FIXED replaces the mapping — old data should be gone.
    void *fixed = LIBC_NAMESPACE::mmap(addr, SIZE, PROT_READ | PROT_WRITE,
                                       MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED,
                                       -1, 0);
    ASSERT_NE(fixed, MAP_FAILED);
    ASSERT_EQ(fixed, addr);

    // New mapping should be zero-initialized.
    EXPECT_EQ(p[0], 0);

    // Write to verify it's usable.
    p[0] = 0x22222222;
    EXPECT_EQ(p[0], 0x22222222);

    EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, SIZE), Succeeds());
  }
}

// ---------------------------------------------------------------------------
// 7. Concurrent MAP_FIXED on adjacent addresses — exercises
//    MmapLockWriterGuard writer-preference under multi-thread contention
//    on disjoint slots of one parent region.
// ---------------------------------------------------------------------------

struct FixedCtx {
  void *base;
  size_t page_size;
  Atomic<int> errors{0};
};

static DWORD fixed_worker(void *arg) {
  auto *ctx = static_cast<FixedCtx *>(arg);
  // Each thread repeatedly MAP_FIXED-replaces its own page within the
  // larger reserved region.
  DWORD tid = ::NtCurrentThreadId();
  int page_idx = static_cast<int>(tid % 4); // 4 pages
  char *target = reinterpret_cast<char *>(ctx->base) +
                 page_idx * ctx->page_size;

  for (int i = 0; i < 50; i++) {
    void *r = LIBC_NAMESPACE::mmap(target, ctx->page_size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED,
                                   -1, 0);
    if (r == MAP_FAILED) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    volatile int *p = reinterpret_cast<volatile int *>(r);
    *p = static_cast<int>(tid);
    if (*p != static_cast<int>(tid))
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST_F(LlvmLibcMmapStressTest, ConcurrentMapFixed) {
  constexpr size_t PAGE = 4096;
  constexpr size_t TOTAL = 4 * PAGE;

  // Reserve a 4-page region.
  void *base = LIBC_NAMESPACE::mmap(nullptr, TOTAL, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(base, MAP_FAILED);

  FixedCtx ctx;
  ctx.base = base;
  ctx.page_size = PAGE;

  constexpr int N = 4;
  HANDLE threads[N];
  for (int i = 0; i < N; i++)
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(fixed_worker, &ctx);

  for (int i = 0; i < N; i++) {
    LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 30000);
    ::NtClose(threads[i]);
  }

  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base, TOTAL), Succeeds());
}

// ---------------------------------------------------------------------------
// 8. Concurrent MAP_FIXED on the SAME address — true contention.
//    Exercises MEM_REPLACE_PLACEHOLDER atomicity vs concurrent claimers
//    on the same VA: the slot cycles LIVE → REMAPPING → LIVE under the
//    MmapLockWriterGuard writer-preference drain, and the (region_id,
//    alloc_id) tuple advances each round so stale snapshots fail safely.
// ---------------------------------------------------------------------------

struct SameAddrCtx {
  void *target;
  size_t size;
  Atomic<int> errors{0};
  Atomic<int> wins{0};
};

static DWORD same_addr_worker(void *arg) {
  auto *ctx = static_cast<SameAddrCtx *>(arg);
  for (int i = 0; i < 100; i++) {
    void *r = LIBC_NAMESPACE::mmap(ctx->target, ctx->size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED,
                                   -1, 0);
    if (r == MAP_FAILED) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    if (r != ctx->target) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
      LIBC_NAMESPACE::munmap(r, ctx->size);
      continue;
    }
    // Write our TID to verify we own the page.
    volatile DWORD *p = reinterpret_cast<volatile DWORD *>(r);
    *p = ::NtCurrentThreadId();
    ctx->wins.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST_F(LlvmLibcMmapStressTest, ConcurrentMapFixedSameAddress) {
  constexpr size_t SIZE = 4096;

  void *base = LIBC_NAMESPACE::mmap(nullptr, SIZE, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(base, MAP_FAILED);

  SameAddrCtx ctx;
  ctx.target = base;
  ctx.size = SIZE;

  constexpr int N = 4;
  HANDLE threads[N];
  for (int i = 0; i < N; i++)
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(same_addr_worker, &ctx);

  for (int i = 0; i < N; i++) {
    LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 30000);
    ::NtClose(threads[i]);
  }

  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);
  // All 4 threads × 100 iterations should succeed (each MAP_FIXED replaces).
  EXPECT_EQ(ctx.wins.load(MemoryOrder::RELAXED), N * 100);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(base, SIZE), Succeeds());
}

// ---------------------------------------------------------------------------
// 9. MAP_FIXED_NOREPLACE vs concurrent munmap — tests the atomic
//    placeholder claim. One thread frees pages; others try NOREPLACE
//    on the same addresses. NOREPLACE must return EEXIST or succeed
//    (never corrupt state or crash).
// ---------------------------------------------------------------------------

struct NoreplaceCtx {
  void *pages[8];
  size_t page_size;
  Atomic<int> started{0};
  Atomic<int> errors{0};
  Atomic<int> eexist{0};
  Atomic<int> claimed{0};
};

static DWORD noreplace_claimer(void *arg) {
  auto *ctx = static_cast<NoreplaceCtx *>(arg);
  ctx->started.fetch_add(1, MemoryOrder::RELEASE);
  // Wait for all threads to start.
  while (ctx->started.load(MemoryOrder::ACQUIRE) < 3)
    ::NtYieldExecution();

  for (int round = 0; round < 20; round++) {
    for (int i = 0; i < 8; i++) {
      void *r = LIBC_NAMESPACE::mmap(
          ctx->pages[i], ctx->page_size, PROT_READ | PROT_WRITE,
          MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
      if (r == MAP_FAILED) {
        // EEXIST is the expected race outcome.
        ctx->eexist.fetch_add(1, MemoryOrder::RELAXED);
      } else if (r == ctx->pages[i]) {
        ctx->claimed.fetch_add(1, MemoryOrder::RELAXED);
        // Immediately free so other threads can try.
        LIBC_NAMESPACE::munmap(r, ctx->page_size);
      } else {
        ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
        LIBC_NAMESPACE::munmap(r, ctx->page_size);
      }
    }
  }
  return 0;
}

static DWORD noreplace_freer(void *arg) {
  auto *ctx = static_cast<NoreplaceCtx *>(arg);
  ctx->started.fetch_add(1, MemoryOrder::RELEASE);
  while (ctx->started.load(MemoryOrder::ACQUIRE) < 3)
    ::NtYieldExecution();

  for (int round = 0; round < 20; round++) {
    for (int i = 0; i < 8; i++) {
      // Try to free and re-allocate at the same address.
      LIBC_NAMESPACE::munmap(ctx->pages[i], ctx->page_size);
      void *r = LIBC_NAMESPACE::mmap(ctx->pages[i], ctx->page_size,
                                     PROT_READ | PROT_WRITE,
                                     MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED,
                                     -1, 0);
      if (r != ctx->pages[i] && r != MAP_FAILED)
        LIBC_NAMESPACE::munmap(r, ctx->page_size);
    }
  }
  return 0;
}

TEST_F(LlvmLibcMmapStressTest, NoreplaceConcurrentFree) {
  constexpr size_t PAGE = 4096;
  NoreplaceCtx ctx;
  ctx.page_size = PAGE;

  // Allocate 8 pages at known addresses.
  void *region = LIBC_NAMESPACE::mmap(nullptr, 8 * PAGE,
                                      PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(region, MAP_FAILED);
  for (int i = 0; i < 8; i++)
    ctx.pages[i] = reinterpret_cast<char *>(region) + i * PAGE;

  HANDLE threads[3];
  threads[0] =
      LIBC_NAMESPACE::test_support::create_thread(noreplace_claimer, &ctx);
  threads[1] =
      LIBC_NAMESPACE::test_support::create_thread(noreplace_claimer, &ctx);
  threads[2] =
      LIBC_NAMESPACE::test_support::create_thread(noreplace_freer, &ctx);

  for (int i = 0; i < 3; i++) {
    LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 30000);
    ::NtClose(threads[i]);
  }

  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);

  // Clean up any remaining pages (some may have been freed by racers).
  for (int i = 0; i < 8; i++)
    LIBC_NAMESPACE::munmap(ctx.pages[i], PAGE);
}

// ---------------------------------------------------------------------------
// 10. Concurrent mmap + mremap + munmap — full lifecycle contention.
//     Exercises radix-tree slot registration through the WRITING/REMAPPING
//     state machine; handle ownership lives on the per-region RegionDesc
//     (one section/file handle per region, not per slot), so concurrent
//     mremap moves only walk the RegionPool refcount, not handle duplication.
// ---------------------------------------------------------------------------

struct LifecycleCtx {
  Atomic<int> errors{0};
  Atomic<int> success{0};
};

static DWORD lifecycle_worker(void *arg) {
  auto *ctx = static_cast<LifecycleCtx *>(arg);
  constexpr size_t PAGE = 4096;

  for (int i = 0; i < 100; i++) {
    // Allocate 2 pages.
    void *addr = LIBC_NAMESPACE::mmap(nullptr, 2 * PAGE,
                                      PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (addr == MAP_FAILED) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }

    // Write sentinel.
    volatile int *p = reinterpret_cast<volatile int *>(addr);
    *p = 0xCAFEBABE;

    // Grow via mremap(MAYMOVE).
    void *grown = LIBC_NAMESPACE::mremap(addr, 2 * PAGE, 4 * PAGE,
                                         MREMAP_MAYMOVE);
    if (grown == MAP_FAILED) {
      // mremap failure is not necessarily an error under contention —
      // but the mapping should still be valid at the original address.
      if (*p != static_cast<int>(0xCAFEBABE))
        ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
      LIBC_NAMESPACE::munmap(addr, 2 * PAGE);
      continue;
    }

    // Verify sentinel survived the move.
    volatile int *q = reinterpret_cast<volatile int *>(grown);
    if (*q != static_cast<int>(0xCAFEBABE)) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
    }

    // Shrink back.
    void *shrunk = LIBC_NAMESPACE::mremap(grown, 4 * PAGE, PAGE,
                                          MREMAP_MAYMOVE);
    if (shrunk == MAP_FAILED) {
      LIBC_NAMESPACE::munmap(grown, 4 * PAGE);
      continue;
    }

    LIBC_NAMESPACE::munmap(shrunk, PAGE);
    ctx->success.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST_F(LlvmLibcMmapStressTest, ConcurrentMmapMremapMunmap) {
  LifecycleCtx ctx;
  constexpr int N = 6;
  HANDLE threads[N];

  for (int i = 0; i < N; i++)
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(lifecycle_worker, &ctx);

  for (int i = 0; i < N; i++) {
    DWORD r =
        LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 60000);
    EXPECT_EQ(r,
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(threads[i]);
  }

  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);
  // At least some should succeed (mremap may legitimately fail under pressure).
  EXPECT_GT(ctx.success.load(MemoryOrder::RELAXED), 0);
}

// ---------------------------------------------------------------------------
// 11. Churn resilience: map/unmap thousands of distinct addresses.
//     After heavy churn, verify that new allocations still succeed without
//     degradation. The radix tree handles this trivially (freed slots revert
//     to KEY_FREE with no residual cost), but this validates end-to-end.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapStressTest, ChurnResilience) {
  constexpr size_t PAGE = 4096;
  constexpr int ROUNDS = 4;
  constexpr int ALLOCS_PER_ROUND = 500;

  for (int round = 0; round < ROUNDS; round++) {
    // Allocate many distinct mappings (each gets a unique address).
    void *addrs[ALLOCS_PER_ROUND];
    for (int i = 0; i < ALLOCS_PER_ROUND; i++) {
      addrs[i] = LIBC_NAMESPACE::mmap(nullptr, PAGE, PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
      ASSERT_NE(addrs[i], MAP_FAILED);
      // Write to commit the page.
      *reinterpret_cast<volatile int *>(addrs[i]) = i;
    }

    // Free all — clears mapping table entries and releases placeholder slots.
    for (int i = 0; i < ALLOCS_PER_ROUND; i++) {
      EXPECT_THAT(LIBC_NAMESPACE::munmap(addrs[i], PAGE), Succeeds());
    }
  }

  // After 2000 alloc+free cycles on distinct addresses, verify new allocs
  // still work.
  constexpr int VERIFY = 100;
  void *verify_addrs[VERIFY];
  for (int i = 0; i < VERIFY; i++) {
    verify_addrs[i] = LIBC_NAMESPACE::mmap(nullptr, PAGE,
                                           PROT_READ | PROT_WRITE,
                                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    ASSERT_NE(verify_addrs[i], MAP_FAILED);
    *reinterpret_cast<volatile int *>(verify_addrs[i]) = 0xFEED;
    EXPECT_EQ(*reinterpret_cast<volatile int *>(verify_addrs[i]), 0xFEED);
  }
  for (int i = 0; i < VERIFY; i++)
    EXPECT_THAT(LIBC_NAMESPACE::munmap(verify_addrs[i], PAGE), Succeeds());
}
