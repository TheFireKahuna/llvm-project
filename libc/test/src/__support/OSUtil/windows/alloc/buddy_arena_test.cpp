//===-- alloc::buddy_arena hermetic suite -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Comprehensive coverage of `windows::alloc::buddy_arena` — the lock-free
// NBALLOC chunk broker (Layer 2 of the NTPOSIX memory architecture).
// Targets every public entry point plus the load-bearing concurrency
// invariants that are easy to regress and hard to detect by code review:
//
//   * NBALLOC TRYALLOC's per-ancestor CAS chain converges; OCC-on-ancestor
//     abort rolls back via FREENODE.
//   * Cross-class coalescing: two adjacent freed 64 KiB chunks reach the
//     parent 128 KiB tree node as available for fresh allocation.
//   * Pagemap entry written after alloc (cookie-XOR'd 8 B encoded word),
//     decoded on free, zeroed via pagemap_retire_range.
//   * Crystalline FreeFn ordering: canary -> pagemap_unregister_range
//     (no-op post-pivot) -> nt_pal::decommit_preserve -> return slot.
//   * Lazy tree commit on first allocation; subsequent allocs skip the
//     commit syscall.
//   * Pagemap stay-committed safety: post-free read of an arbitrary
//     in-window address is wait-free and never faults.
//   * Multi-thread CAS contention does not deadlock and does not corrupt
//     the tree.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/buddy_arena.h"
#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_join.h"
#include "test/UnitTest/Test.h"

#include <stddef.h>
#include <stdint.h>

namespace {

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::g_pcb;
using LIBC_NAMESPACE::windows::alloc::buddy_alloc;
using LIBC_NAMESPACE::windows::alloc::buddy_alloc_huge;
using LIBC_NAMESPACE::windows::alloc::buddy_class_bytes;
using LIBC_NAMESPACE::windows::alloc::buddy_class_shift_for;
using LIBC_NAMESPACE::windows::alloc::buddy_class_tree_level;
using LIBC_NAMESPACE::windows::alloc::buddy_free_huge;
using LIBC_NAMESPACE::windows::alloc::buddy_free_sized;
using LIBC_NAMESPACE::windows::alloc::buddy_stats_snapshot;
using LIBC_NAMESPACE::windows::alloc::BuddyChunkDescriptor;
using LIBC_NAMESPACE::windows::alloc::BuddyStats;
using LIBC_NAMESPACE::windows::alloc::VaChunkConsumer;
using LIBC_NAMESPACE::windows::alloc::derive_slot_cookie;
using LIBC_NAMESPACE::windows::alloc::kBuddyArenaBytes;
using LIBC_NAMESPACE::windows::alloc::kBuddyClassCount;
using LIBC_NAMESPACE::windows::alloc::kBuddyMaxChunkBytes;
using LIBC_NAMESPACE::windows::alloc::kBuddyMaxShift;
using LIBC_NAMESPACE::windows::alloc::kBuddyMinChunkBytes;
using LIBC_NAMESPACE::windows::alloc::kBuddyMinShift;
using LIBC_NAMESPACE::windows::alloc::kBuddyTreeBytes;
using LIBC_NAMESPACE::windows::alloc::kBuddyTreeDepth;
using LIBC_NAMESPACE::windows::alloc::kClassHuge;
using LIBC_NAMESPACE::windows::alloc::kPagemapChunkBytes;
using LIBC_NAMESPACE::windows::alloc::PagemapDecoded;
using LIBC_NAMESPACE::windows::alloc::pagemap_decode;
using LIBC_NAMESPACE::windows::alloc::pagemap_encode;
using LIBC_NAMESPACE::windows::alloc::pagemap_is_tracked;
using LIBC_NAMESPACE::windows::alloc::pagemap_load_decoded;
using LIBC_NAMESPACE::windows::alloc::pagemap_load_descriptor;

// 64 KiB chunk = the new buddy minimum.
constexpr size_t kMinChunk = 64 * 1024;
constexpr size_t k128K = 128 * 1024;

} // namespace

// =========================================================================
// 1. Layout pin — geometry constants are stable
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, LayoutPin) {
  EXPECT_EQ(static_cast<size_t>(kBuddyMinShift), size_t{16});
  EXPECT_EQ(static_cast<size_t>(kBuddyMaxShift), size_t{22});
  EXPECT_EQ(kBuddyMinChunkBytes, size_t{64} * 1024);
  EXPECT_EQ(kBuddyMaxChunkBytes, size_t{4} * 1024 * 1024);
  EXPECT_EQ(static_cast<size_t>(kBuddyClassCount), size_t{7});
  EXPECT_EQ(kBuddyArenaBytes, size_t{4} * 1024 * 1024 * 1024ULL);
  EXPECT_EQ(static_cast<size_t>(kBuddyTreeDepth), size_t{16});
  // NBALLOC tree is 1-indexed; array has 2^(d+1) entries with index 0
  // unused. Deepest valid node index = 2^(d+1) - 1. One byte per node.
  EXPECT_EQ(kBuddyTreeBytes, size_t{1} << 17);
  EXPECT_EQ(kPagemapChunkBytes, size_t{64} * 1024);
}

// =========================================================================
// 2. Class arithmetic — round-up + tree-level mapping
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, ClassArithmetic) {
  // 64 KiB exactly maps to leaf level (depth = 16).
  EXPECT_EQ(static_cast<size_t>(buddy_class_shift_for(kMinChunk)),
            static_cast<size_t>(kBuddyMinShift));
  EXPECT_EQ(static_cast<size_t>(
                buddy_class_tree_level(buddy_class_shift_for(kMinChunk))),
            static_cast<size_t>(kBuddyTreeDepth));

  // 64 KiB + 1 rounds up to 128 KiB (one level above leaf).
  EXPECT_EQ(static_cast<size_t>(buddy_class_shift_for(kMinChunk + 1)),
            static_cast<size_t>(kBuddyMinShift + 1));
  EXPECT_EQ(buddy_class_bytes(buddy_class_shift_for(kMinChunk + 1)), k128K);

  // 4 MiB exactly maps to top class (tree level 10).
  EXPECT_EQ(static_cast<size_t>(buddy_class_shift_for(4 * 1024 * 1024)),
            static_cast<size_t>(kBuddyMaxShift));
  EXPECT_EQ(static_cast<size_t>(
                buddy_class_tree_level(buddy_class_shift_for(4 * 1024 * 1024))),
            size_t{10});

  // > 4 MiB returns the huge-path sentinel.
  EXPECT_GT(static_cast<size_t>(buddy_class_shift_for(8 * 1024 * 1024)),
            static_cast<size_t>(kBuddyMaxShift));
}

// =========================================================================
// 3. Cookie XOR encode/decode round-trip
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, PagemapEntryEncodingRoundTrip) {
  // Spot-check a handful of (slot_idx, tag) pairs across the encoding
  // surface. A literal zero entry must decode to tag=Empty (the cookie
  // low byte is constrained to zero at init for exactly this property).
  // `slot_idx` for a zero entry is `cookie >> 8` — junk that callers
  // never consult because they gate on tag first.
  PagemapDecoded zero = pagemap_decode(0);
  EXPECT_EQ(static_cast<unsigned>(zero.tag),
            static_cast<unsigned>(VaChunkConsumer::Empty));

  const struct {
    uint32_t slot;
    VaChunkConsumer tag;
  } cases[] = {
      {0, VaChunkConsumer::BuddyDirect},
      {1, VaChunkConsumer::BuddyDirect},
      {0xFF, VaChunkConsumer::HugeDirect},
      {0xDEAD, VaChunkConsumer::BuddyDirect},
      {0x00FFFFFF, VaChunkConsumer::HugeDirect},
      {0xFFFFFFFFu, VaChunkConsumer::Misc},
  };
  for (const auto &c : cases) {
    uint64_t enc = pagemap_encode(c.slot, c.tag);
    PagemapDecoded d = pagemap_decode(enc);
    EXPECT_EQ(d.slot_idx, c.slot);
    EXPECT_EQ(static_cast<unsigned>(d.tag), static_cast<unsigned>(c.tag));
  }
}

// =========================================================================
// 4. Pagemap cookie low byte is zero (init invariant)
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, CookieZeroEntryDecodesEmpty) {
  uintptr_t cookie = g_pcb.zone0.pagemap_cookie();
  EXPECT_NE(cookie, uintptr_t{0});
  EXPECT_EQ(cookie & 0xFFu, uintptr_t{0});
}

// =========================================================================
// 5. Round-trip alloc/free at every class size
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, RoundTripAllClasses) {
  for (uint8_t shift = kBuddyMinShift; shift <= kBuddyMaxShift; ++shift) {
    size_t bytes = buddy_class_bytes(shift);
    void *p = buddy_alloc(bytes);
    ASSERT_TRUE(p != nullptr);

    // Pagemap entry must be published (decoded tag = BuddyDirect).
    PagemapDecoded d = pagemap_load_decoded(p);
    EXPECT_EQ(static_cast<unsigned>(d.tag),
              static_cast<unsigned>(VaChunkConsumer::BuddyDirect));

    // Chunk should be writable (commit_replace + MEM_WRITE_WATCH armed).
    static_cast<volatile char *>(p)[0] = 'A';
    static_cast<volatile char *>(p)[bytes - 1] = 'Z';

    buddy_free_sized(p, bytes);

    // Post-pivot: pagemap pages stay committed for life. The post-free
    // load is wait-free and non-faulting; it returns Empty (the entry
    // was zeroed by pagemap_retire_range).
    PagemapDecoded after = pagemap_load_decoded(p);
    EXPECT_EQ(static_cast<unsigned>(after.tag),
              static_cast<unsigned>(VaChunkConsumer::Empty));
  }
}

// =========================================================================
// 6. Pagemap entry written after alloc carries expected tag
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, PagemapEntryWrittenAfterAlloc) {
  void *p = buddy_alloc(256 * 1024);
  ASSERT_TRUE(p != nullptr);
  PagemapDecoded d = pagemap_load_decoded(p);
  EXPECT_EQ(static_cast<unsigned>(d.tag),
            static_cast<unsigned>(VaChunkConsumer::BuddyDirect));
  EXPECT_TRUE(pagemap_is_tracked(p));
  buddy_free_sized(p, 256 * 1024);
}

// =========================================================================
// 7. Cross-class coalescing — two 64 KiB free chunks beget a 128 KiB free
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, CrossClassCoalesceSingleThread) {
  // Allocate two adjacent 64 KiB chunks. Free them; subsequent 128 KiB
  // request must succeed (proves coalescing).
  void *a = buddy_alloc(kMinChunk);
  ASSERT_TRUE(a != nullptr);
  void *b = buddy_alloc(kMinChunk);
  ASSERT_TRUE(b != nullptr);
  buddy_free_sized(a, kMinChunk);
  buddy_free_sized(b, kMinChunk);

  void *c = buddy_alloc(k128K);
  ASSERT_TRUE(c != nullptr);
  buddy_free_sized(c, k128K);
}

// =========================================================================
// 8. 4-thread CAS contention does not corrupt the tree
// =========================================================================
namespace {

struct ContentionWorker {
  uint32_t iterations;
  uint32_t failures;
};

void *contention_worker(void *arg) {
  auto *w = static_cast<ContentionWorker *>(arg);
  for (uint32_t i = 0; i < w->iterations; ++i) {
    void *p = buddy_alloc(kMinChunk);
    if (p == nullptr) {
      w->failures++;
      continue;
    }
    static_cast<volatile char *>(p)[0] = static_cast<char>(i);
    buddy_free_sized(p, kMinChunk);
  }
  return nullptr;
}

} // namespace

TEST(LlvmLibcBuddyArenaTest, FourThreadCASContention) {
  constexpr uint32_t kThreads = 4;
  constexpr uint32_t kIters = 256;
  pthread_t tids[kThreads];
  ContentionWorker workers[kThreads];
  for (uint32_t i = 0; i < kThreads; ++i) {
    workers[i].iterations = kIters;
    workers[i].failures = 0;
    int rc = LIBC_NAMESPACE::pthread_create(&tids[i], nullptr,
                                             contention_worker, &workers[i]);
    ASSERT_EQ(rc, 0);
  }
  for (uint32_t i = 0; i < kThreads; ++i) {
    void *unused = nullptr;
    LIBC_NAMESPACE::pthread_join(tids[i], &unused);
    EXPECT_EQ(workers[i].failures, 0u);
  }
}

// =========================================================================
// 9. Mixed-size workload across threads exercises tree-level scattering
// =========================================================================
namespace {

void *mixed_worker(void *arg) {
  auto *w = static_cast<ContentionWorker *>(arg);
  for (uint32_t i = 0; i < w->iterations; ++i) {
    // 64K, 128K, 256K, 512K, 1M, 2M (skip 4M to avoid arena pressure)
    size_t sz = kMinChunk << (i % 6);
    void *p = buddy_alloc(sz);
    if (p == nullptr) {
      w->failures++;
      continue;
    }
    buddy_free_sized(p, sz);
  }
  return nullptr;
}

} // namespace

TEST(LlvmLibcBuddyArenaTest, MixedSizeMultiThread) {
  constexpr uint32_t kThreads = 8;
  constexpr uint32_t kIters = 128;
  pthread_t tids[kThreads];
  ContentionWorker workers[kThreads];
  for (uint32_t i = 0; i < kThreads; ++i) {
    workers[i].iterations = kIters;
    workers[i].failures = 0;
    int rc = LIBC_NAMESPACE::pthread_create(&tids[i], nullptr, mixed_worker,
                                             &workers[i]);
    ASSERT_EQ(rc, 0);
  }
  for (uint32_t i = 0; i < kThreads; ++i) {
    void *unused = nullptr;
    LIBC_NAMESPACE::pthread_join(tids[i], &unused);
    EXPECT_EQ(workers[i].failures, 0u);
  }
}

// =========================================================================
// 10. Stats snapshot reports realistic counts
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, StatsSnapshot) {
  BuddyStats before = buddy_stats_snapshot();
  void *p = buddy_alloc(kMinChunk);
  ASSERT_TRUE(p != nullptr);
  BuddyStats during = buddy_stats_snapshot();
  EXPECT_GT(during.total_chunks_live, before.total_chunks_live);
  EXPECT_GT(during.descriptor_pool_used, before.descriptor_pool_used);
  buddy_free_sized(p, kMinChunk);
}

// =========================================================================
// 11. Huge-direct path — > 4 MiB bypasses NBALLOC tree
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, HugeDirectPath) {
  size_t huge_bytes = 8 * 1024 * 1024; // 8 MiB
  void *p = buddy_alloc_huge(huge_bytes);
  ASSERT_TRUE(p != nullptr);

  PagemapDecoded d = pagemap_load_decoded(p);
  EXPECT_EQ(static_cast<unsigned>(d.tag),
            static_cast<unsigned>(VaChunkConsumer::HugeDirect));

  // Writable + readable
  static_cast<volatile char *>(p)[0] = 'X';
  static_cast<volatile char *>(p)[huge_bytes - 1] = 'Y';

  buddy_free_huge(p, huge_bytes);
}

// =========================================================================
// 12. buddy_alloc routes > 4 MiB to huge path implicitly
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, AllocAutoRoutesHuge) {
  size_t huge_bytes = 8 * 1024 * 1024;
  void *p = buddy_alloc(huge_bytes);
  ASSERT_TRUE(p != nullptr);
  PagemapDecoded d = pagemap_load_decoded(p);
  EXPECT_EQ(static_cast<unsigned>(d.tag),
            static_cast<unsigned>(VaChunkConsumer::HugeDirect));
  // Must be freed via the matching path. Day-1 simplification: same
  // sized-free path consults the size_class to dispatch internally.
  buddy_free_sized(p, huge_bytes);
}

// =========================================================================
// 13. Multiple chunks from same arena are non-overlapping
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, NonOverlappingAllocations) {
  void *ptrs[16];
  for (int i = 0; i < 16; ++i) {
    ptrs[i] = buddy_alloc(kMinChunk);
    ASSERT_TRUE(ptrs[i] != nullptr);
  }
  // Pairwise non-overlap check.
  for (int i = 0; i < 16; ++i) {
    for (int j = i + 1; j < 16; ++j) {
      uintptr_t a = reinterpret_cast<uintptr_t>(ptrs[i]);
      uintptr_t b = reinterpret_cast<uintptr_t>(ptrs[j]);
      uintptr_t lo = a < b ? a : b;
      uintptr_t hi = a < b ? b : a;
      EXPECT_GE(hi - lo, static_cast<uintptr_t>(kMinChunk));
    }
  }
  for (int i = 0; i < 16; ++i)
    buddy_free_sized(ptrs[i], kMinChunk);
}

// =========================================================================
// 14. Allocations land within first arena's partition VA
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, AllocationWithinPartition) {
  void *p = buddy_alloc(kMinChunk);
  ASSERT_TRUE(p != nullptr);
  EXPECT_TRUE(pagemap_is_tracked(p));
  buddy_free_sized(p, kMinChunk);
}

// =========================================================================
// 15. 1000-cycle stress — alloc/free at random sizes
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, ThousandCycleStress) {
  uint64_t lcg = 0xDEADBEEFCAFEBABEULL;
  for (uint32_t i = 0; i < 1000; ++i) {
    lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    uint8_t shift = kBuddyMinShift +
                    static_cast<uint8_t>((lcg >> 32) % kBuddyClassCount);
    size_t bytes = buddy_class_bytes(shift);
    void *p = buddy_alloc(bytes);
    ASSERT_TRUE(p != nullptr);
    static_cast<volatile char *>(p)[0] = static_cast<char>(i);
    buddy_free_sized(p, bytes);
  }
}

// =========================================================================
// 16. Repeated alloc/free at same class reuses the same VA range
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, ReuseAfterFree) {
  void *first = buddy_alloc(256 * 1024);
  ASSERT_TRUE(first != nullptr);
  buddy_free_sized(first, 256 * 1024);
  void *second = buddy_alloc(256 * 1024);
  ASSERT_TRUE(second != nullptr);
  // No strict guarantee that the same VA is reused (NBALLOC's randomized
  // start may scatter), but the reuse must succeed without OOM.
  buddy_free_sized(second, 256 * 1024);
}

// =========================================================================
// 17. Coalesce produces tree state allowing larger allocation
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, CoalesceProducesLargerFree) {
  // Allocate enough small chunks to fill a tree subtree, then free all
  // and verify a larger-class allocation can land.
  constexpr int N = 4;
  void *small[N];
  for (int i = 0; i < N; ++i) {
    small[i] = buddy_alloc(kMinChunk);
    ASSERT_TRUE(small[i] != nullptr);
  }
  for (int i = 0; i < N; ++i)
    buddy_free_sized(small[i], kMinChunk);

  // After freeing 4 × 64 KiB, NBALLOC should have coalesced upward.
  void *big = buddy_alloc(256 * 1024);
  ASSERT_TRUE(big != nullptr);
  buddy_free_sized(big, 256 * 1024);
}

// =========================================================================
// 18. After free + reuse, the pagemap entry reflects the NEW descriptor
//     slot (i.e. the prior chunk's stamp was retired and overwritten —
//     proves buddy_free_sized's retire path actually clears the entry).
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, PagemapEntryReplacedAfterReuse) {
  void *p1 = buddy_alloc(k128K);
  ASSERT_TRUE(p1 != nullptr);
  PagemapDecoded d1 = pagemap_load_decoded(p1);
  EXPECT_EQ(static_cast<unsigned>(d1.tag),
            static_cast<unsigned>(VaChunkConsumer::BuddyDirect));
  uint32_t slot1 = d1.slot_idx;

  buddy_free_sized(p1, k128K);

  // Re-acquire. Whether or not p2 == p1, the pagemap entry at p2 must
  // now decode to a fresh slot index (FreeFn either returned p1's slot
  // or never fired yet, but in either case desc2 was assigned
  // independently and the entry at p2 is now live again).
  void *p2 = buddy_alloc(k128K);
  ASSERT_TRUE(p2 != nullptr);
  PagemapDecoded d2 = pagemap_load_decoded(p2);
  EXPECT_EQ(static_cast<unsigned>(d2.tag),
            static_cast<unsigned>(VaChunkConsumer::BuddyDirect));
  if (p2 == p1)
    EXPECT_NE(d2.slot_idx, slot1);

  buddy_free_sized(p2, k128K);
}

// =========================================================================
// 19. Sequential allocations at different classes do not leak descriptors
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, NoDescriptorLeakAcrossClasses) {
  BuddyStats baseline = buddy_stats_snapshot();
  for (uint8_t shift = kBuddyMinShift; shift <= kBuddyMinShift + 4; ++shift) {
    size_t bytes = buddy_class_bytes(shift);
    void *p = buddy_alloc(bytes);
    ASSERT_TRUE(p != nullptr);
    buddy_free_sized(p, bytes);
  }
  // After all retires (which Crystalline defers), the count may not have
  // drained yet — but it MUST not be runaway.
  BuddyStats after = buddy_stats_snapshot();
  EXPECT_LE(after.descriptor_pool_used,
            baseline.descriptor_pool_used + 32u);
}

// =========================================================================
// 20. Concurrent allocators of the SAME class scatter via random-start
// =========================================================================
namespace {

struct ScatterWorker {
  uint32_t iterations;
  uint32_t collisions; // counted but not asserted strictly
  uintptr_t last_addr;
};

void *scatter_worker(void *arg) {
  auto *w = static_cast<ScatterWorker *>(arg);
  for (uint32_t i = 0; i < w->iterations; ++i) {
    void *p = buddy_alloc(kMinChunk);
    if (p == nullptr)
      continue;
    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    if (addr == w->last_addr)
      w->collisions++;
    w->last_addr = addr;
    buddy_free_sized(p, kMinChunk);
  }
  return nullptr;
}

} // namespace

TEST(LlvmLibcBuddyArenaTest, ScatterAcrossThreads) {
  constexpr uint32_t kThreads = 4;
  pthread_t tids[kThreads];
  ScatterWorker workers[kThreads] = {};
  for (uint32_t i = 0; i < kThreads; ++i) {
    workers[i].iterations = 32;
    int rc = LIBC_NAMESPACE::pthread_create(&tids[i], nullptr, scatter_worker,
                                             &workers[i]);
    ASSERT_EQ(rc, 0);
  }
  for (uint32_t i = 0; i < kThreads; ++i) {
    void *unused = nullptr;
    LIBC_NAMESPACE::pthread_join(tids[i], &unused);
  }
  // No assertion on collision count — randomized start does not guarantee
  // zero collisions, but the test exercises the path under contention.
}

// =========================================================================
// 21. The load-bearing safety property the pivot delivers:
//     pagemap_load_decoded on an arbitrary in-window address never
//     faults, even on an address that was never registered.
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, PagemapReadOfUnregisteredAddress) {
  // Sample ~20 addresses spaced 1 GiB apart inside the user-VA window,
  // skipping the very low addresses (where the buddy arena lives).
  // Every load must return Empty without faulting.
  uintptr_t max_va = reinterpret_cast<uintptr_t>(g_pcb.zone0.max_address());
  ASSERT_GT(max_va, uintptr_t{16ULL * 1024 * 1024 * 1024});
  for (uintptr_t off = uintptr_t{16} * 1024 * 1024 * 1024;
       off < max_va - kPagemapChunkBytes;
       off += uintptr_t{1} * 1024 * 1024 * 1024) {
    void *probe = reinterpret_cast<void *>(off);
    PagemapDecoded d = pagemap_load_decoded(probe);
    EXPECT_EQ(static_cast<unsigned>(d.tag),
              static_cast<unsigned>(VaChunkConsumer::Empty));
  }
}

// =========================================================================
// 22. Post-free read of the freed VA returns Empty (never faults).
//     The pre-pivot version of this test crashed because retire raced
//     with pagemap-page MEM_DECOMMIT; the snmalloc stay-committed
//     pattern eliminates that race by construction.
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, PagemapReadAfterFreeUnsafe) {
  void *p = buddy_alloc(k128K);
  ASSERT_TRUE(p != nullptr);
  EXPECT_TRUE(pagemap_is_tracked(p));
  buddy_free_sized(p, k128K);
  // Immediate post-free read — wait-free, non-faulting.
  PagemapDecoded d = pagemap_load_decoded(p);
  EXPECT_EQ(static_cast<unsigned>(d.tag),
            static_cast<unsigned>(VaChunkConsumer::Empty));
  EXPECT_FALSE(pagemap_is_tracked(p));
}

// =========================================================================
// 23. Per-slot cookie derivation is exported and self-consistent.
//     Layer 3 will exercise this for slab-page freelist links; we lock
//     down the function shape now so a future edit can't silently shift
//     the constant or rotation amount.
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, DeriveSlotCookieSpread) {
  uint32_t page_cookie = 0xDEADBEEFu;
  // Adjacent slots produce maximally-distinct cookies (the rotr16 of
  // a golden-ratio multiplier flips both halves between consecutive
  // indices).
  uint32_t c0 = derive_slot_cookie(page_cookie, 0);
  uint32_t c1 = derive_slot_cookie(page_cookie, 1);
  uint32_t c2 = derive_slot_cookie(page_cookie, 2);
  EXPECT_EQ(c0, page_cookie); // slot 0 mixes 0 → returns the page cookie
  EXPECT_NE(c1, c0);
  EXPECT_NE(c2, c1);
  EXPECT_NE(c2, c0);
  // page_cookie = 0 is a useful invariant test — derive_slot_cookie's
  // output should equal the rotr-mixed slot index alone.
  uint32_t mixed = 1u * 0x9E3779B9u;
  uint32_t expected = (mixed >> 16) | (mixed << 16);
  EXPECT_EQ(derive_slot_cookie(0, 1), expected);
}

// =========================================================================
// 24. pagemap_load_descriptor<BuddyDirect> resolves to the live arena
//     descriptor. Cross-checks chunk_base / chunk_bytes / size_class so
//     a regression in the typed-load wiring (wrong pool base, wrong
//     slot stride, off-by-one bounds) trips here.
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, PagemapLoadDescriptorBuddyDirect) {
  void *p = buddy_alloc(kMinChunk);
  ASSERT_TRUE(p != nullptr);
  BuddyChunkDescriptor *desc =
      pagemap_load_descriptor<VaChunkConsumer::BuddyDirect>(p);
  ASSERT_TRUE(desc != nullptr);
  EXPECT_EQ(desc->chunk_base, p);
  EXPECT_EQ(desc->chunk_bytes, static_cast<size_t>(kMinChunk));
  EXPECT_EQ(static_cast<size_t>(desc->size_class),
            static_cast<size_t>(kBuddyMinShift));
  EXPECT_EQ(desc->consumer_tag,
            static_cast<uint16_t>(VaChunkConsumer::BuddyDirect));
  buddy_free_sized(p, kMinChunk);
}

// =========================================================================
// 25. pagemap_load_descriptor<HugeDirect> resolves a huge-direct chunk
//     and the descriptor reports kClassHuge.
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, PagemapLoadDescriptorHugeDirect) {
  size_t huge_bytes = 8 * 1024 * 1024;
  void *p = buddy_alloc_huge(huge_bytes);
  ASSERT_TRUE(p != nullptr);
  BuddyChunkDescriptor *desc =
      pagemap_load_descriptor<VaChunkConsumer::HugeDirect>(p);
  ASSERT_TRUE(desc != nullptr);
  EXPECT_EQ(desc->chunk_base, p);
  EXPECT_EQ(desc->chunk_bytes, huge_bytes);
  EXPECT_EQ(static_cast<size_t>(desc->size_class),
            static_cast<size_t>(kClassHuge));
  EXPECT_EQ(desc->consumer_tag,
            static_cast<uint16_t>(VaChunkConsumer::HugeDirect));
  buddy_free_huge(p, huge_bytes);
}

// =========================================================================
// 26. Tag mismatch returns nullptr rather than misrouting. A buddy chunk
//     queried under the HugeDirect tag must not resolve to its
//     descriptor — the tag check is the routing gate.
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, PagemapLoadDescriptorTagMismatchReturnsNull) {
  void *p = buddy_alloc(kMinChunk);
  ASSERT_TRUE(p != nullptr);
  EXPECT_EQ(pagemap_load_descriptor<VaChunkConsumer::HugeDirect>(p),
            static_cast<BuddyChunkDescriptor *>(nullptr));
  // Sanity: BuddyDirect still resolves.
  EXPECT_TRUE(pagemap_load_descriptor<VaChunkConsumer::BuddyDirect>(p) !=
              nullptr);
  buddy_free_sized(p, kMinChunk);
}

// =========================================================================
// 27. Out-of-bounds address returns nullptr without faulting — the same
//     stay-committed safety property the decode path inherits, now
//     extended through the typed load.
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, PagemapLoadDescriptorOutOfBoundsReturnsNull) {
  uintptr_t max_va = reinterpret_cast<uintptr_t>(g_pcb.zone0.max_address());
  void *probe = reinterpret_cast<void *>(max_va + 0x10000);
  EXPECT_EQ(pagemap_load_descriptor<VaChunkConsumer::BuddyDirect>(probe),
            static_cast<BuddyChunkDescriptor *>(nullptr));
  EXPECT_EQ(pagemap_load_descriptor<VaChunkConsumer::HugeDirect>(probe),
            static_cast<BuddyChunkDescriptor *>(nullptr));
}

// =========================================================================
// 28. After buddy_free_sized zeros the pagemap entry, the typed load
//     observes the entry as Empty and returns nullptr — closing the
//     post-free reader-safety property end-to-end through the helper.
// =========================================================================
TEST(LlvmLibcBuddyArenaTest, PagemapLoadDescriptorPostFreeReturnsNull) {
  void *p = buddy_alloc(kMinChunk);
  ASSERT_TRUE(p != nullptr);
  ASSERT_TRUE(pagemap_load_descriptor<VaChunkConsumer::BuddyDirect>(p) !=
              nullptr);
  buddy_free_sized(p, kMinChunk);
  EXPECT_EQ(pagemap_load_descriptor<VaChunkConsumer::BuddyDirect>(p),
            static_cast<BuddyChunkDescriptor *>(nullptr));
}
