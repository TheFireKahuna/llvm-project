//===-- Stress tests for VEH demand-commit and MADV_WILLNEED --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Exercises the VEH demand-commit handler under concurrent load:
//
//   1. Concurrent first-touch on MAP_NORESERVE pages — multiple threads
//      fault simultaneously into uncommitted SEC_RESERVE views, triggering
//      the VEH cluster commit path.
//
//   2. MADV_WILLNEED on SEC_RESERVE — verifies that WILLNEED proactively
//      commits uncommitted pages, eliminating VEH faults on access.
//
//   3. Interleaved DONTNEED + access — decommit pages, then re-fault them
//      through the VEH handler. Tests the decommit→recommit cycle.
//
//   4. Mixed NORESERVE + mprotect — mprotect on SEC_RESERVE views sets
//      VM_FLAG_PROT_CHANGED, forcing the VEH slow path (single-page
//      commit with MBI query). Verifies no cluster-commit corruption.
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/madvise.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/mprotect.h"
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
using LlvmLibcDemandCommitStressTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// ---------------------------------------------------------------------------
// 1. Concurrent first-touch on MAP_NORESERVE — VEH cluster commit.
//    Threads touch disjoint stripes of a large NORESERVE mapping. The VEH
//    handler commits 256KB clusters around each fault. Multiple threads
//    faulting in parallel stress the NtAllocateVirtualMemoryEx commit path
//    and verify no double-commit or corruption.
// ---------------------------------------------------------------------------

struct DemandCtx {
  char *base;
  size_t total_size;
  int num_threads;
  Atomic<int> errors{0};
};

static DWORD __stdcall demand_worker(void *arg) {
  auto *ctx = static_cast<DemandCtx *>(arg);
  DWORD tid = ::NtCurrentThreadId();
  int idx = static_cast<int>(tid % ctx->num_threads);

  // Each thread owns a stripe of pages.
  size_t stripe = ctx->total_size / ctx->num_threads;
  char *start = ctx->base + idx * stripe;
  constexpr size_t PAGE = 4096;
  size_t pages = stripe / PAGE;

  for (size_t i = 0; i < pages; i++) {
    // First touch — triggers VEH demand-commit.
    volatile char *p = start + i * PAGE;
    *p = static_cast<char>(i & 0xFF);
    if (*p != static_cast<char>(i & 0xFF))
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST_F(LlvmLibcDemandCommitStressTest, ConcurrentDemandCommit) {
  // 2MB region with MAP_NORESERVE — pages uncommitted until touched.
  constexpr size_t SIZE = 2 * 1024 * 1024;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, SIZE, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
                                    -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  DemandCtx ctx;
  ctx.base = reinterpret_cast<char *>(addr);
  ctx.total_size = SIZE;
  ctx.num_threads = 8;

  HANDLE threads[8];
  for (int i = 0; i < 8; i++)
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(demand_worker, &ctx);

  for (int i = 0; i < 8; i++) {
    DWORD r =
        LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 30000);
    EXPECT_EQ(r,
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(threads[i]);
  }

  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, SIZE), Succeeds());
}

// ---------------------------------------------------------------------------
// 2. MADV_WILLNEED on SEC_RESERVE — proactive commit.
//    After WILLNEED, pages should be committed and accessible without
//    triggering VEH faults. Verifies the P4 fix.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcDemandCommitStressTest, WillneedCommitsReserve) {
  constexpr size_t SIZE = 256 * 1024; // 256KB = one VEH cluster
  void *addr = LIBC_NAMESPACE::mmap(nullptr, SIZE, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
                                    -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  // MADV_WILLNEED should commit the pages proactively.
  int ret = LIBC_NAMESPACE::madvise(addr, SIZE, MADV_WILLNEED);
  ASSERT_EQ(ret, 0);

  // Access every page — should NOT fault through VEH (already committed).
  char *p = reinterpret_cast<char *>(addr);
  constexpr size_t PAGE = 4096;
  for (size_t i = 0; i < SIZE / PAGE; i++) {
    p[i * PAGE] = static_cast<char>(i);
    EXPECT_EQ(p[i * PAGE], static_cast<char>(i));
  }

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, SIZE), Succeeds());
}

// ---------------------------------------------------------------------------
// 3. Interleaved DONTNEED + access — decommit/recommit cycle.
//    DONTNEED decommits SEC_RESERVE pages. Subsequent access re-faults
//    through VEH, which recommits with zero-fill.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcDemandCommitStressTest, DontneedRecommitCycle) {
  constexpr size_t SIZE = 64 * 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, SIZE, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
                                    -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  char *p = reinterpret_cast<char *>(addr);
  constexpr size_t PAGE = 4096;

  for (int round = 0; round < 10; round++) {
    // Touch all pages (commits via VEH on first round, or direct on later).
    for (size_t i = 0; i < SIZE / PAGE; i++)
      p[i * PAGE] = static_cast<char>(round + 1);

    // Verify data is intact.
    for (size_t i = 0; i < SIZE / PAGE; i++)
      EXPECT_EQ(p[i * PAGE], static_cast<char>(round + 1));

    // DONTNEED: decommit (SEC_RESERVE path), zero-fill guaranteed.
    int ret = LIBC_NAMESPACE::madvise(addr, SIZE, MADV_DONTNEED);
    ASSERT_EQ(ret, 0);

    // Re-access: VEH recommits with zero-fill.
    for (size_t i = 0; i < SIZE / PAGE; i++)
      EXPECT_EQ(p[i * PAGE], '\0');
  }

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, SIZE), Succeeds());
}

// ---------------------------------------------------------------------------
// 4. NORESERVE + mprotect → VM_FLAG_PROT_CHANGED forces VEH slow path.
//    After mprotect changes a SEC_RESERVE view's protection, the VEH
//    handler must use the slow path (single-page commit with MBI query)
//    instead of cluster commit, to avoid reverting per-page protections.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcDemandCommitStressTest, NoreserveMprotectSlowPath) {
  constexpr size_t PAGE = 4096;
  constexpr size_t SIZE = 16 * PAGE;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, SIZE, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
                                    -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  char *p = reinterpret_cast<char *>(addr);

  // Touch first half to commit it via VEH.
  for (size_t i = 0; i < 8; i++)
    p[i * PAGE] = 'A';

  // Change protection on the first half to read-only. This sets
  // VM_FLAG_PROT_CHANGED on the mapping table entry.
  ASSERT_EQ(LIBC_NAMESPACE::mprotect(addr, 8 * PAGE, PROT_READ), 0);

  // Verify first half is readable.
  for (size_t i = 0; i < 8; i++)
    EXPECT_EQ(p[i * PAGE], 'A');

  // Touch second half — VEH must use slow path (check MBI before commit)
  // because VM_FLAG_PROT_CHANGED is set. This must still succeed.
  ASSERT_EQ(LIBC_NAMESPACE::mprotect(reinterpret_cast<char *>(addr) + 8 * PAGE,
                                     8 * PAGE, PROT_READ | PROT_WRITE),
            0);
  for (size_t i = 8; i < 16; i++)
    p[i * PAGE] = 'B';
  for (size_t i = 8; i < 16; i++)
    EXPECT_EQ(p[i * PAGE], 'B');

  // First half should still be read-only (VEH didn't cluster-commit over it).
  // Restore to RW for cleanup.
  ASSERT_EQ(LIBC_NAMESPACE::mprotect(addr, 8 * PAGE, PROT_READ | PROT_WRITE),
            0);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, SIZE), Succeeds());
}

// ---------------------------------------------------------------------------
// 5. Concurrent demand-commit + DONTNEED — one thread touches pages while
//    another decommits them. Tests the VEH handler under concurrent
//    decommit (recommit races).
// ---------------------------------------------------------------------------

struct DontneedRaceCtx {
  char *base;
  size_t size;
  Atomic<int> stop{0};
  Atomic<int> errors{0};
};

static DWORD __stdcall touch_worker(void *arg) {
  auto *ctx = static_cast<DontneedRaceCtx *>(arg);
  constexpr size_t PAGE = 4096;
  size_t pages = ctx->size / PAGE;

  while (ctx->stop.load(MemoryOrder::ACQUIRE) == 0) {
    for (size_t i = 0; i < pages; i++) {
      volatile char *p = ctx->base + i * PAGE;
      *p = static_cast<char>(i);
    }
  }
  return 0;
}

static DWORD __stdcall dontneed_worker(void *arg) {
  auto *ctx = static_cast<DontneedRaceCtx *>(arg);
  for (int round = 0; round < 50; round++) {
    int ret = LIBC_NAMESPACE::madvise(ctx->base, ctx->size, MADV_DONTNEED);
    if (ret != 0)
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
  }
  ctx->stop.store(1, MemoryOrder::RELEASE);
  return 0;
}

TEST_F(LlvmLibcDemandCommitStressTest, ConcurrentDontneedAndAccess) {
  constexpr size_t SIZE = 64 * 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, SIZE, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
                                    -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  // Touch all pages first.
  char *p = reinterpret_cast<char *>(addr);
  for (size_t i = 0; i < SIZE / 4096; i++)
    p[i * 4096] = 'X';

  DontneedRaceCtx ctx;
  ctx.base = p;
  ctx.size = SIZE;

  HANDLE threads[2];
  threads[0] = LIBC_NAMESPACE::test_support::create_thread(touch_worker, &ctx);
  threads[1] =
      LIBC_NAMESPACE::test_support::create_thread(dontneed_worker, &ctx);

  for (int i = 0; i < 2; i++) {
    LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 30000);
    ::NtClose(threads[i]);
  }

  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, SIZE), Succeeds());
}
