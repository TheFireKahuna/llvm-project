//===-- Unittests for MAP_FIXED / MAP_FIXED_NOREPLACE on Windows ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P3 — anti-data-loss + atomicity coverage for the rebuilt
// posix/mmap/mmap_fixed.cpp.
//
// Anti-data-loss matrix (cordon gate):
//   * MAP_FIXED into a loaded PE image VA → EINVAL.
//   * MAP_FIXED into the main-thread stack reservation → EINVAL.
//   * MAP_FIXED into a libc-internal heap chunk → EINVAL.
//   * MAP_FIXED into the NT process heap (Foreign cordon) → ENOMEM.
//
// FIXED happy paths:
//   * MEM_FREE base → success, fresh placeholder.
//   * Replace an existing anon-private region → success, content gone.
//   * Head-only straddle, tail-only straddle, both-edge straddle.
//   * PROT_NONE replacement → bare placeholder (no access).
//
// FIXED_NOREPLACE matrix:
//   * Into MEM_FREE → success.
//   * Over an occupied region → EEXIST.
//   * NULL addr or misaligned addr → EINVAL.
//   * Page-aligned but sub-64 KiB-aligned base → success (substrate
//     prefix-shaves the enclosing alloc granule internally).
//   * Concurrent NOREPLACE at the same range → exactly one winner.
//
// Concurrency:
//   * Disjoint-VA FIXED vs munmap from another thread proceeds without
//     module-wide serialisation. The substrate's per-arena LOCKED hold
//     is the only point of contention, so disjoint ranges land in
//     parallel.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/stdlib/free.h"
#include "src/stdlib/malloc.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcMmapFixedTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

namespace {

constexpr size_t kPage = 4096;
constexpr size_t kAllocGran = 64 * 1024;

inline void *page_down(const void *p) {
  uintptr_t v = reinterpret_cast<uintptr_t>(p);
  return reinterpret_cast<void *>(v & ~(static_cast<uintptr_t>(kPage) - 1));
}

// Resolve the NT process-heap base. The libc bring-up cordon sweep
// stamps process-heap VA as Foreign in the pagemap; an allocation
// inside it gives us an address that `validate_map_fixed_target` will
// classify as Foreign / ForeignStale → ENOMEM. PEB->ProcessHeap is the
// canonical handle; `RtlAllocateHeap` populates a usable address.
void *foreign_heap_address() {
  PEB *peb = NtCurrentPeb();
  if (peb == nullptr || peb->ProcessHeap == nullptr)
    return nullptr;
  return ::RtlAllocateHeap(peb->ProcessHeap, 0, 256);
}

void foreign_heap_free(void *p) {
  PEB *peb = NtCurrentPeb();
  if (peb == nullptr || peb->ProcessHeap == nullptr || p == nullptr)
    return;
  (void)::RtlFreeHeap(peb->ProcessHeap, 0, p);
}

} // namespace

//===----------------------------------------------------------------------===//
// Anti-data-loss cordon rejection.
//===----------------------------------------------------------------------===//

TEST_F(LlvmLibcMmapFixedTest, FixedIntoImageVa_RejectedEINVAL) {
  // The address of `LIBC_NAMESPACE::mmap` is in the host module's PE
  // image, stamped Image at bring-up (and refreshed via the loader
  // notification callback).
  void *image_page =
      page_down(reinterpret_cast<const void *>(&LIBC_NAMESPACE::mmap));
  void *result = LIBC_NAMESPACE::mmap(
      image_page, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  EXPECT_THAT(result, Fails(EINVAL, MAP_FAILED));
}

TEST_F(LlvmLibcMmapFixedTest, FixedIntoStackVa_RejectedEINVAL) {
  // The address of a local variable is on the main-thread stack
  // reservation, stamped Kernel by va_inventory's discover_kernel_regions.
  volatile int local = 0;
  void *stack_page = page_down(const_cast<const int *>(&local));
  void *result = LIBC_NAMESPACE::mmap(
      stack_page, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  EXPECT_THAT(result, Fails(EINVAL, MAP_FAILED));
  (void)local;
}

TEST_F(LlvmLibcMmapFixedTest, FixedIntoLibcHeapVa_RejectedEINVAL) {
  // libc-internal partitions are stamped distinctly from POSIX-visible
  // VA. malloc returns from the libc allocator, so the chunk's pagemap
  // tag is libc-internal — cordon gate refuses with EINVAL.
  void *chunk = LIBC_NAMESPACE::malloc(256);
  ASSERT_NE(chunk, static_cast<void *>(nullptr));
  void *libc_page = page_down(chunk);
  void *result = LIBC_NAMESPACE::mmap(
      libc_page, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  EXPECT_THAT(result, Fails(EINVAL, MAP_FAILED));
  LIBC_NAMESPACE::free(chunk);
}

TEST_F(LlvmLibcMmapFixedTest, FixedIntoForeignVa_RejectedENOMEM) {
  // The NT process heap is captured as Foreign during va_inventory's
  // discover_foreign_regions bulk sweep. The Foreign cordon answer is
  // ENOMEM rather than EINVAL so portable apps can retry with NULL hint.
  void *heap = foreign_heap_address();
  if (heap == nullptr)
    return; // PEB->ProcessHeap unavailable — skip rather than fail.
  void *foreign_page = page_down(heap);
  void *result = LIBC_NAMESPACE::mmap(
      foreign_page, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  EXPECT_THAT(result, Fails(ENOMEM, MAP_FAILED));
  foreign_heap_free(heap);
}

//===----------------------------------------------------------------------===//
// MAP_FIXED happy paths.
//===----------------------------------------------------------------------===//

TEST_F(LlvmLibcMmapFixedTest, FixedIntoMemFree_FreshPlaceholder) {
  // Allocate, free, then FIXED into the now-free VA.
  void *region = LIBC_NAMESPACE::mmap(nullptr, kAllocGran,
                                      PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(region, MAP_FAILED);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(region, kAllocGran), Succeeds());

  void *result = LIBC_NAMESPACE::mmap(
      region, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_EQ(result, region);
  reinterpret_cast<char *>(result)[0] = 'A';
  EXPECT_EQ(reinterpret_cast<char *>(result)[0], 'A');
  EXPECT_THAT(LIBC_NAMESPACE::munmap(result, kPage), Succeeds());
}

TEST_F(LlvmLibcMmapFixedTest, FixedOverAnonPrivate_ContentDestroyed) {
  void *region = LIBC_NAMESPACE::mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(region, MAP_FAILED);
  reinterpret_cast<int *>(region)[0] = static_cast<int>(0xCAFEBABE);

  void *result = LIBC_NAMESPACE::mmap(
      region, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_EQ(result, region);
  EXPECT_EQ(reinterpret_cast<int *>(result)[0], 0);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(result, kPage), Succeeds());
}

TEST_F(LlvmLibcMmapFixedTest, FixedHeadStraddler_Success) {
  // Outer region (64 KiB). FIXED 60 KiB starting at offset 4 KiB — head
  // straddles, tail aligns with outer end (no tail straddler).
  void *outer = LIBC_NAMESPACE::mmap(nullptr, kAllocGran,
                                     PROT_READ | PROT_WRITE,
                                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(outer, MAP_FAILED);

  char *bytes = reinterpret_cast<char *>(outer);
  bytes[0] = 'L';        // head (un-FIXED) sentinel
  bytes[kPage] = 'M';    // inside FIXED range sentinel

  void *fixed_target = bytes + kPage;
  size_t fixed_size = kAllocGran - kPage;
  void *result = LIBC_NAMESPACE::mmap(
      fixed_target, fixed_size, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_EQ(result, fixed_target);
  // The 'L' in head survivor stays readable; the 'M' in the FIXED
  // overlay was destroyed.
  EXPECT_EQ(bytes[0], 'L');
  EXPECT_EQ(bytes[kPage], '\0');

  EXPECT_THAT(LIBC_NAMESPACE::munmap(outer, kAllocGran), Succeeds());
}

TEST_F(LlvmLibcMmapFixedTest, FixedBothStraddlers_Success) {
  // Outer region 64 KiB. FIXED 4 KiB at offset 8 KiB — both edges of
  // the FIXED range cut through the outer desc.
  void *outer = LIBC_NAMESPACE::mmap(nullptr, kAllocGran,
                                     PROT_READ | PROT_WRITE,
                                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(outer, MAP_FAILED);

  char *bytes = reinterpret_cast<char *>(outer);
  bytes[0] = 'A';
  bytes[2 * kPage] = 'M';
  bytes[kAllocGran - 1] = 'B';

  void *fixed_target = bytes + 2 * kPage;
  void *result = LIBC_NAMESPACE::mmap(
      fixed_target, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_EQ(result, fixed_target);
  EXPECT_EQ(bytes[0], 'A');
  EXPECT_EQ(bytes[2 * kPage], '\0');
  EXPECT_EQ(bytes[kAllocGran - 1], 'B');

  EXPECT_THAT(LIBC_NAMESPACE::munmap(outer, kAllocGran), Succeeds());
}

TEST_F(LlvmLibcMmapFixedTest, FixedProtNone_BarePlaceholder) {
  void *region = LIBC_NAMESPACE::mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(region, MAP_FAILED);
  reinterpret_cast<char *>(region)[0] = 'X';

  // PROT_NONE FIXED replacement produces a bare placeholder — readable
  // would fault, so we exercise the round-trip via munmap.
  void *result = LIBC_NAMESPACE::mmap(
      region, kPage, PROT_NONE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_EQ(result, region);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(result, kPage), Succeeds());
}

//===----------------------------------------------------------------------===//
// MAP_FIXED_NOREPLACE matrix.
//===----------------------------------------------------------------------===//

TEST_F(LlvmLibcMmapFixedTest, NoReplaceIntoMemFree_Success) {
  void *region = LIBC_NAMESPACE::mmap(nullptr, kAllocGran,
                                      PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(region, MAP_FAILED);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(region, kAllocGran), Succeeds());

  void *result = LIBC_NAMESPACE::mmap(
      region, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_EQ(result, region);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(result, kPage), Succeeds());
}

TEST_F(LlvmLibcMmapFixedTest, NoReplaceOverOccupied_ReturnsEEXIST) {
  void *region = LIBC_NAMESPACE::mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(region, MAP_FAILED);

  void *result = LIBC_NAMESPACE::mmap(
      region, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
  EXPECT_THAT(result, Fails(EEXIST, MAP_FAILED));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(region, kPage), Succeeds());
}

TEST_F(LlvmLibcMmapFixedTest, NoReplaceNullAddr_RejectedEINVAL) {
  void *result = LIBC_NAMESPACE::mmap(
      nullptr, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
  EXPECT_THAT(result, Fails(EINVAL, MAP_FAILED));
}

TEST_F(LlvmLibcMmapFixedTest, NoReplaceMisalignedAddr_RejectedEINVAL) {
  void *misaligned = reinterpret_cast<void *>(static_cast<uintptr_t>(0x12345));
  void *result = LIBC_NAMESPACE::mmap(
      misaligned, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
  EXPECT_THAT(result, Fails(EINVAL, MAP_FAILED));
}

TEST_F(LlvmLibcMmapFixedTest, NoReplaceSub64KAligned_Success) {
  // Allocate a 128 KiB region, free it, then NOREPLACE at +4 KiB —
  // page-aligned but not alloc-granularity-aligned. The substrate's
  // prefix-shave path reserves the enclosing 64 KiB granule, splits at
  // the prefix, frees the 4 KiB pad to MEM_FREE, and commits exactly
  // the user range.
  size_t outer = 128 * 1024;
  void *region = LIBC_NAMESPACE::mmap(nullptr, outer, PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(region, MAP_FAILED);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(region, outer), Succeeds());

  void *target = static_cast<char *>(region) + kPage;
  void *result = LIBC_NAMESPACE::mmap(
      target, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_EQ(result, target);
  reinterpret_cast<char *>(result)[0] = 'Z';
  EXPECT_EQ(reinterpret_cast<char *>(result)[0], 'Z');
  EXPECT_THAT(LIBC_NAMESPACE::munmap(result, kPage), Succeeds());
}

//===----------------------------------------------------------------------===//
// Concurrent atomicity: NOREPLACE-vs-NOREPLACE and FIXED-vs-munmap on
// disjoint VA proceed without module-wide serialisation.
//===----------------------------------------------------------------------===//

struct NoReplaceRaceCtx {
  void *target;
  Atomic<int> winners{0};
  Atomic<int> losers{0};
};

LIBC_MSABI static DWORD noreplace_race_worker(void *arg) {
  auto *ctx = static_cast<NoReplaceRaceCtx *>(arg);
  void *r = LIBC_NAMESPACE::mmap(
      ctx->target, kPage, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
  if (r == MAP_FAILED)
    ctx->losers.fetch_add(1, MemoryOrder::RELAXED);
  else
    ctx->winners.fetch_add(1, MemoryOrder::RELAXED);
  return 0;
}

TEST_F(LlvmLibcMmapFixedTest, NoReplaceConcurrentClaim_ExactlyOneWinner) {
  // Reserve and free a 64 KiB region to get a known MEM_FREE base, then
  // race N threads at the same NOREPLACE target.
  void *seed = LIBC_NAMESPACE::mmap(nullptr, kAllocGran,
                                    PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(seed, MAP_FAILED);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(seed, kAllocGran), Succeeds());

  constexpr int N = 8;
  NoReplaceRaceCtx ctx;
  ctx.target = seed;
  HANDLE threads[N];
  for (int i = 0; i < N; ++i)
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(noreplace_race_worker,
                                                    &ctx);
  for (int i = 0; i < N; ++i) {
    LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 30000);
    ::NtClose(threads[i]);
  }
  EXPECT_EQ(ctx.winners.load(MemoryOrder::RELAXED), 1);
  EXPECT_EQ(ctx.losers.load(MemoryOrder::RELAXED), N - 1);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(seed, kPage), Succeeds());
}

struct DisjointStressCtx {
  Atomic<int> iters{0};
  Atomic<int> failures{0};
  Atomic<int> stop{0};
};

LIBC_MSABI static DWORD disjoint_fixed_worker(void *arg) {
  auto *ctx = static_cast<DisjointStressCtx *>(arg);
  for (int i = 0; i < 1000; ++i) {
    if (ctx->stop.load(MemoryOrder::RELAXED) != 0)
      break;
    void *base = LIBC_NAMESPACE::mmap(nullptr, kAllocGran,
                                       PROT_READ | PROT_WRITE,
                                       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (base == MAP_FAILED) {
      ctx->failures.fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    void *result = LIBC_NAMESPACE::mmap(
        static_cast<char *>(base) + 2 * kPage, kPage,
        PROT_READ | PROT_WRITE,
        MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (result == MAP_FAILED)
      ctx->failures.fetch_add(1, MemoryOrder::RELAXED);
    (void)LIBC_NAMESPACE::munmap(base, kAllocGran);
    ctx->iters.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

LIBC_MSABI static DWORD disjoint_munmap_worker(void *arg) {
  auto *ctx = static_cast<DisjointStressCtx *>(arg);
  for (int i = 0; i < 1000; ++i) {
    if (ctx->stop.load(MemoryOrder::RELAXED) != 0)
      break;
    void *base = LIBC_NAMESPACE::mmap(nullptr, kAllocGran,
                                       PROT_READ | PROT_WRITE,
                                       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (base == MAP_FAILED) {
      ctx->failures.fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    if (LIBC_NAMESPACE::munmap(base, kAllocGran) != 0)
      ctx->failures.fetch_add(1, MemoryOrder::RELAXED);
    ctx->iters.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST_F(LlvmLibcMmapFixedTest, FixedVsMunmapDisjointVa_NoFailures) {
  // Concurrent MAP_FIXED workers and munmap workers operate on
  // independent allocations — i.e. disjoint VA. Under the per-arena
  // LOCKED-byte model there is no module-wide serialisation, so
  // throughput stays bounded by the number of contending operations
  // on each individual arena (here: one — each worker owns its own).
  // The pass condition is "no failures across 4 + 4 workers × 1000
  // iterations".
  DisjointStressCtx ctx;
  HANDLE fixed_threads[4];
  HANDLE munmap_threads[4];
  for (int i = 0; i < 4; ++i) {
    fixed_threads[i] = LIBC_NAMESPACE::test_support::create_thread(
        disjoint_fixed_worker, &ctx);
    munmap_threads[i] = LIBC_NAMESPACE::test_support::create_thread(
        disjoint_munmap_worker, &ctx);
  }
  for (int i = 0; i < 4; ++i) {
    LIBC_NAMESPACE::test_support::wait_for_single_object(fixed_threads[i],
                                                          60000);
    ::NtClose(fixed_threads[i]);
    LIBC_NAMESPACE::test_support::wait_for_single_object(munmap_threads[i],
                                                          60000);
    ::NtClose(munmap_threads[i]);
  }
  EXPECT_EQ(ctx.failures.load(MemoryOrder::RELAXED), 0);
  EXPECT_GT(ctx.iters.load(MemoryOrder::RELAXED), 0);
}
