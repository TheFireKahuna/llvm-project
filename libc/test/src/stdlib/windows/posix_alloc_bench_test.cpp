//===-- Benchmark tests for posix_alloc -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Microbenchmarks for the slab allocator's critical paths:
//
//   1. Thread-cache fast path (malloc+free, same thread)
//   2. Batch alloc/free (throughput without immediate reuse)
//   3. Slow path with segment scan (exhaust partial + ring, force bitmap scan)
//   4. Cross-thread free throughput
//   5. Mixed size-class contention (multiple threads, varied sizes)
//   6. Large allocation path (direct mmap)
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/windows/posix_alloc.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include <stdint.h>

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;

// ---------------------------------------------------------------------------
// Timing helpers
// ---------------------------------------------------------------------------

static int64_t now_ns() {
  LARGE_INTEGER freq, ctr;
  ::RtlQueryPerformanceFrequency(&freq);
  ::RtlQueryPerformanceCounter(&ctr);
  return ctr.QuadPart * 1000000000LL / freq.QuadPart;
}

// ---------------------------------------------------------------------------
// 1. Single-thread fast path: malloc+free of same size (thread cache hit)
// ---------------------------------------------------------------------------

TEST(LlvmLibcPosixAllocBench, FastPathMallocFree) {
  LIBC_NAMESPACE::posix_alloc_init();

  // Warmup: prime the thread cache.
  for (int i = 0; i < 1000; i++) {
    void *p = LIBC_NAMESPACE::posix_alloc(32);
    LIBC_NAMESPACE::posix_free(p);
  }

  constexpr int N = 1000000;
  int64_t t0 = now_ns();
  for (int i = 0; i < N; i++) {
    void *p = LIBC_NAMESPACE::posix_alloc(32);
    LIBC_NAMESPACE::posix_free(p);
  }
  int64_t elapsed = now_ns() - t0;

  int64_t ns_per_op = elapsed / N;
  // Expect < 500ns per malloc+free pair on modern hardware with thread cache.
  EXPECT_LT(ns_per_op, static_cast<int64_t>(500));
}

// ---------------------------------------------------------------------------
// 2. Batch alloc then batch free — throughput without immediate reuse.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPosixAllocBench, BatchAllocFree) {
  constexpr int BATCH = 4096;
  constexpr int ROUNDS = 100;
  void *ptrs[BATCH];

  // Warmup.
  for (int i = 0; i < BATCH; i++)
    ptrs[i] = LIBC_NAMESPACE::posix_alloc(64);
  for (int i = 0; i < BATCH; i++)
    LIBC_NAMESPACE::posix_free(ptrs[i]);

  int64_t t0 = now_ns();
  for (int r = 0; r < ROUNDS; r++) {
    for (int i = 0; i < BATCH; i++)
      ptrs[i] = LIBC_NAMESPACE::posix_alloc(64);
    for (int i = 0; i < BATCH; i++)
      LIBC_NAMESPACE::posix_free(ptrs[i]);
  }
  int64_t elapsed = now_ns() - t0;

  int64_t total_ops = static_cast<int64_t>(BATCH) * ROUNDS * 2;
  int64_t ns_per_op = elapsed / total_ops;
  EXPECT_LT(ns_per_op, static_cast<int64_t>(500));
}

// ---------------------------------------------------------------------------
// 3. Slow path — force segment scan by exhausting partial page + ring.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPosixAllocBench, SlowPathSegmentScan) {
  // 64KB page / 16-byte slots = 4096 slots/page.
  // 31 pages/segment = 126,976 slots/segment.
  constexpr int N = 300000;
  void **ptrs = static_cast<void **>(
      LIBC_NAMESPACE::posix_alloc(N * sizeof(void *)));
  ASSERT_NE(ptrs, static_cast<void **>(nullptr));

  // Phase 1: fill segments.
  for (int i = 0; i < N; i++) {
    ptrs[i] = LIBC_NAMESPACE::posix_alloc(16);
    ASSERT_NE(ptrs[i], static_cast<void *>(nullptr));
  }

  // Phase 2: free all — pages become recyclable.
  for (int i = 0; i < N; i++)
    LIBC_NAMESPACE::posix_free(ptrs[i]);

  // Phase 3: re-allocate — ring pops first, then bitmap scan.
  int64_t t0 = now_ns();
  for (int i = 0; i < N; i++)
    ptrs[i] = LIBC_NAMESPACE::posix_alloc(16);
  int64_t elapsed = now_ns() - t0;

  for (int i = 0; i < N; i++)
    LIBC_NAMESPACE::posix_free(ptrs[i]);
  LIBC_NAMESPACE::posix_free(ptrs);

  int64_t ns_per_op = elapsed / N;
  // Bitmap scan should keep this fast even with many segments.
  EXPECT_LT(ns_per_op, static_cast<int64_t>(2000));
}

// ---------------------------------------------------------------------------
// 4. Cross-thread free throughput
// ---------------------------------------------------------------------------

struct XFreeBenchCtx {
  void **ptrs;
  int count;
  Atomic<int> ready{0};
  Atomic<int> done{0};
};

static DWORD __stdcall xfree_bench_worker(void *arg) {
  auto *ctx = static_cast<XFreeBenchCtx *>(arg);
  while (ctx->ready.load(MemoryOrder::ACQUIRE) == 0)
    ;
  for (int i = 0; i < ctx->count; i++)
    LIBC_NAMESPACE::posix_free(ctx->ptrs[i]);
  ctx->done.store(1, MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcPosixAllocBench, CrossThreadFree) {
  constexpr int N = 100000;
  void **ptrs = static_cast<void **>(
      LIBC_NAMESPACE::posix_alloc(N * sizeof(void *)));
  ASSERT_NE(ptrs, static_cast<void **>(nullptr));

  for (int i = 0; i < N; i++) {
    ptrs[i] = LIBC_NAMESPACE::posix_alloc(48);
    ASSERT_NE(ptrs[i], static_cast<void *>(nullptr));
  }

  XFreeBenchCtx ctx;
  ctx.ptrs = ptrs;
  ctx.count = N;

  HANDLE t =
      LIBC_NAMESPACE::test_support::create_thread(xfree_bench_worker, &ctx);

  int64_t t0 = now_ns();
  ctx.ready.store(1, MemoryOrder::RELEASE);
  while (ctx.done.load(MemoryOrder::ACQUIRE) == 0)
    ;
  int64_t elapsed = now_ns() - t0;

  LIBC_NAMESPACE::test_support::wait_for_single_object(t, 5000);
  ::NtClose(t);

  LIBC_NAMESPACE::posix_free(ptrs);

  int64_t ns_per_op = elapsed / N;
  EXPECT_LT(ns_per_op, static_cast<int64_t>(2000));
}

// ---------------------------------------------------------------------------
// 5. Mixed size-class contention — 8 threads, varied sizes.
// ---------------------------------------------------------------------------

struct MixedCtx {
  Atomic<int64_t> total_ops{0};
  Atomic<int> errors{0};
  Atomic<int> go{0};
};

static DWORD __stdcall mixed_worker(void *arg) {
  auto *ctx = static_cast<MixedCtx *>(arg);
  while (ctx->go.load(MemoryOrder::ACQUIRE) == 0)
    ;

  constexpr int ITERS = 50000;
  constexpr int BATCH = 8;
  void *ptrs[BATCH];
  static constexpr size_t SIZES[] = {16, 48, 128, 512, 2048, 8192, 32768};

  for (int i = 0; i < ITERS; i++) {
    size_t sz = SIZES[i % 7];
    for (int j = 0; j < BATCH; j++) {
      ptrs[j] = LIBC_NAMESPACE::posix_alloc(sz);
      if (!ptrs[j])
        ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
    }
    for (int j = 0; j < BATCH; j++) {
      if (ptrs[j])
        LIBC_NAMESPACE::posix_free(ptrs[j]);
    }
  }
  ctx->total_ops.fetch_add(static_cast<int64_t>(ITERS) * BATCH * 2,
                           MemoryOrder::RELAXED);
  return 0;
}

TEST(LlvmLibcPosixAllocBench, MixedSizeContention) {
  MixedCtx ctx;
  constexpr int THREADS = 8;
  HANDLE threads[THREADS];

  for (int i = 0; i < THREADS; i++)
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(mixed_worker, &ctx);

  int64_t t0 = now_ns();
  ctx.go.store(1, MemoryOrder::RELEASE);

  for (int i = 0; i < THREADS; i++) {
    LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 60000);
    ::NtClose(threads[i]);
  }
  int64_t elapsed = now_ns() - t0;

  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);

  int64_t ops = ctx.total_ops.load(MemoryOrder::RELAXED);
  int64_t ns_per_op = elapsed / ops;
  EXPECT_LT(ns_per_op, static_cast<int64_t>(5000));
}

// ---------------------------------------------------------------------------
// 6. Large allocation path (direct mmap).
// ---------------------------------------------------------------------------

TEST(LlvmLibcPosixAllocBench, LargeAllocPath) {
  constexpr int N = 1000;
  constexpr size_t SZ = 128 * 1024; // 128KB

  // Warmup.
  for (int i = 0; i < 10; i++) {
    void *p = LIBC_NAMESPACE::posix_alloc(SZ);
    LIBC_NAMESPACE::posix_free(p);
  }

  int64_t t0 = now_ns();
  for (int i = 0; i < N; i++) {
    void *p = LIBC_NAMESPACE::posix_alloc(SZ);
    LIBC_NAMESPACE::posix_free(p);
  }
  int64_t elapsed = now_ns() - t0;

  int64_t ns_per_op = elapsed / N;
  // Large allocs go through mmap — expect < 100us each.
  EXPECT_LT(ns_per_op, static_cast<int64_t>(100000));
}
