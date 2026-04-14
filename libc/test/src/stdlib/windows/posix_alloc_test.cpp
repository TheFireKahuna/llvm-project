//===-- Correctness tests for posix_alloc (C/POSIX compliance) ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tests POSIX and C standard requirements for the allocator:
//
//   C17 7.22.3: malloc, calloc, realloc, free
//   C23 7.24.3.7: realloc(ptr, 0) frees and returns NULL
//   POSIX.1-2024: aligned_alloc, malloc_usable_size
//
// Each test cites the specific clause it validates.
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/windows/posix_alloc.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/string/memory_utils/inline_memset.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include <stdint.h>

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;

// ---------------------------------------------------------------------------
// C17 7.22.3.4: malloc
// ---------------------------------------------------------------------------

// "If the size of the space requested is zero, the behavior is
//  implementation-defined: either a null pointer is returned..."
// Our implementation returns a unique freeable pointer for malloc(0).
TEST(LlvmLibcPosixAllocTest, MallocZero) {
  void *p = LIBC_NAMESPACE::posix_alloc(0);
  EXPECT_NE(p, static_cast<void *>(nullptr));
  LIBC_NAMESPACE::posix_free(p);
}

// "The pointer returned if the allocation succeeds is suitably aligned so
//  that it may be assigned to a pointer to any type of object with a
//  fundamental alignment requirement."
TEST(LlvmLibcPosixAllocTest, MallocAlignment) {
  // x86_64/AArch64: fundamental alignment = 16 bytes.
  static constexpr size_t SIZES[] = {1, 7, 16, 24, 48, 64, 100, 256, 1024, 8192, 32768};
  for (size_t sz : SIZES) {
    void *p = LIBC_NAMESPACE::posix_alloc(sz);
    ASSERT_NE(p, static_cast<void *>(nullptr));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, static_cast<uintptr_t>(0));
    LIBC_NAMESPACE::posix_free(p);
  }
}

// Returned memory is writable and readable.
TEST(LlvmLibcPosixAllocTest, MallocWriteRead) {
  auto *p = static_cast<unsigned char *>(LIBC_NAMESPACE::posix_alloc(128));
  ASSERT_NE(p, static_cast<unsigned char *>(nullptr));
  for (int i = 0; i < 128; i++)
    p[i] = static_cast<unsigned char>(i);
  for (int i = 0; i < 128; i++)
    EXPECT_EQ(p[i], static_cast<unsigned char>(i));
  LIBC_NAMESPACE::posix_free(p);
}

// ---------------------------------------------------------------------------
// C17 7.22.3.2: calloc (posix_alloc_zeroed)
// ---------------------------------------------------------------------------

// "The space is initialized to all bits zero."
TEST(LlvmLibcPosixAllocTest, CallocZeroed) {
  static constexpr size_t SIZES[] = {16, 64, 256, 1024, 4096, 32768};
  for (size_t sz : SIZES) {
    auto *p = static_cast<unsigned char *>(
        LIBC_NAMESPACE::posix_alloc_zeroed(sz));
    ASSERT_NE(p, static_cast<unsigned char *>(nullptr));
    for (size_t i = 0; i < sz; i++)
      EXPECT_EQ(p[i], static_cast<unsigned char>(0));
    LIBC_NAMESPACE::posix_free(p);
  }
}

// Zero-fill must hold after alloc-free-alloc cycles (catches stale data).
TEST(LlvmLibcPosixAllocTest, CallocZeroAfterReuse) {
  void *p1 = LIBC_NAMESPACE::posix_alloc(64);
  ASSERT_NE(p1, static_cast<void *>(nullptr));
  LIBC_NAMESPACE::inline_memset(p1, 0xFF, 64);
  LIBC_NAMESPACE::posix_free(p1);

  auto *p2 = static_cast<unsigned char *>(
      LIBC_NAMESPACE::posix_alloc_zeroed(64));
  ASSERT_NE(p2, static_cast<unsigned char *>(nullptr));
  for (int i = 0; i < 64; i++)
    EXPECT_EQ(p2[i], static_cast<unsigned char>(0));
  LIBC_NAMESPACE::posix_free(p2);
}

// ---------------------------------------------------------------------------
// C17 7.22.3.5: realloc
// ---------------------------------------------------------------------------

// "If ptr is a null pointer, the realloc function behaves like the malloc
//  function for the specified size."
TEST(LlvmLibcPosixAllocTest, ReallocNull) {
  void *p = LIBC_NAMESPACE::posix_realloc(nullptr, 100);
  ASSERT_NE(p, static_cast<void *>(nullptr));
  LIBC_NAMESPACE::posix_free(p);
}

// C23 7.24.3.7: "If size is zero [...] it is a free."
TEST(LlvmLibcPosixAllocTest, ReallocZeroSize) {
  void *p = LIBC_NAMESPACE::posix_alloc(64);
  ASSERT_NE(p, static_cast<void *>(nullptr));
  void *r = LIBC_NAMESPACE::posix_realloc(p, 0);
  EXPECT_EQ(r, static_cast<void *>(nullptr));
}

// "The contents of the object shall be unchanged up to the lesser of the
//  new and old sizes."
TEST(LlvmLibcPosixAllocTest, ReallocPreservesData) {
  auto *p = static_cast<unsigned char *>(LIBC_NAMESPACE::posix_alloc(64));
  ASSERT_NE(p, static_cast<unsigned char *>(nullptr));
  for (int i = 0; i < 64; i++)
    p[i] = static_cast<unsigned char>(i);

  // Grow.
  auto *p2 = static_cast<unsigned char *>(
      LIBC_NAMESPACE::posix_realloc(p, 256));
  ASSERT_NE(p2, static_cast<unsigned char *>(nullptr));
  for (int i = 0; i < 64; i++)
    EXPECT_EQ(p2[i], static_cast<unsigned char>(i));

  // Shrink.
  auto *p3 = static_cast<unsigned char *>(
      LIBC_NAMESPACE::posix_realloc(p2, 32));
  ASSERT_NE(p3, static_cast<unsigned char *>(nullptr));
  for (int i = 0; i < 32; i++)
    EXPECT_EQ(p3[i], static_cast<unsigned char>(i));

  LIBC_NAMESPACE::posix_free(p3);
}

// Realloc to same size class returns the same pointer (no-op optimization).
TEST(LlvmLibcPosixAllocTest, ReallocSameClass) {
  void *p = LIBC_NAMESPACE::posix_alloc(16);
  ASSERT_NE(p, static_cast<void *>(nullptr));
  void *p2 = LIBC_NAMESPACE::posix_realloc(p, 15);
  EXPECT_EQ(p, p2);
  LIBC_NAMESPACE::posix_free(p2);
}

// Realloc across slab-to-large boundary.
TEST(LlvmLibcPosixAllocTest, ReallocSlabToLarge) {
  auto *p = static_cast<unsigned char *>(LIBC_NAMESPACE::posix_alloc(100));
  ASSERT_NE(p, static_cast<unsigned char *>(nullptr));
  for (int i = 0; i < 100; i++)
    p[i] = static_cast<unsigned char>(i);

  auto *p2 = static_cast<unsigned char *>(
      LIBC_NAMESPACE::posix_realloc(p, 64 * 1024));
  ASSERT_NE(p2, static_cast<unsigned char *>(nullptr));
  for (int i = 0; i < 100; i++)
    EXPECT_EQ(p2[i], static_cast<unsigned char>(i));

  LIBC_NAMESPACE::posix_free(p2);
}

// ---------------------------------------------------------------------------
// C17 7.22.3.3: free
// ---------------------------------------------------------------------------

// "If ptr is a null pointer, no action occurs."
TEST(LlvmLibcPosixAllocTest, FreeNull) {
  LIBC_NAMESPACE::posix_free(nullptr);
}

// ---------------------------------------------------------------------------
// POSIX aligned_alloc / posix_memalign alignment
// ---------------------------------------------------------------------------

TEST(LlvmLibcPosixAllocTest, AlignedAlloc) {
  static constexpr size_t ALIGNS[] = {32, 64, 128, 256, 512, 1024, 4096};
  for (size_t align : ALIGNS) {
    void *p = LIBC_NAMESPACE::posix_alloc_aligned(64, align);
    ASSERT_NE(p, static_cast<void *>(nullptr));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % align,
              static_cast<uintptr_t>(0));
    LIBC_NAMESPACE::posix_free(p);
  }
}

// Alignment <= MALLOC_ALIGN falls back to regular malloc.
TEST(LlvmLibcPosixAllocTest, AlignedAllocTrivial) {
  void *p = LIBC_NAMESPACE::posix_alloc_aligned(32, 16);
  ASSERT_NE(p, static_cast<void *>(nullptr));
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, static_cast<uintptr_t>(0));
  LIBC_NAMESPACE::posix_free(p);
}

// Large aligned allocation.
TEST(LlvmLibcPosixAllocTest, AlignedAllocLarge) {
  void *p = LIBC_NAMESPACE::posix_alloc_aligned(128 * 1024, 65536);
  ASSERT_NE(p, static_cast<void *>(nullptr));
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 65536,
            static_cast<uintptr_t>(0));
  LIBC_NAMESPACE::posix_free(p);
}

// ---------------------------------------------------------------------------
// POSIX malloc_usable_size
// ---------------------------------------------------------------------------

// "returns the usable size of the allocation [...] at least as large as size."
TEST(LlvmLibcPosixAllocTest, UsableSize) {
  static constexpr size_t SIZES[] = {1, 16, 24, 100, 256, 1024, 8192, 32768};
  for (size_t sz : SIZES) {
    void *p = LIBC_NAMESPACE::posix_alloc(sz);
    ASSERT_NE(p, static_cast<void *>(nullptr));
    size_t usable = LIBC_NAMESPACE::posix_usable_size(p);
    EXPECT_GE(usable, sz);
    LIBC_NAMESPACE::posix_free(p);
  }
}

TEST(LlvmLibcPosixAllocTest, UsableSizeNull) {
  EXPECT_EQ(LIBC_NAMESPACE::posix_usable_size(nullptr),
            static_cast<size_t>(0));
}

// Large allocation usable size.
TEST(LlvmLibcPosixAllocTest, UsableSizeLarge) {
  size_t sz = 2 * 1024 * 1024;
  void *p = LIBC_NAMESPACE::posix_alloc(sz);
  ASSERT_NE(p, static_cast<void *>(nullptr));
  EXPECT_GE(LIBC_NAMESPACE::posix_usable_size(p), sz);
  LIBC_NAMESPACE::posix_free(p);
}

// ---------------------------------------------------------------------------
// Size class coverage — allocate across all 23 slab size classes.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPosixAllocTest, AllSizeClasses) {
  static constexpr uint16_t CLASSES[] = {
      16,    24,    32,    48,    64,    96,    128,   192,
      256,   384,   512,   768,   1024,  1536,  2048,  3072,
      4096,  6144,  8192,  12288, 16384, 24576, 32768,
  };
  void *ptrs[23];
  for (int i = 0; i < 23; i++) {
    ptrs[i] = LIBC_NAMESPACE::posix_alloc(CLASSES[i]);
    ASSERT_NE(ptrs[i], static_cast<void *>(nullptr));
    LIBC_NAMESPACE::inline_memset(ptrs[i], 0xAB, CLASSES[i]);
  }
  for (int i = 0; i < 23; i++)
    LIBC_NAMESPACE::posix_free(ptrs[i]);
}

// ---------------------------------------------------------------------------
// Large allocation path (> LARGE_THRESHOLD = 32768).
// ---------------------------------------------------------------------------

TEST(LlvmLibcPosixAllocTest, LargeAlloc) {
  static constexpr size_t SIZES[] = {33000, 65536, 1024 * 1024, 4 * 1024 * 1024};
  for (size_t sz : SIZES) {
    void *p = LIBC_NAMESPACE::posix_alloc(sz);
    ASSERT_NE(p, static_cast<void *>(nullptr));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, static_cast<uintptr_t>(0));
    static_cast<char *>(p)[0] = 'A';
    static_cast<char *>(p)[sz - 1] = 'Z';
    LIBC_NAMESPACE::posix_free(p);
  }
}

// ---------------------------------------------------------------------------
// Uniqueness — concurrent allocations must return distinct addresses.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPosixAllocTest, AllocUniqueness) {
  constexpr int N = 256;
  void *ptrs[N];
  for (int i = 0; i < N; i++) {
    ptrs[i] = LIBC_NAMESPACE::posix_alloc(32);
    ASSERT_NE(ptrs[i], static_cast<void *>(nullptr));
  }
  for (int i = 0; i < N; i++) {
    for (int j = i + 1; j < N; j++)
      EXPECT_NE(ptrs[i], ptrs[j]);
  }
  for (int i = 0; i < N; i++)
    LIBC_NAMESPACE::posix_free(ptrs[i]);
}

// ---------------------------------------------------------------------------
// Alloc-free-alloc cycles — exercises page recycling and recycle ring.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPosixAllocTest, AllocFreeCycles) {
  for (int round = 0; round < 100; round++) {
    void *p = LIBC_NAMESPACE::posix_alloc(128);
    ASSERT_NE(p, static_cast<void *>(nullptr));
    LIBC_NAMESPACE::inline_memset(p, 0xCC, 128);
    LIBC_NAMESPACE::posix_free(p);
  }
}

// ---------------------------------------------------------------------------
// Cross-thread free — allocate on one thread, free on another.
// ---------------------------------------------------------------------------

struct XThreadCtx {
  void *ptr;
  Atomic<int> done{0};
};

static DWORD __stdcall xthread_free_worker(void *arg) {
  auto *ctx = static_cast<XThreadCtx *>(arg);
  LIBC_NAMESPACE::posix_free(ctx->ptr);
  ctx->done.store(1, MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcPosixAllocTest, CrossThreadFree) {
  for (int i = 0; i < 50; i++) {
    XThreadCtx ctx;
    ctx.ptr = LIBC_NAMESPACE::posix_alloc(64);
    ASSERT_NE(ctx.ptr, static_cast<void *>(nullptr));
    LIBC_NAMESPACE::inline_memset(ctx.ptr, 0xDD, 64);

    HANDLE t = LIBC_NAMESPACE::test_support::create_thread(
        xthread_free_worker, &ctx);
    LIBC_NAMESPACE::test_support::wait_for_single_object(t, 5000);
    ::NtClose(t);
    EXPECT_EQ(ctx.done.load(MemoryOrder::ACQUIRE), 1);
  }
}

// ---------------------------------------------------------------------------
// Concurrent alloc/free from multiple threads — stress segment scan path.
// ---------------------------------------------------------------------------

struct StressCtx {
  Atomic<int> errors{0};
  Atomic<int> ops{0};
};

static DWORD __stdcall stress_worker(void *arg) {
  auto *ctx = static_cast<StressCtx *>(arg);
  constexpr int ITERS = 500;
  constexpr int BATCH = 16;
  void *ptrs[BATCH];

  for (int i = 0; i < ITERS; i++) {
    size_t sz = (i % 5 == 0) ? 16 : (i % 5 == 1) ? 64
                                 : (i % 5 == 2)   ? 256
                                 : (i % 5 == 3)   ? 1024
                                                   : 8192;
    for (int j = 0; j < BATCH; j++) {
      ptrs[j] = LIBC_NAMESPACE::posix_alloc(sz);
      if (!ptrs[j]) {
        ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
        continue;
      }
      static_cast<volatile char *>(ptrs[j])[0] = 'X';
    }
    for (int j = 0; j < BATCH; j++) {
      if (ptrs[j])
        LIBC_NAMESPACE::posix_free(ptrs[j]);
    }
    ctx->ops.fetch_add(BATCH, MemoryOrder::RELAXED);
  }
  return 0;
}

TEST(LlvmLibcPosixAllocTest, ConcurrentAllocFree) {
  StressCtx ctx;
  constexpr int THREADS = 8;
  HANDLE threads[THREADS];

  for (int i = 0; i < THREADS; i++)
    threads[i] =
        LIBC_NAMESPACE::test_support::create_thread(stress_worker, &ctx);

  for (int i = 0; i < THREADS; i++) {
    DWORD r =
        LIBC_NAMESPACE::test_support::wait_for_single_object(threads[i], 30000);
    EXPECT_EQ(r,
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(threads[i]);
  }

  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);
}

// ---------------------------------------------------------------------------
// Segment scan exhaustion — forces find_page_for_class to scan segments.
// Fills pages in one size class until the partial page and recycle ring
// are both empty, verifying the bitmap-accelerated scan finds empty pages.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPosixAllocTest, SegmentScanExhaustion) {
  // 64KB page / 16-byte slots = 4096 slots/page.
  // 31 pages/segment x 4096 = 126,976 slots fills one segment.
  // Allocate enough to span multiple segments.
  constexpr int N = 200000;
  void **ptrs = static_cast<void **>(
      LIBC_NAMESPACE::posix_alloc(N * sizeof(void *)));
  ASSERT_NE(ptrs, static_cast<void **>(nullptr));

  for (int i = 0; i < N; i++) {
    ptrs[i] = LIBC_NAMESPACE::posix_alloc(16);
    ASSERT_NE(ptrs[i], static_cast<void *>(nullptr));
  }

  // Free all — pages become recyclable.
  for (int i = 0; i < N; i++)
    LIBC_NAMESPACE::posix_free(ptrs[i]);

  // Re-allocate — must find recycled pages via bitmap scan.
  for (int i = 0; i < N; i++) {
    ptrs[i] = LIBC_NAMESPACE::posix_alloc(16);
    ASSERT_NE(ptrs[i], static_cast<void *>(nullptr));
  }
  for (int i = 0; i < N; i++)
    LIBC_NAMESPACE::posix_free(ptrs[i]);

  LIBC_NAMESPACE::posix_free(ptrs);
}
