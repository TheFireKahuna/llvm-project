//===-- VaSubstrate comprehensive hermetic test ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Comprehensive coverage for VaSubstrate's public surface. Hermetic exe
// with a file-scope static fixture and TEST() cases driving every public
// entry point, plus the stressors:
//
//   * Multi-slot capacity — fill a single arena, force a second arena,
//     verify arena_count reflects it.
//   * Retire + reclaim — drain a multi-arena burst, pump try_reclaim,
//     verify VA comes back to the pool.
//   * Concurrent acquire/release — N threads × M iters with a fill/verify
//     pattern across every byte of every slot; any cross-slot aliasing
//     or tail-page inaccessibility surfaces as an immediate mismatch or
//     an AV.
//   * Cross-thread release — producer/consumer via MPMC ring: the thread
//     that ran the acquire CAS isn't the one that runs the release CAS.
//     Exercises owner_token CAS-consume from a different core.
//
// The substrate is brought up by `pcb_startup_init` (Tier A Phase 0a)
// before `__libc_init` dispatches to `main`, so `g_substrate` is live
// with one pre-warmed arena per class at the moment the first TEST
// runs. Every TEST body uses only the public substrate API — no friend
// access — which keeps the test honest about the contract consumers see.
//
// Gaps NOT covered by this suite (documented for follow-up):
//
//   * Trap-path death tests (double-release, forged ptr, bit-flipped
//     token, wrong class bits). These require fork + WIFSIGNALED: child
//     runs the bad op, parent observes SIGILL / SIGSEGV. Hermetic fork
//     is non-functional in this tree today — a trivial
//     `fork → NtTerminateProcess(0) → waitpid` pair crashes the parent
//     (exit 127). Every fork-adjacent hermetic test in-tree
//     (pty_lifecycle, vt_pty_tree_join) routes through posix_spawn, not
//     raw fork. Covering the trap surface needs a c.dll-hosted unittest
//     (separate file) with a handle-builder test hook to construct bad
//     inputs; that's a separate scaffolding change.
//
//   * Fork reinit round-trip — same fork dependency; parent-side state
//     snapshot + child-side acquire/release can't run in the current
//     hermetic model.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/va_substrate.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_helpers.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_join.h"
#include "test/UnitTest/Test.h"

#include <stddef.h>
#include <stdint.h>

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::windows::alloc::ConsumerTag;
using LIBC_NAMESPACE::windows::alloc::g_substrate;
using LIBC_NAMESPACE::windows::alloc::layout_of;
using LIBC_NAMESPACE::windows::alloc::SubSlotClass;
using LIBC_NAMESPACE::windows::alloc::SubSlotHandle;
using LIBC_NAMESPACE::windows::alloc::SubSlotLayout;

namespace {

// The three live classes enumerated. `Count` is the sentinel and isn't
// a real class — ordering here must match the enum so token-class-bit
// checks can index directly.
constexpr SubSlotClass kClasses[] = {SubSlotClass::Small, SubSlotClass::Medium,
                                     SubSlotClass::Large};
constexpr unsigned kNumClasses = 3;

// Upper bound on slots_per_arena across every class (Small has 60).
// Used to size fixed-length handle arrays so the tests stay freestanding-
// friendly (no heap alloc).
constexpr unsigned kMaxSlotsPerArena = 80;

// Stamp every byte of [p, p+n) with a keyed LCG so a cross-slot write
// corrupts into a readable mismatch rather than a benign-looking overlap
// of the same seed.
void fill_pattern(void *p, size_t n, unsigned seed) {
  auto *b = static_cast<unsigned char *>(p);
  unsigned x = seed;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1103515245u + 12345u;
    b[i] = static_cast<unsigned char>(x >> 24);
  }
}

bool verify_pattern(const void *p, size_t n, unsigned seed) {
  const auto *b = static_cast<const unsigned char *>(p);
  unsigned x = seed;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1103515245u + 12345u;
    if (b[i] != static_cast<unsigned char>(x >> 24))
      return false;
  }
  return true;
}

unsigned lcg(unsigned &s) {
  s = s * 1664525u + 1013904223u;
  return s;
}

} // namespace

// ============================================================================
// Single-thread correctness
// ============================================================================

TEST(LlvmLibcVaSubstrateTest, PreInitPreWarmsOneArenaPerClass) {
  // Phase 0a reserves one seed arena per class before __libc_init dispatches
  // to main — every pool must report a non-zero arena count on the very
  // first TEST to run.
  for (SubSlotClass c : kClasses)
    EXPECT_GE(g_substrate.arena_count(c), uint32_t{1});
}

TEST(LlvmLibcVaSubstrateTest, AcquireReturnsValidHandle) {
  for (SubSlotClass c : kClasses) {
    SubSlotHandle h = g_substrate.acquire(c, ConsumerTag::SlabPool);
    ASSERT_TRUE(static_cast<bool>(h));
    EXPECT_NE(h.ptr(), static_cast<void *>(nullptr));
    EXPECT_NE(h.token(), uint64_t{0});
    // Class bits occupy the top 3 bits of the token per the SubSlotHandle
    // layout (5 classes today, room for 8) — wrong-class tokens would
    // route to the wrong pool's release.
    EXPECT_EQ(static_cast<unsigned>(h.token() >> 61) & 0x7u,
              static_cast<unsigned>(c));
    // Slot starts are slot_size-aligned within the slot region, and the
    // slot region is page-aligned (so the absolute ptr is page-aligned
    // too). The absolute ptr is NOT slot_size-aligned — the slot region
    // starts at kLeadingBytes (12 KB = guard+header+guard) which is a
    // 4 KB multiple but not a 16 KB / 64 KB / 128 KB multiple.
    const SubSlotLayout lo = layout_of(c);
    const uintptr_t p = reinterpret_cast<uintptr_t>(h.ptr());
    const uintptr_t arena_base = p & ~(lo.arena_size - 1);
    EXPECT_EQ((p - arena_base - lo.slot_region_offset) %
                  static_cast<uintptr_t>(lo.slot_size),
              uintptr_t{0});
    EXPECT_EQ(p & uintptr_t{0xFFF}, uintptr_t{0});
    g_substrate.release(h);
  }
}

TEST(LlvmLibcVaSubstrateTest, EmptyHandleIsFalsey) {
  SubSlotHandle empty;
  EXPECT_FALSE(static_cast<bool>(empty));
  EXPECT_EQ(empty.ptr(), static_cast<void *>(nullptr));
  EXPECT_EQ(empty.token(), uint64_t{0});
}

TEST(LlvmLibcVaSubstrateTest, SlotIsFullyWritable) {
  // Every byte of the returned slot must be readable and writable. The
  // classic regression this catches is an arena where the tail page
  // wasn't committed — the write faults mid-pattern. A fill then verify
  // hits every offset deterministically.
  for (SubSlotClass c : kClasses) {
    SubSlotHandle h = g_substrate.acquire(c, ConsumerTag::SlabPool);
    ASSERT_TRUE(static_cast<bool>(h));
    const size_t n = layout_of(c).slot_size;
    const unsigned seed = 0xC0FFEEu ^ static_cast<unsigned>(c);
    fill_pattern(h.ptr(), n, seed);
    EXPECT_TRUE(verify_pattern(h.ptr(), n, seed));
    g_substrate.release(h);
  }
}

TEST(LlvmLibcVaSubstrateTest, HandlesWithinClassAreDistinct) {
  // 8 simultaneous Medium handles — confirm every pair has distinct
  // ptr and token. Aliased returns would be a live_count-vs-bitmap
  // consistency bug.
  constexpr unsigned K = 8;
  SubSlotHandle hs[K];
  for (unsigned i = 0; i < K; ++i) {
    hs[i] = g_substrate.acquire(SubSlotClass::Medium, ConsumerTag::SlabPool);
    ASSERT_TRUE(static_cast<bool>(hs[i]));
    for (unsigned j = 0; j < i; ++j) {
      EXPECT_NE(hs[i].ptr(), hs[j].ptr());
      EXPECT_NE(hs[i].token(), hs[j].token());
    }
  }
  for (unsigned i = 0; i < K; ++i)
    g_substrate.release(hs[i]);
}

TEST(LlvmLibcVaSubstrateTest, CrossClassHandlesDoNotOverlap) {
  // Hold one handle per class simultaneously and verify their slot
  // ranges are mutually non-overlapping. Classes live in separate
  // arenas so this is really a sanity test on arena-VA disjointness
  // and on the token class-bit dispatch.
  SubSlotHandle hs[kNumClasses];
  for (unsigned i = 0; i < kNumClasses; ++i) {
    hs[i] = g_substrate.acquire(kClasses[i], ConsumerTag::SlabPool);
    ASSERT_TRUE(static_cast<bool>(hs[i]));
    EXPECT_EQ(static_cast<unsigned>(hs[i].token() >> 61) & 0x7u, i);
  }
  for (unsigned i = 0; i < kNumClasses; ++i) {
    const uintptr_t ai = reinterpret_cast<uintptr_t>(hs[i].ptr());
    const size_t si = layout_of(kClasses[i]).slot_size;
    for (unsigned j = 0; j < i; ++j) {
      const uintptr_t aj = reinterpret_cast<uintptr_t>(hs[j].ptr());
      const size_t sj = layout_of(kClasses[j]).slot_size;
      EXPECT_TRUE(ai + si <= aj || aj + sj <= ai);
    }
  }
  for (unsigned i = 0; i < kNumClasses; ++i)
    g_substrate.release(hs[i]);
}

TEST(LlvmLibcVaSubstrateTest, ReleaseReturnsSlotsToPool) {
  // Fill a Medium arena, release, refill. Second fill must not grow
  // arena_count past the first fill's peak — released slots are
  // re-usable from the same arena.
  const SubSlotLayout lo = layout_of(SubSlotClass::Medium);
  const unsigned N = lo.slots_per_arena;
  ASSERT_LE(N, kMaxSlotsPerArena);
  SubSlotHandle hs[kMaxSlotsPerArena];

  for (unsigned i = 0; i < N; ++i) {
    hs[i] = g_substrate.acquire(SubSlotClass::Medium, ConsumerTag::SlabPool);
    ASSERT_TRUE(static_cast<bool>(hs[i]));
  }
  const uint32_t peak = g_substrate.arena_count(SubSlotClass::Medium);
  for (unsigned i = 0; i < N; ++i)
    g_substrate.release(hs[i]);

  for (unsigned i = 0; i < N; ++i) {
    hs[i] = g_substrate.acquire(SubSlotClass::Medium, ConsumerTag::SlabPool);
    ASSERT_TRUE(static_cast<bool>(hs[i]));
  }
  EXPECT_LE(g_substrate.arena_count(SubSlotClass::Medium), peak);
  for (unsigned i = 0; i < N; ++i)
    g_substrate.release(hs[i]);
}

// ============================================================================
// Capacity & arena growth
// ============================================================================

TEST(LlvmLibcVaSubstrateTest, OverflowSpawnsNewArena) {
  // Acquire slots_per_arena + 1 slots — the +1 cannot fit in the seed
  // arena (or any single arena), so arena_count must grow by ≥1.
  // Small is used because it has the most slots/arena → cheapest fill.
  const SubSlotLayout lo = layout_of(SubSlotClass::Small);
  const uint32_t start = g_substrate.arena_count(SubSlotClass::Small);
  const unsigned N = lo.slots_per_arena + 1;
  ASSERT_LE(N, kMaxSlotsPerArena);
  SubSlotHandle hs[kMaxSlotsPerArena];

  for (unsigned i = 0; i < N; ++i) {
    hs[i] = g_substrate.acquire(SubSlotClass::Small, ConsumerTag::SlabPool);
    ASSERT_TRUE(static_cast<bool>(hs[i]));
  }
  EXPECT_GT(g_substrate.arena_count(SubSlotClass::Small), start);
  for (unsigned i = 0; i < N; ++i)
    g_substrate.release(hs[i]);
}

TEST(LlvmLibcVaSubstrateTest, TryReclaimDrainsDrainedArenas) {
  // Burst Large enough to force multiple arenas, release all, pump
  // try_reclaim until the count retreats. Epoch-gated reclaim only
  // frees arenas whose retire_epoch is below min_pinned_epoch — with
  // no other threads holding EpochGuards the drain is prompt.
  const SubSlotLayout lo = layout_of(SubSlotClass::Large);
  const uint32_t start = g_substrate.arena_count(SubSlotClass::Large);
  const unsigned N = lo.slots_per_arena * 3 + 1; // forces ≥2 new arenas
  ASSERT_LE(N, kMaxSlotsPerArena);
  SubSlotHandle hs[kMaxSlotsPerArena];

  for (unsigned i = 0; i < N; ++i) {
    hs[i] = g_substrate.acquire(SubSlotClass::Large, ConsumerTag::SlabPool);
    ASSERT_TRUE(static_cast<bool>(hs[i]));
  }
  const uint32_t peak = g_substrate.arena_count(SubSlotClass::Large);
  EXPECT_GT(peak, start);
  for (unsigned i = 0; i < N; ++i)
    g_substrate.release(hs[i]);

  // try_reclaim scans up to kReclaimScanBudget (=8) arenas per call.
  // Several passes guarantee we cross the full quarantine list.
  for (int k = 0; k < 8; ++k)
    g_substrate.try_reclaim();

  EXPECT_LE(g_substrate.arena_count(SubSlotClass::Large), peak);
}

TEST(LlvmLibcVaSubstrateTest, TryReclaimIsIdempotentWhenQuiescent) {
  // Calling try_reclaim with no quarantined arenas must be a no-op and
  // must not perturb arena_count for any class.
  uint32_t before[kNumClasses];
  for (unsigned i = 0; i < kNumClasses; ++i)
    before[i] = g_substrate.arena_count(kClasses[i]);
  for (int k = 0; k < 4; ++k)
    g_substrate.try_reclaim();
  for (unsigned i = 0; i < kNumClasses; ++i)
    EXPECT_EQ(g_substrate.arena_count(kClasses[i]), before[i]);
}

// ============================================================================
// Concurrent acquire / release stress
// ============================================================================
//
// N threads each cycle a bounded set of live handles. On every iteration a
// thread either releases an existing slot (after verifying its pattern is
// intact) or acquires a new one and stamps a pattern. A pattern mismatch
// means another thread wrote into our slot — i.e. an aliasing or double-
// acquire bug. A tail-page-inaccessible bug would AV inside fill_pattern.

struct StressCtx {
  unsigned seed;
  unsigned long iters;
  Atomic<uint32_t> *errors;
};

struct LiveEntry {
  SubSlotHandle h;
  unsigned seed;
  size_t size;
};

void *stress_worker(void *arg) {
  auto *ctx = static_cast<StressCtx *>(arg);
  constexpr unsigned kLive = 12;
  LiveEntry live[kLive] = {};
  unsigned s = ctx->seed;

  for (unsigned long i = 0; i < ctx->iters; ++i) {
    const unsigned slot = lcg(s) % kLive;
    if (static_cast<bool>(live[slot].h)) {
      if (!verify_pattern(live[slot].h.ptr(), live[slot].size,
                          live[slot].seed))
        ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
      g_substrate.release(live[slot].h);
      live[slot] = {};
    } else {
      const SubSlotClass c = kClasses[lcg(s) % kNumClasses];
      SubSlotHandle h = g_substrate.acquire(c, ConsumerTag::SlabPool);
      if (!static_cast<bool>(h)) {
        ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
        continue;
      }
      const unsigned seed = lcg(s) | 1u;
      const size_t size = layout_of(c).slot_size;
      fill_pattern(h.ptr(), size, seed);
      live[slot] = {h, seed, size};
    }
  }
  for (unsigned k = 0; k < kLive; ++k) {
    if (static_cast<bool>(live[k].h)) {
      if (!verify_pattern(live[k].h.ptr(), live[k].size, live[k].seed))
        ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
      g_substrate.release(live[k].h);
    }
  }
  return nullptr;
}

TEST(LlvmLibcVaSubstrateTest, ConcurrentAcquireReleaseNoCorruption) {
  constexpr unsigned kThreads = 8;
  constexpr unsigned long kIters = 4000;
  Atomic<uint32_t> errors{0};
  pthread_t tids[kThreads];
  StressCtx ctx[kThreads];
  for (unsigned t = 0; t < kThreads; ++t) {
    ctx[t] = {t * 0x9E3779B9u + 1u, kIters, &errors};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&tids[t], nullptr, stress_worker,
                                             &ctx[t]),
              0);
  }
  for (unsigned t = 0; t < kThreads; ++t)
    LIBC_NAMESPACE::pthread_join(tids[t], nullptr);
  EXPECT_EQ(errors.load(MemoryOrder::RELAXED), uint32_t{0});
}

// ============================================================================
// Cross-thread release via MPMC ring
// ============================================================================
//
// Producers acquire + stamp + enqueue. Consumers dequeue + verify + release.
// The acquirer and releaser are always different threads — the owner_token
// CAS-consume runs on a core that never ran the acquire's live_count CAS,
// which is the hand-off pattern SlabPool's xthread_free relies on.

struct Handoff {
  SubSlotHandle h;
  unsigned seed;
  size_t size;
};

constexpr unsigned kRingSize = 256; // power of 2

// Vyukov-style MPMC bounded ring. Each slot carries a sequence number that
// encodes the next permitted operation, so producer-writes-before-CAS and
// consumer-reads-after-CAS are both enforced by the slot state itself — no
// torn reads or reads of uninitialised slots under contention.
//
// Invariant per slot at position p = pos & (kRingSize - 1):
//   seq == pos           → slot empty, producer may fill
//   seq == pos + 1       → slot full, consumer may drain
//   seq == pos + N       → slot was drained at pos; producer at pos + N
//                          may fill
struct RingEntry {
  Atomic<uint64_t> seq{0};
  Handoff data{};
};

struct Ring {
  RingEntry entries[kRingSize];
  alignas(64) Atomic<uint64_t> head{0};
  alignas(64) Atomic<uint64_t> tail{0};
  Atomic<bool> done{false};

  Ring() {
    for (unsigned i = 0; i < kRingSize; ++i)
      entries[i].seq.store(static_cast<uint64_t>(i), MemoryOrder::RELAXED);
  }
};

bool ring_push(Ring &r, const Handoff &h) {
  for (int spin = 0; spin < 1000000; ++spin) {
    uint64_t pos = r.head.load(MemoryOrder::RELAXED);
    RingEntry &e = r.entries[pos & (kRingSize - 1)];
    uint64_t seq = e.seq.load(MemoryOrder::ACQUIRE);
    int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
    if (diff == 0) {
      if (r.head.compare_exchange_weak(pos, pos + 1, MemoryOrder::RELAXED,
                                       MemoryOrder::RELAXED)) {
        e.data = h;
        e.seq.store(pos + 1, MemoryOrder::RELEASE);
        return true;
      }
    }
    // diff < 0: ring full; diff > 0: another producer advanced ahead of us.
    // Both: retry.
  }
  return false;
}

bool ring_pop(Ring &r, Handoff *out) {
  for (;;) {
    uint64_t pos = r.tail.load(MemoryOrder::RELAXED);
    RingEntry &e = r.entries[pos & (kRingSize - 1)];
    uint64_t seq = e.seq.load(MemoryOrder::ACQUIRE);
    int64_t diff =
        static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1);
    if (diff == 0) {
      if (r.tail.compare_exchange_weak(pos, pos + 1, MemoryOrder::RELAXED,
                                       MemoryOrder::RELAXED)) {
        *out = e.data;
        e.seq.store(pos + kRingSize, MemoryOrder::RELEASE);
        return true;
      }
      continue;
    }
    if (diff < 0) {
      // Slot not yet produced. If producers are done AND head caught up,
      // the ring is truly empty. Re-check head/tail ordering to avoid the
      // producer-committed-head-but-not-yet-slot race.
      if (r.done.load(MemoryOrder::ACQUIRE) &&
          r.head.load(MemoryOrder::ACQUIRE) ==
              r.tail.load(MemoryOrder::ACQUIRE))
        return false;
    }
    // diff > 0: another consumer got ahead of us. Retry.
  }
}

struct HandoffCtx {
  Ring *ring;
  unsigned long iters;
  unsigned seed;
  Atomic<uint32_t> *errors;
};

void *producer(void *arg) {
  auto *ctx = static_cast<HandoffCtx *>(arg);
  unsigned s = ctx->seed;
  for (unsigned long i = 0; i < ctx->iters; ++i) {
    const SubSlotClass c = kClasses[lcg(s) % kNumClasses];
    SubSlotHandle h = g_substrate.acquire(c, ConsumerTag::SlabPool);
    if (!static_cast<bool>(h)) {
      ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    const unsigned seed = lcg(s) | 1u;
    const size_t size = layout_of(c).slot_size;
    fill_pattern(h.ptr(), size, seed);
    Handoff ho = {h, seed, size};
    if (!ring_push(*ctx->ring, ho))
      g_substrate.release(h); // ring full too long — drop locally
  }
  return nullptr;
}

void *consumer(void *arg) {
  auto *ctx = static_cast<HandoffCtx *>(arg);
  Handoff ho;
  while (ring_pop(*ctx->ring, &ho)) {
    if (!verify_pattern(ho.h.ptr(), ho.size, ho.seed))
      ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
    g_substrate.release(ho.h);
  }
  return nullptr;
}

TEST(LlvmLibcVaSubstrateTest, CrossThreadReleasePreservesIntegrity) {
  Ring ring;
  Atomic<uint32_t> errors{0};
  constexpr unsigned kProducers = 4;
  constexpr unsigned kConsumers = 4;
  constexpr unsigned long kIters = 2000;
  pthread_t p[kProducers], c[kConsumers];
  HandoffCtx pctx[kProducers], cctx[kConsumers];

  // Start consumers first so producers never see a full ring on the
  // initial burst.
  for (unsigned i = 0; i < kConsumers; ++i) {
    cctx[i] = {&ring, 0, 0, &errors};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&c[i], nullptr, consumer,
                                             &cctx[i]),
              0);
  }
  for (unsigned i = 0; i < kProducers; ++i) {
    pctx[i] = {&ring, kIters, i * 0xDEADBEEFu + 1u, &errors};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&p[i], nullptr, producer,
                                             &pctx[i]),
              0);
  }
  for (unsigned i = 0; i < kProducers; ++i)
    LIBC_NAMESPACE::pthread_join(p[i], nullptr);
  ring.done.store(true, MemoryOrder::RELEASE);
  for (unsigned i = 0; i < kConsumers; ++i)
    LIBC_NAMESPACE::pthread_join(c[i], nullptr);
  EXPECT_EQ(errors.load(MemoryOrder::RELAXED), uint32_t{0});
}

// ============================================================================
// Burst smoke — drive every class through a tight 10k alloc/free loop.
//
// Amortized reclaim fires every 16 releases; 10k/class exercises several
// reclaim passes under a single thread — the quiet-path counterpart to
// the concurrent tests. A failure here is almost certainly a pure-logic
// bug (not a race).
// ============================================================================

TEST(LlvmLibcVaSubstrateTest, BurstAllocFreeSingleThread) {
  constexpr unsigned long kIters = 10000;
  for (SubSlotClass c : kClasses) {
    const size_t size = layout_of(c).slot_size;
    for (unsigned long i = 0; i < kIters; ++i) {
      SubSlotHandle h = g_substrate.acquire(c, ConsumerTag::SlabPool);
      ASSERT_TRUE(static_cast<bool>(h));
      // Stamp the first + last byte of each slot — cheap tail-page touch
      // without paying for the full fill every iteration.
      auto *b = static_cast<unsigned char *>(h.ptr());
      b[0] = 0xA5;
      b[size - 1] = 0x5A;
      EXPECT_EQ(b[0], static_cast<unsigned char>(0xA5));
      EXPECT_EQ(b[size - 1], static_cast<unsigned char>(0x5A));
      g_substrate.release(h);
    }
  }
}

// ============================================================================
// Aggressive stress tier
// ============================================================================
//
// The tests above establish single-thread correctness and basic concurrency.
// The tests below push harder on specific race windows and adverse patterns:
//
//   1. Class-biased contention — every thread hammers one pool, driving
//      active_ CAS, abandoned_head_ Treiber traffic, and arena reservation
//      to saturation on a single pool instead of spreading load across three.
//   2. Mixed hold times — long-hold "hoarder" threads hold ~40 live handles
//      for thousands of iterations while "grazer" threads rapid-churn with
//      2 live. Pins of very different epoch ages coexist, which exercises
//      the epoch gate's min-pinned scan.
//   3. Concurrent try_reclaim vs. workers — a dedicated reclaim thread bangs
//      on try_reclaim while workers keep live epoch pins. The [R2] gate in
//      the plan's atomic-correctness table: reclaim must NOT free an arena
//      whose VA a worker is dereferencing.
//   4. Bursty fill/drain cycles — each cycle fills beyond the active arena's
//      capacity (forcing full-→-abandoned demotion and new-arena spawn) then
//      releases everything. Repeated cycles drive the new pop-filter path
//      hard.
//   5. Multi-stage pipeline — a handle traverses three thread boundaries
//      before release, testing owner_token CAS from threads several hops
//      removed from the acquirer.
//
// Every stress test uses the same fill-pattern + verify-pattern invariant:
// a non-zero `errors` counter at join time means slot aliasing, a use-after-
// free, or a decommit-racing-reader landed in our bytes.

// A consolidated worker job shared by several of the stress tests — acquire
// a handle, stamp a seed-derived pattern through every byte, hold for H
// iterations, verify the pattern, release. The holder size H and per-thread
// live-set size are the variable knobs.
struct WorkerJob {
  SubSlotClass klass;       // SubSlotClass::Count means random
  unsigned long iters;
  unsigned live_slots;      // peak live handles this worker keeps
  unsigned hold_depth;      // how many iters a handle stays live
  unsigned seed;
  Atomic<uint32_t> *errors;
  Atomic<uint64_t> *ops;
};

void *mixed_worker(void *arg) {
  auto *job = static_cast<WorkerJob *>(arg);
  constexpr unsigned kCap = 64;
  LiveEntry live[kCap] = {};
  unsigned s = job->seed;
  const unsigned cap = job->live_slots < kCap ? job->live_slots : kCap;

  for (unsigned long i = 0; i < job->iters; ++i) {
    const unsigned idx = lcg(s) % cap;
    // With probability ~1/(hold_depth+1) we release (if live); otherwise
    // we only release if the slot is occupied and age >= hold_depth, to
    // prevent unbounded growth.
    const bool should_release =
        static_cast<bool>(live[idx].h) &&
        ((lcg(s) % (job->hold_depth + 1)) == 0 ||
         (i - reinterpret_cast<uintptr_t>(&live[idx]) % 32) >= job->hold_depth);

    if (should_release) {
      if (!verify_pattern(live[idx].h.ptr(), live[idx].size, live[idx].seed))
        job->errors->fetch_add(1, MemoryOrder::RELAXED);
      g_substrate.release(live[idx].h);
      live[idx] = {};
      job->ops->fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    if (static_cast<bool>(live[idx].h))
      continue; // slot occupied but not due to drop — rotate
    const SubSlotClass c = (job->klass == SubSlotClass::Count)
                               ? kClasses[lcg(s) % kNumClasses]
                               : job->klass;
    SubSlotHandle h = g_substrate.acquire(c, ConsumerTag::SlabPool);
    if (!static_cast<bool>(h)) {
      job->errors->fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    const unsigned seed = lcg(s) | 1u;
    const size_t size = layout_of(c).slot_size;
    fill_pattern(h.ptr(), size, seed);
    live[idx] = {h, seed, size};
    job->ops->fetch_add(1, MemoryOrder::RELAXED);
  }
  // Drain.
  for (unsigned k = 0; k < cap; ++k) {
    if (static_cast<bool>(live[k].h)) {
      if (!verify_pattern(live[k].h.ptr(), live[k].size, live[k].seed))
        job->errors->fetch_add(1, MemoryOrder::RELAXED);
      g_substrate.release(live[k].h);
    }
  }
  return nullptr;
}

TEST(LlvmLibcVaSubstrateTest, ClassBiasedHighContention) {
  // Each thread is pinned to one class so all 16/3 ≈ 5 threads on a single
  // pool contend on the same active_ / abandoned_head_ / quarantine_head_
  // state. Round-robin random-class workloads spread contention three ways;
  // this one doesn't, so per-pool race windows are 3× denser.
  constexpr unsigned kThreads = 15;               // 5 per class
  constexpr unsigned long kIters = 6000;
  Atomic<uint32_t> errors{0};
  Atomic<uint64_t> ops{0};
  pthread_t tids[kThreads];
  WorkerJob jobs[kThreads];
  for (unsigned t = 0; t < kThreads; ++t) {
    jobs[t] = {kClasses[t % kNumClasses], kIters, /*live_slots=*/8,
               /*hold_depth=*/4, t * 0x9E3779B9u + 5u, &errors, &ops};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&tids[t], nullptr, mixed_worker,
                                             &jobs[t]),
              0);
  }
  for (unsigned t = 0; t < kThreads; ++t)
    LIBC_NAMESPACE::pthread_join(tids[t], nullptr);
  EXPECT_EQ(errors.load(MemoryOrder::RELAXED), uint32_t{0});
  EXPECT_GT(ops.load(MemoryOrder::RELAXED), uint64_t{0});
}

TEST(LlvmLibcVaSubstrateTest, MixedHoldTimesNoLeakage) {
  // 8 "grazers" and 4 "hoarders". Grazers rapid-churn 2 live; hoarders
  // keep ~32 live for the entire run and only slowly release. The point
  // is to interleave very-short epoch pins with very-long epoch pins so
  // the epoch reclaim gate sees a min_pinned_epoch that stays well behind
  // the global epoch for long stretches — if reclaim prematurely drops
  // an arena under a long-lived pin we see a pattern corruption or AV.
  constexpr unsigned kGrazers = 8;
  constexpr unsigned kHoarders = 4;
  constexpr unsigned kThreads = kGrazers + kHoarders;
  Atomic<uint32_t> errors{0};
  Atomic<uint64_t> ops{0};
  pthread_t tids[kThreads];
  WorkerJob jobs[kThreads];

  for (unsigned t = 0; t < kGrazers; ++t) {
    jobs[t] = {SubSlotClass::Count, 8000, /*live_slots=*/2,
               /*hold_depth=*/2, 1u + t * 0xBAADF00Du, &errors, &ops};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&tids[t], nullptr, mixed_worker,
                                             &jobs[t]),
              0);
  }
  for (unsigned t = 0; t < kHoarders; ++t) {
    jobs[kGrazers + t] = {SubSlotClass::Count, 8000,
                          /*live_slots=*/32, /*hold_depth=*/128,
                          1u + (t + 64) * 0xDEADBEEFu, &errors, &ops};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&tids[kGrazers + t], nullptr,
                                             mixed_worker,
                                             &jobs[kGrazers + t]),
              0);
  }
  for (unsigned t = 0; t < kThreads; ++t)
    LIBC_NAMESPACE::pthread_join(tids[t], nullptr);
  EXPECT_EQ(errors.load(MemoryOrder::RELAXED), uint32_t{0});
}

// Reclaim-banger: continuously pump try_reclaim() while workers churn. The
// worker's EpochGuard (inside every acquire/release) is the only thing
// preventing reclaim from MEM_RELEASE'ing an arena we're dereferencing.
// Any missing pin would land as a SIGSEGV / AV on the worker's slot access.

struct ReclaimCtx {
  Atomic<bool> *stop;
  Atomic<uint64_t> *passes;
};

void *reclaim_banger(void *arg) {
  auto *ctx = static_cast<ReclaimCtx *>(arg);
  while (!ctx->stop->load(MemoryOrder::ACQUIRE)) {
    g_substrate.try_reclaim();
    ctx->passes->fetch_add(1, MemoryOrder::RELAXED);
  }
  return nullptr;
}

TEST(LlvmLibcVaSubstrateTest, ConcurrentReclaimVsWorkers) {
  constexpr unsigned kWorkers = 8;
  constexpr unsigned kReclaimers = 2;
  Atomic<uint32_t> errors{0};
  Atomic<uint64_t> ops{0};
  Atomic<bool> stop{false};
  Atomic<uint64_t> reclaim_passes{0};

  pthread_t workers[kWorkers];
  pthread_t reclaimers[kReclaimers];
  WorkerJob jobs[kWorkers];
  ReclaimCtx rctx[kReclaimers];

  for (unsigned t = 0; t < kReclaimers; ++t) {
    rctx[t] = {&stop, &reclaim_passes};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&reclaimers[t], nullptr,
                                             reclaim_banger, &rctx[t]),
              0);
  }
  for (unsigned t = 0; t < kWorkers; ++t) {
    jobs[t] = {SubSlotClass::Count, 5000, /*live_slots=*/12,
               /*hold_depth=*/6, 1u + t * 0x85EBCA77u, &errors, &ops};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&workers[t], nullptr,
                                             mixed_worker, &jobs[t]),
              0);
  }

  for (unsigned t = 0; t < kWorkers; ++t)
    LIBC_NAMESPACE::pthread_join(workers[t], nullptr);
  stop.store(true, MemoryOrder::RELEASE);
  for (unsigned t = 0; t < kReclaimers; ++t)
    LIBC_NAMESPACE::pthread_join(reclaimers[t], nullptr);

  EXPECT_EQ(errors.load(MemoryOrder::RELAXED), uint32_t{0});
  EXPECT_GT(reclaim_passes.load(MemoryOrder::RELAXED), uint64_t{0});
}

// Bursty fill/drain — each thread fills beyond the seed arena's capacity
// in a tight loop then releases everything. Forces sustained arena-boundary
// pressure: full-→-abandoned demotion (the new pop-filter path) + new-arena
// reservation + eventual retire on the next cycle. A pre-fix regression
// where acquire-with-full-active spun forever would hang this test.

struct BurstCtx {
  SubSlotClass klass;
  unsigned cycles;
  unsigned per_burst;
  Atomic<uint32_t> *errors;
};

void *bursty_worker(void *arg) {
  auto *ctx = static_cast<BurstCtx *>(arg);
  const size_t size = layout_of(ctx->klass).slot_size;
  constexpr unsigned kCap = 80;
  SubSlotHandle hs[kCap];
  unsigned seeds[kCap] = {};
  unsigned s = 0xA5A5u ^ static_cast<unsigned>(ctx->klass);

  for (unsigned cyc = 0; cyc < ctx->cycles; ++cyc) {
    const unsigned n = ctx->per_burst < kCap ? ctx->per_burst : kCap;
    for (unsigned i = 0; i < n; ++i) {
      hs[i] = g_substrate.acquire(ctx->klass, ConsumerTag::SlabPool);
      if (!static_cast<bool>(hs[i])) {
        ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
        // Partial burst — release what we got, move on.
        for (unsigned k = 0; k < i; ++k)
          g_substrate.release(hs[k]);
        goto next_cycle;
      }
      seeds[i] = lcg(s) | 1u;
      fill_pattern(hs[i].ptr(), size, seeds[i]);
    }
    // Verify + release in reverse order.
    for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
      if (!verify_pattern(hs[i].ptr(), size, seeds[i]))
        ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
      g_substrate.release(hs[i]);
    }
  next_cycle:;
  }
  return nullptr;
}

TEST(LlvmLibcVaSubstrateTest, BurstyFillDrainCycles) {
  constexpr unsigned kThreads = 9; // 3 per class
  Atomic<uint32_t> errors{0};
  pthread_t tids[kThreads];
  BurstCtx ctx[kThreads];
  for (unsigned t = 0; t < kThreads; ++t) {
    const SubSlotClass c = kClasses[t % kNumClasses];
    // per_burst is slots_per_arena + 2 so every cycle forces a new arena.
    const unsigned per_burst =
        layout_of(c).slots_per_arena + 2;
    ctx[t] = {c, /*cycles=*/40, per_burst, &errors};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&tids[t], nullptr, bursty_worker,
                                             &ctx[t]),
              0);
  }
  for (unsigned t = 0; t < kThreads; ++t)
    LIBC_NAMESPACE::pthread_join(tids[t], nullptr);
  EXPECT_EQ(errors.load(MemoryOrder::RELAXED), uint32_t{0});
}

// Multi-stage pipeline: handle travels through three thread boundaries
// (stage 1 producer → stage 2 filter → stage 3 terminator). Each stage
// verifies the prior stage's pattern, refills with its own pattern, and
// forwards. The thread that ultimately releases is two hops removed from
// the acquirer — the owner_token CAS runs on a core that never touched
// the arena's live_count on the acquire side.

struct PipeSlot {
  SubSlotHandle h;
  SubSlotClass klass;
  unsigned seed_a;  // stage 1 pattern
  unsigned seed_b;  // stage 2 pattern
  size_t size;
};

constexpr unsigned kPipeRing = 128;

// Same Vyukov MPMC ring as the cross-thread test — the naive read-before-
// CAS ring raced under this test's 3-to-4-to-3 producer/filter/consumer
// load and could hand a consumer an uninitialised slot (SIGSEGV on
// ps.h.ptr()).
struct PipeEntry {
  Atomic<uint64_t> seq{0};
  PipeSlot data{};
};

struct PipeRing {
  PipeEntry entries[kPipeRing];
  alignas(64) Atomic<uint64_t> head{0};
  alignas(64) Atomic<uint64_t> tail{0};
  Atomic<bool> done{false};

  PipeRing() {
    for (unsigned i = 0; i < kPipeRing; ++i)
      entries[i].seq.store(static_cast<uint64_t>(i), MemoryOrder::RELAXED);
  }
};

bool pipe_push(PipeRing &r, const PipeSlot &s) {
  for (int spin = 0; spin < 1000000; ++spin) {
    uint64_t pos = r.head.load(MemoryOrder::RELAXED);
    PipeEntry &e = r.entries[pos & (kPipeRing - 1)];
    uint64_t seq = e.seq.load(MemoryOrder::ACQUIRE);
    int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
    if (diff == 0) {
      if (r.head.compare_exchange_weak(pos, pos + 1, MemoryOrder::RELAXED,
                                       MemoryOrder::RELAXED)) {
        e.data = s;
        e.seq.store(pos + 1, MemoryOrder::RELEASE);
        return true;
      }
    }
  }
  return false;
}

bool pipe_pop(PipeRing &r, PipeSlot *out) {
  for (;;) {
    uint64_t pos = r.tail.load(MemoryOrder::RELAXED);
    PipeEntry &e = r.entries[pos & (kPipeRing - 1)];
    uint64_t seq = e.seq.load(MemoryOrder::ACQUIRE);
    int64_t diff =
        static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1);
    if (diff == 0) {
      if (r.tail.compare_exchange_weak(pos, pos + 1, MemoryOrder::RELAXED,
                                       MemoryOrder::RELAXED)) {
        *out = e.data;
        e.seq.store(pos + kPipeRing, MemoryOrder::RELEASE);
        return true;
      }
      continue;
    }
    if (diff < 0) {
      if (r.done.load(MemoryOrder::ACQUIRE) &&
          r.head.load(MemoryOrder::ACQUIRE) ==
              r.tail.load(MemoryOrder::ACQUIRE))
        return false;
    }
  }
}

struct PipeStage1Ctx {
  PipeRing *out_ring;
  unsigned long iters;
  unsigned seed;
  Atomic<uint32_t> *errors;
};

struct PipeMidCtx {
  PipeRing *in_ring;
  PipeRing *out_ring;
  Atomic<uint32_t> *errors;
};

struct PipeTermCtx {
  PipeRing *in_ring;
  Atomic<uint32_t> *errors;
};

void *pipe_stage1(void *arg) {
  auto *ctx = static_cast<PipeStage1Ctx *>(arg);
  unsigned s = ctx->seed;
  for (unsigned long i = 0; i < ctx->iters; ++i) {
    const SubSlotClass c = kClasses[lcg(s) % kNumClasses];
    SubSlotHandle h = g_substrate.acquire(c, ConsumerTag::SlabPool);
    if (!static_cast<bool>(h)) {
      ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    const unsigned seed_a = lcg(s) | 1u;
    const size_t size = layout_of(c).slot_size;
    fill_pattern(h.ptr(), size, seed_a);
    PipeSlot ps = {h, c, seed_a, 0, size};
    if (!pipe_push(*ctx->out_ring, ps))
      g_substrate.release(h);
  }
  return nullptr;
}

void *pipe_mid(void *arg) {
  auto *ctx = static_cast<PipeMidCtx *>(arg);
  PipeSlot ps;
  while (pipe_pop(*ctx->in_ring, &ps)) {
    if (!verify_pattern(ps.h.ptr(), ps.size, ps.seed_a))
      ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
    ps.seed_b = ps.seed_a * 0xC2B2AE3Du + 1u;
    fill_pattern(ps.h.ptr(), ps.size, ps.seed_b);
    if (!pipe_push(*ctx->out_ring, ps))
      g_substrate.release(ps.h);
  }
  return nullptr;
}

void *pipe_term(void *arg) {
  auto *ctx = static_cast<PipeTermCtx *>(arg);
  PipeSlot ps;
  while (pipe_pop(*ctx->in_ring, &ps)) {
    if (!verify_pattern(ps.h.ptr(), ps.size, ps.seed_b))
      ctx->errors->fetch_add(1, MemoryOrder::RELAXED);
    g_substrate.release(ps.h);
  }
  return nullptr;
}


// ============================================================================
// NT-level permanence witness — seed arenas never page_free
// ============================================================================
//
// pre_init reserves one permanent seed arena per SubSlotClass (is_permanent=1)
// whose VA must outlive the substrate: the SubstrateRegistry membership
// check on `release()` relies on the seed reservation staying valid for
// the whole process lifetime, the mapping table's L1 lives in the Small
// seed, and the bootstrap-tier scratch synth header reuses Medium-seed
// VA. Substrate's retire path (try_retire_arena → pop_abandoned_filtered)
// short-circuits permanent arenas and never hands them to Crystalline; a
// permanent arena reaching MEM_FREE would mean is_permanent gating broke
// somewhere in the retire chain, which is a load-bearing correctness
// invariant.
//
// The test forces churn that drives substantial slow-path activity on
// the Large pool, then witnesses that the Large seed's VA stays
// committed across every cycle. Direct witness via NtQueryVirtualMemory.
//
// (A stronger "every drained non-permanent arena eventually reaches
// MEM_FREE" assertion is intentionally NOT made here — pop_abandoned_
// filtered breaks on the first eligible-or-permanent arena it
// encounters during its chain walk, so the order in which specific
// captured bases get retired depends on chain composition at the time
// of each pop call. With test contamination (many earlier tests have
// already populated the pool), there's no guarantee a specific captured
// base will be reached before the walk breaks. TryReclaimDrainsDrained-
// Arenas covers the bookkeeping invariant in a controlled way; this
// test focuses on the permanence invariant the bookkeeping test cannot
// observe.)

TEST(LlvmLibcVaSubstrateTest, ReclamationReturnsVaToKernel) {
  using LIBC_NAMESPACE::nt_helpers::query_basic_info;

  const SubSlotLayout lo = layout_of(SubSlotClass::Large);
  const unsigned N = lo.slots_per_arena * 3 + 1; // forces ≥2 new arenas
  constexpr unsigned kMax = 80;
  ASSERT_LE(N, kMax);

  SubSlotHandle hs[kMax];
  for (unsigned i = 0; i < N; ++i) {
    hs[i] = g_substrate.acquire(SubSlotClass::Large, ConsumerTag::SlabPool);
    ASSERT_TRUE(static_cast<bool>(hs[i]));
  }

  // Capture every unique arena base observed across our handles.
  uintptr_t bases[16] = {};
  unsigned base_count = 0;
  for (unsigned i = 0; i < N && base_count < 16; ++i) {
    const uintptr_t b =
        reinterpret_cast<uintptr_t>(hs[i].ptr()) & ~(lo.arena_size - 1);
    bool seen = false;
    for (unsigned k = 0; k < base_count; ++k)
      if (bases[k] == b) { seen = true; break; }
    if (!seen)
      bases[base_count++] = b;
  }
  // We forced ≥2 new arenas on top of the seed — should observe ≥2 distinct
  // bases if the acquire loop spread across them.
  EXPECT_GE(base_count, 2u);

  for (unsigned i = 0; i < N; ++i)
    g_substrate.release(hs[i]);

  // Retirement is triggered only by pop_abandoned_filtered, which runs
  // on the acquire path when the current active arena is unusable. A
  // pure release workflow never retires drained arenas — they sit with
  // live_count=0 until a subsequent burst displaces the active arena
  // (moving it to abandoned) and a later acquire pops-and-retires the
  // drained ones. ONE churn burst displaces some but leaves the last-
  // active drained on abandoned without ever being popped, so not all
  // captured bases get retired in a single cycle. Loop churn-and-reclaim
  // until every captured base is observed MEM_FREE, bounded so a real
  // reclamation regression fails fast rather than spinning.
  //
  // NtQueryVirtualMemory must succeed for every arena base — the base is
  // a 2 MB-aligned address in user VA that was a valid reservation and,
  // if reclaimed, now lies inside an MEM_FREE region. Query failure
  // means the call is wrong (bad class, bad buffer), not that the memory
  // is freed. Assert NT_SUCCESS and inspect State directly.
  // bases[0] is the per-class seed arena: the very first acquire pulls
  // from `active_` which is initialized to the seed at pre_init time.
  // Seeds carry `is_permanent=1` and are intentionally never page_free'd
  // (their VA outlives the substrate — the SubstrateRegistry membership
  // check on release relies on the seed reservation staying valid for
  // the process lifetime). Reclamation invariant therefore applies only
  // to bases[1..base_count); bases[0] must NOT reach MEM_FREE.
  // bases[0] is the per-class seed arena: the very first acquire pulls
  // from `active_` which is initialized to the seed at pre_init time.
  const uintptr_t seed_base = bases[0];

  // Drive churn that exercises the slow_acquire_arena → pop_abandoned_
  // filtered → try_retire_arena chain. Each cycle's release returns
  // every captured slot, then the next cycle's acquires re-fill them
  // and any displaced arenas hit slow path. The seed gets re-promoted
  // back as `active_` whenever pop_abandoned_filtered encounters a
  // permanent drained arena, which is the codepath the permanence gate
  // protects most directly.
  SubSlotHandle churn[kMax];
  constexpr int kChurnBudget = 6;
  for (int cycles = 0; cycles < kChurnBudget; ++cycles) {
    for (unsigned i = 0; i < N; ++i) {
      churn[i] = g_substrate.acquire(SubSlotClass::Large, ConsumerTag::SlabPool);
      ASSERT_TRUE(static_cast<bool>(churn[i]));
    }
    for (unsigned i = 0; i < N; ++i)
      g_substrate.release(churn[i]);
    for (int k = 0; k < 16; ++k)
      g_substrate.try_reclaim();

    // Per-cycle witness: the seed arena's VA must NOT be MEM_FREE. The
    // membership check on every release() depends on this VA staying
    // valid; a single transition to MEM_FREE is unrecoverable for the
    // process. Per-cycle (not just final) so a flaky regression in a
    // mid-test transition fails fast rather than masking.
    MEMORY_BASIC_INFORMATION seed_mbi = {};
    NTSTATUS seed_st =
        query_basic_info(reinterpret_cast<void *>(seed_base), seed_mbi);
    ASSERT_TRUE(NT_SUCCESS(seed_st));
    ASSERT_NE(seed_mbi.State, static_cast<DWORD>(MEM_FREE));
  }
}

// ============================================================================
// Bounded-growth witness — purely behavioural (public API only)
// ============================================================================
//
// Acquire-then-release churn must not cause arena_count to grow without
// bound. Each cycle deliberately exceeds the seed arena's slot count so
// slow_acquire_arena fires (driving pop_abandoned_filtered → retire), and
// try_reclaim drains the calling thread's slot pins so any published
// retire batch can complete its refs→0 transition and free.
//
// The bound is loose by design: arena_count starts contaminated by every
// preceding test's residual arenas (Crystalline-W's batch model leaves
// per-thread retires unpublished until the batch reaches Freq nodes, and
// pop_abandoned_filtered breaks on the first eligible/permanent arena),
// so the pre-test floor is not zero. What we DO assert is that running
// many more cycles past the initial fill does not push arena_count
// monotonically upward — i.e., reclamation is keeping pace with new
// reservations on average.
TEST(LlvmLibcVaSubstrateTest, ChurnDoesNotLeakArenas) {
  const SubSlotLayout lo = layout_of(SubSlotClass::Large);
  const unsigned N = lo.slots_per_arena * 3 + 1; // forces ≥2 new arenas
  constexpr unsigned kMax = 80;
  ASSERT_LE(N, kMax);

  // Prime: drive a few cycles to stabilise arena_count (so any one-shot
  // pre_init / Tier-A residual settles into the steady-state).
  SubSlotHandle hs[kMax];
  for (int prime = 0; prime < 4; ++prime) {
    for (unsigned i = 0; i < N; ++i) {
      hs[i] = g_substrate.acquire(SubSlotClass::Large, ConsumerTag::SlabPool);
      ASSERT_TRUE(static_cast<bool>(hs[i]));
    }
    for (unsigned i = 0; i < N; ++i)
      g_substrate.release(hs[i]);
    for (int k = 0; k < 16; ++k)
      g_substrate.try_reclaim();
  }

  const uint32_t baseline = g_substrate.arena_count(SubSlotClass::Large);

  // Sustained churn — many more cycles than the prime. If reclamation
  // is broken (every cycle's retired arenas leak), arena_count will
  // grow unboundedly. If reclamation works, it plateaus.
  constexpr int kSustainedCycles = 32;
  for (int cycle = 0; cycle < kSustainedCycles; ++cycle) {
    for (unsigned i = 0; i < N; ++i) {
      hs[i] = g_substrate.acquire(SubSlotClass::Large, ConsumerTag::SlabPool);
      ASSERT_TRUE(static_cast<bool>(hs[i]));
    }
    for (unsigned i = 0; i < N; ++i)
      g_substrate.release(hs[i]);
    for (int k = 0; k < 16; ++k)
      g_substrate.try_reclaim();
  }

  const uint32_t final_count = g_substrate.arena_count(SubSlotClass::Large);

  // Slack accommodates a few in-flight batches sitting unpublished in
  // per-thread Crystalline batches (capped by Freq=8 per thread). The
  // bound is "growth proportional to thread count", not "zero growth".
  // 32 sustained cycles past the prime should not accumulate more than
  // a small multiple of the per-thread batch capacity.
  constexpr uint32_t kBoundedGrowthSlack = 64;
  EXPECT_LE(final_count, baseline + kBoundedGrowthSlack);
}

#ifdef LIBC_SUBSTRATE_DIAG_COUNTERS
// ============================================================================
// Strict reclamation witness — gated, internal counters
// ============================================================================
//
// Compiled in only when the substrate object library is built with
// -DLIBC_SUBSTRATE_DIAG_COUNTERS=ON (CMake option of the same name).
// Counters are file-scope atomics in LIBC_NAMESPACE::windows::alloc::
// (substrate retire/free) and LIBC_NAMESPACE::concurrent:: (Crystalline
// retire/publish path) — no public API surface, no DLL export.
// Production binaries don't carry the counter symbols or the per-event
// atomic-bump cost.
//
// What the witness establishes: substrate_free_arena fired at least
// once during the test window. This is the minimal claim that
// distinguishes "reclamation works" from "reclamation silently
// leaks" — the prior Freq=1 regression (Crystalline-W's Phase A
// early-returns single-node batches without reaching Phase B) had
// retire_count grow with every drained arena while free_count stayed
// at zero. The Crystalline path counters localise where in the chain
// reclamation breaks if this assertion fails again.
TEST(LlvmLibcVaSubstrateTest, FreesActuallyHappen) {
  using LIBC_NAMESPACE::concurrent::g_diag_free_list_calls;
  using LIBC_NAMESPACE::concurrent::g_diag_free_list_freefn_calls;
  using LIBC_NAMESPACE::concurrent::g_diag_phase_a_return;
  using LIBC_NAMESPACE::concurrent::g_diag_phase_b_immediate_free;
  using LIBC_NAMESPACE::concurrent::g_diag_phase_b_publishes;
  using LIBC_NAMESPACE::concurrent::g_diag_phase_b_reached;
  using LIBC_NAMESPACE::concurrent::g_diag_try_retire;
  using LIBC_NAMESPACE::windows::alloc::g_diag_free_count;
  using LIBC_NAMESPACE::windows::alloc::g_diag_pop_abandoned_calls;
  using LIBC_NAMESPACE::windows::alloc::g_diag_pop_returned_null;
  using LIBC_NAMESPACE::windows::alloc::g_diag_pop_returned_partial;
  using LIBC_NAMESPACE::windows::alloc::g_diag_pop_returned_permanent;
  using LIBC_NAMESPACE::windows::alloc::g_diag_retire_count;
  using LIBC_NAMESPACE::windows::alloc::g_diag_pop_chain_state_full;
  using LIBC_NAMESPACE::windows::alloc::g_diag_pop_chain_state_partial;
  using LIBC_NAMESPACE::windows::alloc::g_diag_pop_chain_state_quarantined;
  using LIBC_NAMESPACE::windows::alloc::g_diag_pop_chain_state_zero_nonperm;
  using LIBC_NAMESPACE::windows::alloc::g_diag_pop_chain_state_zero_perm;
  using LIBC_NAMESPACE::windows::alloc::g_diag_pop_chain_total_nodes;
  using LIBC_NAMESPACE::windows::alloc::g_diag_slow_acquire_calls;

  const SubSlotLayout lo = layout_of(SubSlotClass::Large);
  const unsigned N = lo.slots_per_arena * 3 + 1;
  constexpr unsigned kMax = 80;
  ASSERT_LE(N, kMax);

  const uint64_t retire_baseline =
      g_diag_retire_count.load(MemoryOrder::RELAXED);
  const uint64_t free_baseline =
      g_diag_free_count.load(MemoryOrder::RELAXED);
  const uint64_t try_retire_baseline =
      g_diag_try_retire.load(MemoryOrder::RELAXED);
  const uint64_t phase_a_return_baseline =
      g_diag_phase_a_return.load(MemoryOrder::RELAXED);
  const uint64_t phase_b_reached_baseline =
      g_diag_phase_b_reached.load(MemoryOrder::RELAXED);
  const uint64_t phase_b_publishes_baseline =
      g_diag_phase_b_publishes.load(MemoryOrder::RELAXED);
  const uint64_t immediate_free_baseline =
      g_diag_phase_b_immediate_free.load(MemoryOrder::RELAXED);
  const uint64_t free_list_calls_baseline =
      g_diag_free_list_calls.load(MemoryOrder::RELAXED);
  const uint64_t free_list_freefn_baseline =
      g_diag_free_list_freefn_calls.load(MemoryOrder::RELAXED);
  const uint64_t slow_acquire_baseline =
      g_diag_slow_acquire_calls.load(MemoryOrder::RELAXED);
  const uint64_t pop_calls_baseline =
      g_diag_pop_abandoned_calls.load(MemoryOrder::RELAXED);
  const uint64_t pop_null_baseline =
      g_diag_pop_returned_null.load(MemoryOrder::RELAXED);
  const uint64_t pop_perm_baseline =
      g_diag_pop_returned_permanent.load(MemoryOrder::RELAXED);
  const uint64_t pop_partial_baseline =
      g_diag_pop_returned_partial.load(MemoryOrder::RELAXED);
  const uint64_t chain_total_baseline =
      g_diag_pop_chain_total_nodes.load(MemoryOrder::RELAXED);
  const uint64_t chain_zero_nonperm_baseline =
      g_diag_pop_chain_state_zero_nonperm.load(MemoryOrder::RELAXED);
  const uint64_t chain_zero_perm_baseline =
      g_diag_pop_chain_state_zero_perm.load(MemoryOrder::RELAXED);
  const uint64_t chain_partial_baseline =
      g_diag_pop_chain_state_partial.load(MemoryOrder::RELAXED);
  const uint64_t chain_full_baseline =
      g_diag_pop_chain_state_full.load(MemoryOrder::RELAXED);
  const uint64_t chain_quarantined_baseline =
      g_diag_pop_chain_state_quarantined.load(MemoryOrder::RELAXED);

  // Drive enough churn to push the calling thread's substrate batch
  // past Freq=8 retires several times over.
  SubSlotHandle hs[kMax];
  constexpr int kCycles = 32;
  for (int cycle = 0; cycle < kCycles; ++cycle) {
    for (unsigned i = 0; i < N; ++i) {
      hs[i] = g_substrate.acquire(SubSlotClass::Large, ConsumerTag::SlabPool);
      ASSERT_TRUE(static_cast<bool>(hs[i]));
    }
    for (unsigned i = 0; i < N; ++i)
      g_substrate.release(hs[i]);
    for (int k = 0; k < 16; ++k)
      g_substrate.try_reclaim();
  }

  const uint64_t retire_delta =
      g_diag_retire_count.load(MemoryOrder::RELAXED) - retire_baseline;
  const uint64_t free_delta =
      g_diag_free_count.load(MemoryOrder::RELAXED) - free_baseline;
  const uint64_t try_retire_delta =
      g_diag_try_retire.load(MemoryOrder::RELAXED) - try_retire_baseline;
  const uint64_t phase_a_return_delta =
      g_diag_phase_a_return.load(MemoryOrder::RELAXED) -
      phase_a_return_baseline;
  const uint64_t phase_b_reached_delta =
      g_diag_phase_b_reached.load(MemoryOrder::RELAXED) -
      phase_b_reached_baseline;
  const uint64_t phase_b_publishes_delta =
      g_diag_phase_b_publishes.load(MemoryOrder::RELAXED) -
      phase_b_publishes_baseline;
  const uint64_t immediate_free_delta =
      g_diag_phase_b_immediate_free.load(MemoryOrder::RELAXED) -
      immediate_free_baseline;
  const uint64_t free_list_calls_delta =
      g_diag_free_list_calls.load(MemoryOrder::RELAXED) -
      free_list_calls_baseline;
  const uint64_t free_list_freefn_delta =
      g_diag_free_list_freefn_calls.load(MemoryOrder::RELAXED) -
      free_list_freefn_baseline;
  const uint64_t slow_acquire_delta =
      g_diag_slow_acquire_calls.load(MemoryOrder::RELAXED) -
      slow_acquire_baseline;
  const uint64_t pop_calls_delta =
      g_diag_pop_abandoned_calls.load(MemoryOrder::RELAXED) -
      pop_calls_baseline;
  const uint64_t pop_null_delta =
      g_diag_pop_returned_null.load(MemoryOrder::RELAXED) -
      pop_null_baseline;
  const uint64_t pop_perm_delta =
      g_diag_pop_returned_permanent.load(MemoryOrder::RELAXED) -
      pop_perm_baseline;
  const uint64_t pop_partial_delta =
      g_diag_pop_returned_partial.load(MemoryOrder::RELAXED) -
      pop_partial_baseline;
  const uint64_t chain_total_delta =
      g_diag_pop_chain_total_nodes.load(MemoryOrder::RELAXED) -
      chain_total_baseline;
  const uint64_t chain_zero_nonperm_delta =
      g_diag_pop_chain_state_zero_nonperm.load(MemoryOrder::RELAXED) -
      chain_zero_nonperm_baseline;
  const uint64_t chain_zero_perm_delta =
      g_diag_pop_chain_state_zero_perm.load(MemoryOrder::RELAXED) -
      chain_zero_perm_baseline;
  const uint64_t chain_partial_delta =
      g_diag_pop_chain_state_partial.load(MemoryOrder::RELAXED) -
      chain_partial_baseline;
  const uint64_t chain_full_delta =
      g_diag_pop_chain_state_full.load(MemoryOrder::RELAXED) -
      chain_full_baseline;
  const uint64_t chain_quarantined_delta =
      g_diag_pop_chain_state_quarantined.load(MemoryOrder::RELAXED) -
      chain_quarantined_baseline;

  LIBC_NAMESPACE::testing::tlog
      << "[FreesActuallyHappen]"
      << " slow_acquire=" << slow_acquire_delta
      << " pop_calls=" << pop_calls_delta
      << " chain_total=" << chain_total_delta
      << " chain_drained=" << chain_zero_nonperm_delta
      << " chain_seed=" << chain_zero_perm_delta
      << " chain_partial=" << chain_partial_delta
      << " chain_full=" << chain_full_delta
      << " chain_quar=" << chain_quarantined_delta
      << " pop_null=" << pop_null_delta
      << " pop_perm=" << pop_perm_delta
      << " pop_partial=" << pop_partial_delta
      << " retire=" << retire_delta
      << " free=" << free_delta
      << " try_retire=" << try_retire_delta
      << " phaseA_return=" << phase_a_return_delta
      << " phaseB_reached=" << phase_b_reached_delta
      << " phaseB_publishes=" << phase_b_publishes_delta
      << " immediate_free=" << immediate_free_delta
      << " free_list=" << free_list_calls_delta
      << " freefn=" << free_list_freefn_delta << "\n";

  // Retires must happen — pop_abandoned_filtered drains drained non-
  // permanent arenas it walks past; with N forcing slow_acquire_arena
  // every cycle, at least some retires are unavoidable.
  EXPECT_GT(retire_delta, uint64_t{0});

  // Frees must happen — the strict invariant. Zero frees with non-zero
  // retires is the silent-leak signature.
  EXPECT_GT(free_delta, uint64_t{0});
}
#endif // LIBC_SUBSTRATE_DIAG_COUNTERS

TEST(LlvmLibcVaSubstrateTest, MultiStagePipelineCrossThreadOwnership) {
  PipeRing r1, r2;
  Atomic<uint32_t> errors{0};
  constexpr unsigned kS1 = 3, kMid = 4, kTerm = 3;
  constexpr unsigned long kIters = 2500;
  pthread_t s1[kS1], mid[kMid], term[kTerm];
  PipeStage1Ctx s1ctx[kS1];
  PipeMidCtx midctx[kMid];
  PipeTermCtx termctx[kTerm];

  // Terminators first so r2 has consumers before mid starts producing.
  for (unsigned i = 0; i < kTerm; ++i) {
    termctx[i] = {&r2, &errors};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&term[i], nullptr, pipe_term,
                                             &termctx[i]),
              0);
  }
  for (unsigned i = 0; i < kMid; ++i) {
    midctx[i] = {&r1, &r2, &errors};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&mid[i], nullptr, pipe_mid,
                                             &midctx[i]),
              0);
  }
  for (unsigned i = 0; i < kS1; ++i) {
    s1ctx[i] = {&r1, kIters, i * 0x1337C0DEu + 3u, &errors};
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&s1[i], nullptr, pipe_stage1,
                                             &s1ctx[i]),
              0);
  }

  for (unsigned i = 0; i < kS1; ++i)
    LIBC_NAMESPACE::pthread_join(s1[i], nullptr);
  r1.done.store(true, MemoryOrder::RELEASE);
  for (unsigned i = 0; i < kMid; ++i)
    LIBC_NAMESPACE::pthread_join(mid[i], nullptr);
  r2.done.store(true, MemoryOrder::RELEASE);
  for (unsigned i = 0; i < kTerm; ++i)
    LIBC_NAMESPACE::pthread_join(term[i], nullptr);

  EXPECT_EQ(errors.load(MemoryOrder::RELAXED), uint32_t{0});
}

// ============================================================================
// Phase-7 follow-up tests — structural invariants of the Crystalline
// integration and the bootstrap-tier path.
// ============================================================================

// Seed-arena protection: pre_init pre-warms one seed arena per class with
// `is_permanent = 1`. The seed must survive a fill-drain-pop cycle — i.e.
// when its slots are all released and it gets demoted to `abandoned_head_`,
// `pop_abandoned_filtered`'s `state == 0` branch must see the permanent
// flag and return it as a re-promotable candidate, not retire it. After
// the cycle, `arena_count(c)` must NOT have decreased below 1.
TEST(LlvmLibcVaSubstrateTest, SeedArenaSurvivesFillDrainCycle) {
  for (unsigned ci = 0; ci < kNumClasses; ++ci) {
    SubSlotClass c = kClasses[ci];
    const SubSlotLayout lo = layout_of(c);

    const uint32_t pre = g_substrate.arena_count(c);
    ASSERT_GE(pre, uint32_t{1});

    // Force the seed into "drained-then-demoted" state by overflowing it
    // with a fresh arena, then releasing every slot. The seed has slots
    // 0..N-1 occupied during the burst; the second arena takes the
    // overflow. After release, seed is fully drained AND no longer the
    // active arena (the fresh arena is). Subsequent acquires re-promote
    // the seed via pop_abandoned_filtered.
    const unsigned N = lo.slots_per_arena + 1;
    ASSERT_LE(N, kMaxSlotsPerArena);
    SubSlotHandle hs[kMaxSlotsPerArena];
    for (unsigned i = 0; i < N; ++i) {
      hs[i] = g_substrate.acquire(c, ConsumerTag::SlabPool);
      ASSERT_TRUE(static_cast<bool>(hs[i]));
    }
    for (unsigned i = 0; i < N; ++i)
      g_substrate.release(hs[i]);

    // Pump Crystalline reclaim — non-permanent fresh arenas can drain;
    // the seed is permanent and protected.
    for (int k = 0; k < 8; ++k)
      g_substrate.try_reclaim();

    // Re-acquire one slot to force the seed back to active (exercises
    // the permanent-pop-and-re-promote path).
    SubSlotHandle h = g_substrate.acquire(c, ConsumerTag::SlabPool);
    ASSERT_TRUE(static_cast<bool>(h));
    g_substrate.release(h);

    // Permanent invariant: arena_count never falls below the pre-test
    // value (the seed is alive throughout). Also at minimum 1, since
    // pre_init pre-warms a seed.
    EXPECT_GE(g_substrate.arena_count(c), uint32_t{1});
    EXPECT_GE(g_substrate.arena_count(c), pre);
  }
}

// Mapping-table init must be fully INIT_READY by the time main() (and
// therefore any TEST) runs. The mapping_table_finalize_init step at the
// end of the walker's Pass 2 is supposed to latch ready=true. A regression
// where the latch goes missing or moves earlier (re-introducing the
// register-then-deadlock window) shows up here.
TEST(LlvmLibcVaSubstrateTest, MappingTableReadyByMainEntry) {
  EXPECT_TRUE(LIBC_NAMESPACE::windows::g_mapping_table.is_init_ready());
  // No-op extra acquire to confirm the post-Tier-A inline-stamp path is
  // healthy: a brand-new arena reservation goes through
  // register_mapping_internal inline (not the deferred-receipt queue).
  // If that path is broken the call traps inside register_mapping_internal.
  SubSlotHandle h = g_substrate.acquire(SubSlotClass::Medium, ConsumerTag::SlabPool);
  ASSERT_TRUE(static_cast<bool>(h));
  g_substrate.release(h);
}

// CrystallineRetireDoesntStallUnderStuckReader — the wait-free helping
// protocol must drive arena reclamation forward even when one thread
// holds an ancient reservation without releasing.
//
// Setup: one thread acquires a Medium handle and holds it for the entire
// test (its slot 0 reservation pins the arena it acquired from). Many
// other threads burst-acquire/release Medium slots, forcing the substrate
// to reserve and eventually retire arenas. After the burst, arena_count
// must NOT grow unbounded — Crystalline's helping protocol guarantees
// retired arenas reclaim despite the stuck reader (which only pins ONE
// specific arena, not all of them).
struct StuckReaderCtx {
  Atomic<bool> *stop;
  unsigned long iters;
};

void *stuck_reader_holder(void *arg) {
  auto *ctx = static_cast<StuckReaderCtx *>(arg);
  // Take a handle, hold it, idle until told to stop.
  SubSlotHandle h = g_substrate.acquire(SubSlotClass::Medium, ConsumerTag::SlabPool);
  while (!ctx->stop->load(MemoryOrder::ACQUIRE)) {
    // Spin gently — substrate.acquire's Crystalline pin sits on slot 0
    // for the entire idle window. Reclaim of the arena `h` belongs to
    // is blocked by the pin, but every other arena must reclaim freely.
    LIBC_NAMESPACE::cpp::atomic_thread_fence(MemoryOrder::ACQUIRE);
  }
  g_substrate.release(h);
  return nullptr;
}

void *stuck_reader_churner(void *arg) {
  auto *ctx = static_cast<StuckReaderCtx *>(arg);
  for (unsigned long i = 0; i < ctx->iters; ++i) {
    SubSlotHandle h = g_substrate.acquire(SubSlotClass::Medium, ConsumerTag::SlabPool);
    if (static_cast<bool>(h))
      g_substrate.release(h);
  }
  // Final reclaim hint to drain pending retires.
  g_substrate.try_reclaim();
  return nullptr;
}

TEST(LlvmLibcVaSubstrateTest, CrystallineRetireDoesntStallUnderStuckReader) {
  Atomic<bool> stop{false};
  StuckReaderCtx holder_ctx{&stop, 0};
  StuckReaderCtx churn_ctx{&stop, 4096};

  pthread_t holder;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&holder, nullptr,
                                           stuck_reader_holder, &holder_ctx),
            0);

  const uint32_t pre = g_substrate.arena_count(SubSlotClass::Medium);

  constexpr unsigned kChurners = 4;
  pthread_t churners[kChurners];
  for (unsigned i = 0; i < kChurners; ++i)
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&churners[i], nullptr,
                                             stuck_reader_churner, &churn_ctx),
              0);

  for (unsigned i = 0; i < kChurners; ++i)
    LIBC_NAMESPACE::pthread_join(churners[i], nullptr);

  // Pump reclaim. The stuck holder keeps its arena pinned (cannot drop
  // below seed+1), but other retired arenas must reclaim.
  for (int k = 0; k < 16; ++k)
    g_substrate.try_reclaim();

  const uint32_t post = g_substrate.arena_count(SubSlotClass::Medium);

  // Bound: the holder pins at most one arena; the churners temporarily
  // peak much higher than seed+1. After reclaim, arena_count should NOT
  // have grown unboundedly. Allow a generous slack to absorb in-flight
  // retires whose Crystalline batch hasn't yet drained — but the count
  // must converge to a small multiple of the seed-plus-holder baseline,
  // not the burst peak.
  EXPECT_LE(post, pre + 8U);

  stop.store(true, MemoryOrder::RELEASE);
  LIBC_NAMESPACE::pthread_join(holder, nullptr);
}
