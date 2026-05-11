//===-- alloc::pagemap hermetic suite ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage of `windows::alloc::pagemap` — the address->metadata index for
// libc-allocator chunks, sealed Tier-A libc-internal VA, and cordons.
// POSIX-visible mappings are out of scope here (those live in
// `memory/va_tracker.h`'s ART + interval skiplist). The pivoted layer
// (8 B cookie-XOR'd entry, stay-committed PAGE_READONLY backing) is much
// smaller than its predecessor, so this suite is correspondingly focused:
//
//   * Cookie XOR encode/decode round-trip and the low-byte=0 invariant
//     that makes a freshly zero-filled (untouched-shared-zero) entry
//     decode to (slot_idx=0, tag=Empty).
//   * Store + load + retire pipeline on a single chunk-aligned address.
//   * Multi-entry register/store covering a full pagemap OS page boundary.
//   * pagemap_load_decoded is wait-free and non-faulting on arbitrary
//     in-window addresses (never touched, never registered) — this is
//     the load-bearing safety property the snmalloc pivot delivers.
//   * Concurrent disjoint store/load — no torn writes between adjacent
//     8 B slots in the same OS page.
//
// Test address strategy. We reserve a 16 MiB MEM_RESERVE_PLACEHOLDER once
// per process, find a 32 MiB-aligned window inside it, and use 64 KiB
// chunk-aligned slices of that window as our chunk_base values. 32 MiB
// === one pagemap OS page worth of user VA, so the placeholder
// guarantees that the pagemap OS pages covering our test chunks are
// NOT shared with any other consumer (substrate / posix_alloc / other
// chunk owner). We never write to the user VA at chunk_base — pagemap
// is keyed on `chunk_base >> kPagemapShift` and operates entirely on
// the pagemap reservation in Zone 0.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
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
using LIBC_NAMESPACE::windows::alloc::VaChunkConsumer;
using LIBC_NAMESPACE::windows::alloc::kPagemapChunkBytes;
using LIBC_NAMESPACE::windows::alloc::kPagemapEntriesPerOsPage;
using LIBC_NAMESPACE::windows::alloc::kPagemapOsPageSize;
using LIBC_NAMESPACE::windows::alloc::kPagemapShift;
using LIBC_NAMESPACE::windows::alloc::kPagemapTrackedVaPerOsPage;
using LIBC_NAMESPACE::windows::alloc::PagemapDecoded;
using LIBC_NAMESPACE::windows::alloc::pagemap_decode;
using LIBC_NAMESPACE::windows::alloc::pagemap_encode;
using LIBC_NAMESPACE::windows::alloc::pagemap_is_tracked;
using LIBC_NAMESPACE::windows::alloc::pagemap_load_decoded;
using LIBC_NAMESPACE::windows::alloc::pagemap_register_range;
using LIBC_NAMESPACE::windows::alloc::pagemap_retire_range;
using LIBC_NAMESPACE::windows::alloc::pagemap_store;
using LIBC_NAMESPACE::windows::alloc::pagemap_unregister_range;

// One pagemap OS page covers 32 MiB of tracked user VA at the new 8 B /
// 64 KiB density.
constexpr size_t kVAPerPagemapPage = kPagemapTrackedVaPerOsPage; // 32 MiB

// Reservation big enough to find an aligned multi-pagemap-page window
// — six adjacent exclusive pagemap OS pages so the multi-page register
// test can span boundaries.
constexpr size_t kReservationBytes = 256 * 1024 * 1024; // 256 MiB

struct TestArena {
  void *placeholder_base;
  void *aligned_base;     // first 32 MiB-aligned addr inside the placeholder
  size_t aligned_bytes;
};

TestArena *test_arena() {
  static TestArena arena = {nullptr, nullptr, 0};
  if (arena.aligned_base != nullptr)
    return &arena;
  void *p = LIBC_NAMESPACE::nt_pal::reserve_placeholder(kReservationBytes);
  if (p == nullptr)
    return nullptr;
  arena.placeholder_base = p;
  uintptr_t base = reinterpret_cast<uintptr_t>(p);
  uintptr_t aligned =
      (base + kVAPerPagemapPage - 1) & ~(kVAPerPagemapPage - 1);
  arena.aligned_base = reinterpret_cast<void *>(aligned);
  arena.aligned_bytes = kReservationBytes - (aligned - base);
  arena.aligned_bytes &= ~(kVAPerPagemapPage - 1);
  return &arena;
}

// chunk at slot_index within pagemap-OS-page page_idx of the test arena.
void *arena_chunk(unsigned page_idx, unsigned slot_index) {
  TestArena *a = test_arena();
  if (a == nullptr)
    return nullptr;
  uintptr_t b = reinterpret_cast<uintptr_t>(a->aligned_base);
  return reinterpret_cast<void *>(b + page_idx * kVAPerPagemapPage +
                                  slot_index * kPagemapChunkBytes);
}

unsigned lcg(unsigned &s) {
  s = s * 1664525u + 1013904223u;
  return s;
}

} // namespace

// =========================================================================
// Init invariants
// =========================================================================

TEST(LlvmLibcPagemapTest, ArenaReservationSucceeds) {
  ASSERT_NE(test_arena(), static_cast<TestArena *>(nullptr));
  ASSERT_NE(test_arena()->aligned_base, static_cast<void *>(nullptr));
  ASSERT_GE(test_arena()->aligned_bytes, kVAPerPagemapPage * 2);
}

TEST(LlvmLibcPagemapTest, PagemapBaseIsSealed) {
  EXPECT_NE(g_pcb.zone0.pagemap_base(), static_cast<void *>(nullptr));
  EXPECT_NE(g_pcb.zone0.pagemap_end(), static_cast<void *>(nullptr));
  EXPECT_LT(g_pcb.zone0.pagemap_base(), g_pcb.zone0.pagemap_end());
}

TEST(LlvmLibcPagemapTest, CookieLowByteZero) {
  uintptr_t cookie = g_pcb.zone0.pagemap_cookie();
  EXPECT_NE(cookie, uintptr_t{0});
  EXPECT_EQ(cookie & 0xFFu, uintptr_t{0});
}

// =========================================================================
// Cookie XOR encode/decode
// =========================================================================

TEST(LlvmLibcPagemapTest, EncodingRoundTrip) {
  // Zero entry decodes to tag=Empty (cookie low byte is sealed to 0 at
  // init). slot_idx for a zero entry is cookie>>8 — undefined garbage
  // that callers never consult because they gate on tag first.
  PagemapDecoded zero = pagemap_decode(0);
  EXPECT_EQ(static_cast<unsigned>(zero.tag),
            static_cast<unsigned>(VaChunkConsumer::Empty));

  const struct {
    uint32_t slot;
    VaChunkConsumer tag;
  } cases[] = {
      {0, VaChunkConsumer::BuddyDirect},
      {1, VaChunkConsumer::HugeDirect},
      {0xFF, VaChunkConsumer::BuddyDirect},
      {0x12345, VaChunkConsumer::HugeDirect},
      {0x00FFFFFFu, VaChunkConsumer::BuddyDirect},
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
// Single-entry register / store / retire
// =========================================================================

TEST(LlvmLibcPagemapTest, EmptyEntryReturnsEmpty) {
  // After register_range — but before any store — the slot reads as
  // empty. Either the OS page was already RW from a previous test (in
  // which case retire_range zeroed our entry below) or the read goes
  // through the still-RO shared-zero page.
  void *chunk = arena_chunk(0, 0);
  ASSERT_EQ(pagemap_register_range(chunk, kPagemapChunkBytes), 0);
  PagemapDecoded d = pagemap_load_decoded(chunk);
  EXPECT_EQ(static_cast<unsigned>(d.tag),
            static_cast<unsigned>(VaChunkConsumer::Empty));
  EXPECT_FALSE(pagemap_is_tracked(chunk));
  pagemap_retire_range(chunk, kPagemapChunkBytes);
  pagemap_unregister_range(chunk, kPagemapChunkBytes);
}

TEST(LlvmLibcPagemapTest, StoreLoadRoundtrip) {
  void *chunk = arena_chunk(0, 1);
  ASSERT_EQ(pagemap_register_range(chunk, kPagemapChunkBytes), 0);
  pagemap_store(chunk, 0xCAFEBA, VaChunkConsumer::BuddyDirect);
  PagemapDecoded d = pagemap_load_decoded(chunk);
  EXPECT_EQ(d.slot_idx, uint32_t{0xCAFEBA});
  EXPECT_EQ(static_cast<unsigned>(d.tag),
            static_cast<unsigned>(VaChunkConsumer::BuddyDirect));
  EXPECT_TRUE(pagemap_is_tracked(chunk));
  pagemap_retire_range(chunk, kPagemapChunkBytes);
  pagemap_unregister_range(chunk, kPagemapChunkBytes);
}

TEST(LlvmLibcPagemapTest, DistinctChunksDistinctSlots) {
  // Same pagemap OS page, different slots: each chunk's slot must
  // resolve independently. A bit-shift typo in encode/decode would
  // cross-contaminate adjacent slots and surface here.
  constexpr unsigned kN = 8;
  void *chunks[kN];
  for (unsigned i = 0; i < kN; ++i) {
    chunks[i] = arena_chunk(0, 16 + i);
    ASSERT_EQ(pagemap_register_range(chunks[i], kPagemapChunkBytes), 0);
    pagemap_store(chunks[i], 0x10000u * (i + 1),
                  static_cast<VaChunkConsumer>(0x10 + (i & 0x0F)));
  }
  for (unsigned i = 0; i < kN; ++i) {
    PagemapDecoded d = pagemap_load_decoded(chunks[i]);
    EXPECT_EQ(d.slot_idx, 0x10000u * (i + 1));
    EXPECT_EQ(static_cast<unsigned>(d.tag),
              static_cast<unsigned>(0x10 + (i & 0x0F)));
  }
  for (unsigned i = 0; i < kN; ++i) {
    pagemap_retire_range(chunks[i], kPagemapChunkBytes);
    pagemap_unregister_range(chunks[i], kPagemapChunkBytes);
  }
}

// =========================================================================
// Bounds + safety
// =========================================================================

TEST(LlvmLibcPagemapTest, RegisterNullReturnsEinval) {
  EXPECT_EQ(pagemap_register_range(nullptr, kPagemapChunkBytes),
            -22 /*EINVAL*/);
}

TEST(LlvmLibcPagemapTest, RegisterZeroBytesReturnsEinval) {
  void *chunk = arena_chunk(0, 0);
  EXPECT_EQ(pagemap_register_range(chunk, 0), -22 /*EINVAL*/);
}

TEST(LlvmLibcPagemapTest, RegisterOutOfBoundsReturnsEinval) {
  uintptr_t max_va = reinterpret_cast<uintptr_t>(g_pcb.zone0.max_address());
  // `max_va` itself is the last valid chunk-aligned address (its pagemap
  // entry sits inside the reservation). Add one full pagemap-OS-page of
  // VA coverage so the covering pagemap page is past the reservation's
  // upper bound.
  void *out_of_bounds =
      reinterpret_cast<void *>(max_va + kVAPerPagemapPage);
  EXPECT_EQ(pagemap_register_range(out_of_bounds, kPagemapChunkBytes),
            -22 /*EINVAL*/);
}

// The load-bearing safety property the snmalloc pivot delivers: a load
// on an arbitrary in-window address — never touched, never registered —
// returns Empty without faulting.
TEST(LlvmLibcPagemapTest, LoadOfArbitraryAddressNeverFaults) {
  uintptr_t max_va = reinterpret_cast<uintptr_t>(g_pcb.zone0.max_address());
  ASSERT_GT(max_va, uintptr_t{16ULL * 1024 * 1024 * 1024});
  // Sample addresses strided across the user-VA window. No registers,
  // no stores — these chunks were never touched. Each load must return
  // Empty (zero entry decodes to Empty thanks to the cookie low-byte
  // constraint) and must never fault.
  for (uintptr_t off = uintptr_t{16} * 1024 * 1024 * 1024;
       off < max_va - kPagemapChunkBytes;
       off += uintptr_t{1} * 1024 * 1024 * 1024) {
    void *probe = reinterpret_cast<void *>(off);
    PagemapDecoded d = pagemap_load_decoded(probe);
    EXPECT_EQ(static_cast<unsigned>(d.tag),
              static_cast<unsigned>(VaChunkConsumer::Empty));
  }
}

TEST(LlvmLibcPagemapTest, IsTrackedHandlesOutOfBounds) {
  uintptr_t max_va = reinterpret_cast<uintptr_t>(g_pcb.zone0.max_address());
  EXPECT_FALSE(pagemap_is_tracked(reinterpret_cast<void *>(max_va)));
  EXPECT_FALSE(
      pagemap_is_tracked(reinterpret_cast<void *>(max_va + 0x10000)));
}

// =========================================================================
// Multi-entry register / store across a pagemap OS page boundary
// =========================================================================

TEST(LlvmLibcPagemapTest, RangeSpansMultiplePagemapPages) {
  TestArena *a = test_arena();
  ASSERT_NE(a, static_cast<TestArena *>(nullptr));
  // Span 32 MiB + 64 KiB to straddle exactly one pagemap OS page boundary.
  void *base = arena_chunk(0, 0);
  size_t bytes = kVAPerPagemapPage + kPagemapChunkBytes;
  ASSERT_EQ(pagemap_register_range(base, bytes), 0);

  // Spot-check chunks on each side of the page boundary read as Empty.
  EXPECT_EQ(static_cast<unsigned>(pagemap_load_decoded(arena_chunk(0, 0)).tag),
            static_cast<unsigned>(VaChunkConsumer::Empty));
  EXPECT_EQ(static_cast<unsigned>(
                pagemap_load_decoded(arena_chunk(0, kPagemapEntriesPerOsPage - 1))
                    .tag),
            static_cast<unsigned>(VaChunkConsumer::Empty));
  EXPECT_EQ(static_cast<unsigned>(pagemap_load_decoded(arena_chunk(1, 0)).tag),
            static_cast<unsigned>(VaChunkConsumer::Empty));

  // Stamp + verify cross-page.
  pagemap_store(arena_chunk(0, 200), 0xAA00u, VaChunkConsumer::BuddyDirect);
  pagemap_store(arena_chunk(1, 0), 0xBB00u, VaChunkConsumer::HugeDirect);
  PagemapDecoded d0 = pagemap_load_decoded(arena_chunk(0, 200));
  PagemapDecoded d1 = pagemap_load_decoded(arena_chunk(1, 0));
  EXPECT_EQ(d0.slot_idx, uint32_t{0xAA00});
  EXPECT_EQ(static_cast<unsigned>(d0.tag),
            static_cast<unsigned>(VaChunkConsumer::BuddyDirect));
  EXPECT_EQ(d1.slot_idx, uint32_t{0xBB00});
  EXPECT_EQ(static_cast<unsigned>(d1.tag),
            static_cast<unsigned>(VaChunkConsumer::HugeDirect));

  pagemap_retire_range(arena_chunk(0, 200), kPagemapChunkBytes);
  pagemap_retire_range(arena_chunk(1, 0), kPagemapChunkBytes);
  pagemap_unregister_range(base, bytes);
}

// =========================================================================
// Register is idempotent — repeating it doesn't fail
// =========================================================================

TEST(LlvmLibcPagemapTest, RepeatRegisterIsIdempotent) {
  void *chunk = arena_chunk(0, 32);
  ASSERT_EQ(pagemap_register_range(chunk, kPagemapChunkBytes), 0);
  ASSERT_EQ(pagemap_register_range(chunk, kPagemapChunkBytes), 0);
  ASSERT_EQ(pagemap_register_range(chunk, kPagemapChunkBytes), 0);
  // Page is RW now; store + load must work.
  pagemap_store(chunk, 0xABCDu, VaChunkConsumer::BuddyDirect);
  EXPECT_TRUE(pagemap_is_tracked(chunk));
  pagemap_retire_range(chunk, kPagemapChunkBytes);
  pagemap_unregister_range(chunk, kPagemapChunkBytes);
  pagemap_unregister_range(chunk, kPagemapChunkBytes);
}

// =========================================================================
// Concurrent disjoint store/load — verify single-publisher ownership
// produces consistent reads under contention from threads scribbling
// adjacent 8 B slots in the same OS page.
// =========================================================================

namespace {

struct DisjointCtx {
  unsigned slot_index;
  unsigned long iters;
  Atomic<uint32_t> *errors;
  unsigned seed;
};

void *disjoint_worker(void *arg) {
  auto *ctx = static_cast<DisjointCtx *>(arg);
  void *chunk = arena_chunk(0, ctx->slot_index);
  unsigned s = ctx->seed;
  for (unsigned long i = 0; i < ctx->iters; ++i) {
    if (pagemap_register_range(chunk, kPagemapChunkBytes) != 0) {
      ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    uint32_t want_slot = (ctx->slot_index << 16) | (lcg(s) & 0xFFFF);
    pagemap_store(chunk, want_slot, VaChunkConsumer::BuddyDirect);
    PagemapDecoded got = pagemap_load_decoded(chunk);
    if (got.slot_idx != want_slot ||
        got.tag != VaChunkConsumer::BuddyDirect)
      ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
    pagemap_retire_range(chunk, kPagemapChunkBytes);
    pagemap_unregister_range(chunk, kPagemapChunkBytes);
  }
  return nullptr;
}

} // namespace

TEST(LlvmLibcPagemapTest, ConcurrentDisjointChunksSamePagemapPage) {
  constexpr unsigned kThreads = 16;
  constexpr unsigned long kIters = 2000;
  Atomic<uint32_t> errors{0};
  pthread_t tids[kThreads];
  DisjointCtx ctx[kThreads];
  for (unsigned t = 0; t < kThreads; ++t) {
    // Stride across the OS page so threads write distinct 8 B entries.
    ctx[t] = {/*slot_index=*/128 + t * 8, kIters, &errors,
              0x9E3779B9u * (t + 1)};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&tids[t], nullptr,
                                             disjoint_worker, &ctx[t]),
              0);
  }
  for (unsigned t = 0; t < kThreads; ++t)
    LIBC_NAMESPACE::pthread_join(tids[t], nullptr);
  EXPECT_EQ(errors.load(MemoryOrder::RELAXED), uint32_t{0});
}

// =========================================================================
// Concurrent same-chunk register — exercises the upgrade-byte CAS-elision
// path under contention. With the snmalloc stay-committed model, a same-
// chunk burst from N threads must converge: the first thread's protect()
// upgrades the OS page, the rest see byte=1 and skip. No CAS contention,
// no missed upgrade, no AV from a publish racing the upgrade.
// =========================================================================

namespace {

struct SameChunkCtx {
  void *chunk;
  unsigned long iters;
  Atomic<uint32_t> *errors;
};

void *same_chunk_worker(void *arg) {
  auto *ctx = static_cast<SameChunkCtx *>(arg);
  for (unsigned long i = 0; i < ctx->iters; ++i) {
    if (pagemap_register_range(ctx->chunk, kPagemapChunkBytes) != 0)
      ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
  }
  return nullptr;
}

} // namespace

TEST(LlvmLibcPagemapTest, ConcurrentSameChunkRegister) {
  void *chunk = arena_chunk(0, 100);
  constexpr unsigned kThreads = 16;
  constexpr unsigned long kIters = 2000;
  Atomic<uint32_t> errors{0};
  pthread_t tids[kThreads];
  SameChunkCtx ctx[kThreads];
  for (unsigned t = 0; t < kThreads; ++t) {
    ctx[t] = {chunk, kIters, &errors};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&tids[t], nullptr,
                                             same_chunk_worker, &ctx[t]),
              0);
  }
  for (unsigned t = 0; t < kThreads; ++t)
    LIBC_NAMESPACE::pthread_join(tids[t], nullptr);
  EXPECT_EQ(errors.load(MemoryOrder::RELAXED), uint32_t{0});

  // A subsequent store/load must succeed — the OS page is RW.
  pagemap_store(chunk, 0xABCD, VaChunkConsumer::BuddyDirect);
  EXPECT_TRUE(pagemap_is_tracked(chunk));
  pagemap_retire_range(chunk, kPagemapChunkBytes);
}
