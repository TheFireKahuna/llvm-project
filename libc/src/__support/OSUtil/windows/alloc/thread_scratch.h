//===-- Per-thread demand-commit scratch allocator ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thread-local arena allocator with boundary-tagged blocks, freelist,
// demand-commit, canary validation, high-watermark decommit, and
// zero-on-free. Designed for temporary buffers that must not live on the
// stack (worker threads have small initial commits).
//
// Supports both LIFO (stack-scoped) and non-LIFO (movable, multi-scope)
// allocation patterns via a unified bump+freelist design:
//
//   - LIFO fast path: bump allocate, rewind on free. Identical cost to
//     a pure bump allocator (~7 cycles alloc, ~5 cycles free + memset).
//   - Non-LIFO: freed blocks go to a per-arena freelist. Subsequent
//     allocs reuse freelist blocks (first-fit, ~10 cycles). Enables
//     ScratchAlloc<T> — a movable RAII type for buffers that outlive
//     their allocation scope (e.g., RemapGuard::records_).
//
// Arena geometry (one 64 KB reservation per arena):
//
//   Page 0  [0x0000-0x0FFF]  Control block (ArenaHeader)     committed
//   Page 1  [0x1000-0x1FFF]  Leading guard                   never committed
//   Page 2  [0x2000-0x2FFF]  Data page 0                     committed
//   Page 3  [0x3000-0x3FFF]  Data page 1                     committed
//   Pages 4..14              Data growth                      demand-commit
//   Page 15 [0xF000-0xFFFF]  Trailing guard                  never committed
//
// When a primary arena is exhausted, overflow arenas with identical
// geometry are allocated on demand (SlabPool-style growth). No hard
// cap — the allocator grows transparently with the same guard pages,
// canaries, and freelist recycling in every arena.
//
// Each allocation carries a 16-byte BlockHeader:
//   [4B user_size] [2B flags] [2B pad] [8B canary]
// The canary is seed ^ &header ^ user_size — protects both address and
// size. Verified on every free; mismatch traps immediately.
//
// Security properties:
//   - Per-allocation canary: CSPRNG seed XOR header address XOR size.
//   - Zero-on-free: user data volatile-zeroed before freelist/rewind.
//   - Bump start randomization: CSPRNG-seeded offset within initial page.
//   - Leading + trailing guard pages: MMU-enforced, zero runtime cost.
//   - Uncommitted fault zone between commit watermark and trailing guard.
//   - High-watermark decommit: physical memory returned when bump rewinds.
//   - Thread-exit: entire arena chain (primary + overflow) VA-released.
//   - Fork safety: dead-thread arenas reclaimed, survivor preserved.
//   - Pointer range validation on free: traps on wild pointers.
//
// Depends only on page_alloc.h, page_size.h, teb_tls.h, tls_cleanup.h,
// bcryptprimitives.h. No libc, no mmap, no malloc.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_THREAD_SCRATCH_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_THREAD_SCRATCH_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/primitives/canary_seed.h"
#include "src/__support/OSUtil/windows/alloc/primitives/guarded_region.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_local_state.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives_bootstrap.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/threads/windows/thread_local_word.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// ---------------------------------------------------------------------------
// Geometry constants
// ---------------------------------------------------------------------------

namespace scratch_detail {

// Compile-time geometry constants. Intentionally shadow the runtime
// page_size.h accessors — enables constexpr layout math and static_assert
// validation. Mismatch with the runtime OS values traps in init_tls_slot().
inline constexpr size_t ALLOC_GRANULARITY = 65536;
inline constexpr size_t PAGE_SIZE = 4096;

// Total VA reservation per thread. One allocation granularity unit (64 KB).
inline constexpr size_t RESERVE_SIZE = ALLOC_GRANULARITY;

// Page layout:
//   Page 0:     Control block (committed) — also holds per-domain
//               CrystallineBatch records and per-domain slot indices
//               into the cross-thread CrystallineSlotPool.
//   Page 1:     Leading guard (never committed — isolates control)
//   Pages 2-14: Data (13 pages, 53248 bytes, demand-commit)
//   Page 15:    Trailing guard (never committed — catches forward overflow)
//
// Crystalline per-thread reservation slots no longer live inline in the
// arena — they reside in CrystallineSlotPool (one per CrystallineDomain),
// reachable via `crystalline_slot_idx[domain_id]` on this control block.
// This decoupling lets the entire 64 KB arena `page_free` cleanly on
// thread exit; cross-thread Crystalline walks iterate the pool, not the
// scratch registry, so peer threads cannot fault through a dying
// thread's released VA.
inline constexpr size_t CONTROL_OFFSET = 0;
inline constexpr size_t LEADING_GUARD_OFFSET = PAGE_SIZE;
inline constexpr size_t DATA_OFFSET = LEADING_GUARD_OFFSET + PAGE_SIZE;
inline constexpr size_t TRAILING_GUARD_OFFSET = RESERVE_SIZE - PAGE_SIZE;

// Usable data region: pages 2 through 14 (13 pages, 53 KB).
inline constexpr size_t DATA_CAPACITY = TRAILING_GUARD_OFFSET - DATA_OFFSET;
static_assert(DATA_CAPACITY == 13 * PAGE_SIZE,
              "Data region should be 13 pages (53248 bytes) after the "
              "control / leading-guard / trailing-guard carve-outs");

// Initial commit: control page + 2 data pages.
// Pages committed on creation: 0, 2, 3.
// Pages 1, 15 are NEVER committed (guard pages); pages 4-14 are
// demand-committed by arena_alloc_slow.
inline constexpr size_t INITIAL_DATA_COMMIT = 2 * PAGE_SIZE; // pages 2, 3

// Canary overhead per allocation: 16 bytes (8-byte canary + 8-byte complement).
// Keeps user data 16-byte aligned when the allocation base is 16-byte aligned.
inline constexpr size_t CANARY_OVERHEAD = 16;

// Alignment for all allocations. Sufficient for WCHAR, SSE, and general use.
inline constexpr size_t ALLOC_ALIGN = 16;

// Maximum randomization offset within the first committed data page.
// Uses at most 25% of the first page (1024 bytes / ALLOC_ALIGN = 64 positions).
// This limits waste while providing meaningful spray resistance.
inline constexpr size_t MAX_RANDOM_OFFSET = PAGE_SIZE / 4;
inline constexpr size_t RANDOM_POSITIONS = MAX_RANDOM_OFFSET / ALLOC_ALIGN;

// ---------------------------------------------------------------------------
// GuardedRegion layout for arenas (primary and overflow share the geometry)
// ---------------------------------------------------------------------------
//
// Every byte of the 64 KB reservation is accounted for: control page at
// offset 0 (edge-guarded on the left — see guarded_region.h's bracket rule),
// leading guard, data region, trailing guard. The DATA region is marked
// commit_eager=false because creation only commits INITIAL_DATA_COMMIT
// bytes; the remaining pages grow on demand via arena_alloc_slow. Every
// commit/decommit/release call downstream routes through the GuardedRegion
// static typed API, which asserts kind != GUARD — a guard-page commit is
// unrepresentable at the call site even if offset math drifts in a future
// edit.
inline constexpr alloc_primitives::RegionSpec kArenaEntries[] = {
    // [0] Control page — ArenaHeader + per-arena state + per-domain
    //     CrystallineBatch records. Eager commit.
    {CONTROL_OFFSET, PAGE_SIZE, alloc_primitives::RegionKind::META,
     /*commit_eager=*/true},
    // [1] Leading guard — isolates control from data. Never committed.
    {LEADING_GUARD_OFFSET, PAGE_SIZE, alloc_primitives::RegionKind::GUARD,
     /*commit_eager=*/false},
    // [2] Data region (13 pages). Demand-committed: create_* commits the
    //     first INITIAL_DATA_COMMIT bytes explicitly, arena_alloc_slow
    //     grows the committed window from there.
    {DATA_OFFSET, DATA_CAPACITY, alloc_primitives::RegionKind::DATA,
     /*commit_eager=*/false},
    // [3] Trailing guard — catches forward overflow from data. Never committed.
    {TRAILING_GUARD_OFFSET, PAGE_SIZE, alloc_primitives::RegionKind::GUARD,
     /*commit_eager=*/false},
};
inline constexpr alloc_primitives::Layout kArenaLayout{kArenaEntries,
                                                       /*count=*/4u};
static_assert(kArenaLayout.total_size() == RESERVE_SIZE,
              "thread_scratch arena layout total size must equal RESERVE_SIZE");
static_assert(kArenaLayout.is_valid(),
              "thread_scratch arena layout failed GuardedRegion validation -- "
              "check guard placement, page alignment, or 64 KB rounding");

// Layout-entry indices for the static typed API.
inline constexpr unsigned CONTROL_REGION_IDX = 0;
inline constexpr unsigned DATA_REGION_IDX = 2;
static_assert(kArenaEntries[CONTROL_REGION_IDX].kind ==
                  alloc_primitives::RegionKind::META,
              "CONTROL_REGION_IDX must name the control (META) entry");
static_assert(kArenaEntries[DATA_REGION_IDX].kind ==
                  alloc_primitives::RegionKind::DATA,
              "DATA_REGION_IDX must name the DATA entry");

} // namespace scratch_detail

// ---------------------------------------------------------------------------
// ArenaHeader — shared allocator state for primary and overflow arenas
// ---------------------------------------------------------------------------
//
// Both primary (ThreadScratchState) and overflow arenas embed this struct
// at the start of their control page. scratch_free derives the arena from
// the pointer via (ptr & ~0xFFFF) — same code path for all arenas.

struct ArenaHeader {
  char *data_base;       // Start of data region (page 2 of reservation).
  char *bump;            // Current allocation frontier.
  char *commit_limit;    // End of committed data pages.
  char *data_limit;      // End of data region (start of trailing guard).
  // Per-arena CSPRNG seed for canary derivation. Single-seed variant
  // (alloc_primitives::SingleCanarySeed wraps one uintptr_t) — thread_scratch
  // data never escapes a single thread, so a freelist-forges-canary chain
  // is not in the threat model and one seed suffices. Same storage size
  // and alignment as the old uintptr_t field; ArenaHeader stays 48 bytes.
  alloc_primitives::SingleCanarySeed canary_seed;
  char *free_list_head;  // Singly-linked free block list (nullptr = empty).
};

static_assert(sizeof(ArenaHeader) == 48, "ArenaHeader must be 48 bytes");
static_assert(sizeof(alloc_primitives::SingleCanarySeed) == sizeof(uintptr_t),
              "SingleCanarySeed must match the legacy uintptr_t slot");

// ---------------------------------------------------------------------------
// ScratchOverflowArena — control block for overflow arenas
// ---------------------------------------------------------------------------
//
// When the primary 53KB arena is exhausted, overflow arenas with identical
// 64KB geometry are allocated on demand. Same guard pages, same canaries,
// same freelist recycling — no degraded experience past 53KB.

struct ScratchOverflowArena {
  ArenaHeader arena;           // Same allocator fields as primary.
  ScratchOverflowArena *next;  // Chain link (singly-linked from primary).
  // Live-allocation counter. Incremented by `scratch_alloc` on every
  // allocation served from this arena, decremented by `scratch_free` on
  // every release. When it reaches zero the arena holds no live bytes
  // — neither bump-resident nor freelist-resident — and `scratch_free`
  // splices it from the chain, removes its mapping-table entry, and
  // page_frees the 64 KB reservation back to NT. Aggressive release-on-
  // empty: a thread that briefly burst past its primary arena returns
  // every overflow byte the moment its workload drains.
  //
  // Primary arena does NOT carry this counter — the primary is
  // permanent per-thread state and is released only at thread exit.
  // 32-bit is plenty: an overflow arena holds at most ~2300 16-byte
  // allocations (36 KB data / 16 B header per alloc), which fits in
  // 12 bits with room to spare.
  uint32_t live_blocks;
  uint32_t pad;
};

static_assert(sizeof(ScratchOverflowArena) == 64,
              "ScratchOverflowArena must be 64 bytes — one cache line");
static_assert(__builtin_offsetof(ScratchOverflowArena, live_blocks) == 56,
              "live_blocks must occupy the slot the substrate token used "
              "to inhabit; size and alignment are unchanged");

// ---------------------------------------------------------------------------
// Pending-flag bits for cross-thread signaling via ThreadLocalWord
// ---------------------------------------------------------------------------
//
// Cross-thread writers set bits via ThreadLocalWord::signal_or().
// The owner checks any_pending() on the alloc hot path — one RELAXED
// load + TEST + predicted-not-taken Jcc. Zero cost when no flags are set.

namespace scratch_flags {

// Memory pressure: decommit demand-committed pages above the initial level.
// Set by a global memory pressure detector or explicit cross-thread request.
// Cleared by the owner after decommitting.
inline constexpr uint32_t DECOMMIT = 1u << 0;

} // namespace scratch_flags

// ---------------------------------------------------------------------------
// ThreadScratchState — per-thread control block, lives on page 0
// ---------------------------------------------------------------------------
//
// Three cache lines. Line 0: hot-path allocator state (arena header,
// overflow pointer). Line 1: cold fields (registry link, owner TID).
// Line 2: ThreadLocalWord for cross-thread signaling.
//
// Separated from data by the leading guard page — underflow from data
// hits the guard; overflow from data hits the trailing guard. Neither
// direction can reach this control block.

struct alignas(64) ThreadScratchState {
  // -- Cache line 0: Hot path (alloc/free) --
  ArenaHeader arena;                // Primary arena allocator state (48B).
  ScratchOverflowArena *overflow;   // Head of overflow arena chain (nullptr).
  uint64_t pad_;                    // Pad to cache-line boundary.

  // -- Cache line 1: Cold fields (registry, fork) --
  ThreadScratchState *next;         // Global registry linked-list link.
  uint32_t owner_tid;               // Thread ID for fork dead-thread detection.
  uint32_t pad2_;                   // Pad.
  // Pre-INIT_READY indicator. 1 iff this thread's primary arena was
  // reserved before the mapping table reached INIT_READY, and the
  // arena's {base, RESERVE_SIZE} was queued via the deferred Receipt
  // mechanism for the walker's Pass 2 to stamp LIBC_INTERNAL. The
  // common case (every thread spawned past Tier A) sets this to 0 and
  // takes the inline `register_mapping_internal` path.
  //
  // The bit only governs how the arena's mapping-table entry was
  // INSTALLED. Cleanup is uniform — `g_mapping_table.remove(base)` is
  // a no-op when no entry exists, so the same removal call works for
  // pre-INIT_READY threads whose receipts were enqueued but had not
  // yet been stamped at thread exit. (Bounded by Tier A duration; in
  // practice receipts always stamp before any thread exits.)
  uint8_t is_pre_init_ready;
  uint8_t pad3_[7];                 // Pad to keep reserved_ 8-aligned.
  char reserved_[40];               // Reserved for future use; sized so
                                    // `word` lands at offset 128 (asserted).

  // -- Cache line 2: Cross-thread signaling --
  // ThreadLocalWord for pending-flag notification. Cross-thread writers
  // set scratch_flags bits via ThreadLocalWord::signal_or(). The owner
  // checks word.any_pending() on the alloc hot path — one RELAXED load
  // + TEST + predicted-not-taken Jcc per allocation. Zero cost when clear.
  ThreadLocalWord word;

  // -- Crystalline per-thread state --
  // Per-domain index into the cross-thread CrystallineSlotPool. 0 is
  // the pool's null sentinel — "this thread has not yet reserved in
  // domain `d`"; lazy-claimed on the first read()/reserve_slot() call
  // for that domain. Cleared by CrystallineDomain::fork_reinit on the
  // surviving thread so its next reservation lazy-claims into the
  // post-fork-reset pool (the slot it owned pre-fork is back on the
  // freelist; reusing it would double-allocate).
  uint16_t crystalline_slot_idx[concurrent::kMaxCrystallineDomains];

  // Per-domain retire-batch records. Owner-exclusive — no cross-thread
  // access. Batches live on ThreadScratchState because their first /
  // last / list chains thread through CrystallineNode objects the owner
  // allocated within the per-thread arena. On thread exit
  // scratch_flush_crystalline_batches closes each batch and republishes
  // its retires into the domain pool's slot chains before the arena is
  // released. CrystallineBatch is alignof(8) — `crystalline_slot_idx[]`
  // is 16 bytes (8-aligned), so this array is naturally aligned.
  concurrent::CrystallineBatch
      crystalline_batches[concurrent::kMaxCrystallineDomains];

  // Reconstruct the VA reservation base from `this`. The control block
  // is always at page 0 of a 64KB-aligned reservation.
  LIBC_INLINE char *base() const {
    return reinterpret_cast<char *>(
        const_cast<ThreadScratchState *>(this));
  }
};

// Control-block fits in the 4 KB control page we commit per arena.
static_assert(sizeof(ThreadScratchState) <= scratch_detail::PAGE_SIZE,
              "ThreadScratchState must fit in control page");
static_assert(__builtin_offsetof(ThreadScratchState, word) == 128,
              "ThreadLocalWord must be on cache line 2");
static_assert(
    __builtin_offsetof(ThreadScratchState, crystalline_slot_idx) % 8 == 0,
    "crystalline_slot_idx[] must be 8-byte aligned");

// ---------------------------------------------------------------------------
// Block header — 16 bytes per allocation (same overhead as old canary pair)
// ---------------------------------------------------------------------------
//
// Replaces the [canary, ~canary] pair with a boundary-tagged header that
// enables non-LIFO freelist management while preserving canary hardening.
// The canary incorporates both the header address and user_size — any
// corruption to size or address is detected.

namespace scratch_detail {

inline constexpr uint16_t FLAG_FREE = 0x1;

struct BlockHeader {
  uint32_t user_size;   // User data size in bytes (not including header).
  uint16_t flags;       // FLAG_FREE when on the freelist.
  uint16_t pad;
  uintptr_t canary;     // seed ^ &this ^ user_size.
};

static_assert(sizeof(BlockHeader) == 16, "BlockHeader must be 16 bytes");

LIBC_INLINE uintptr_t make_canary(uintptr_t seed, const BlockHeader *hdr) {
  return alloc_primitives::derive_canary(seed, hdr,
                                         static_cast<size_t>(hdr->user_size));
}

LIBC_INLINE void write_header(uintptr_t seed, char *block, uint32_t user_size) {
  auto *hdr = reinterpret_cast<BlockHeader *>(block);
  hdr->user_size = user_size;
  hdr->flags = 0;
  hdr->pad = 0;
  hdr->canary = alloc_primitives::derive_canary(
      seed, hdr, static_cast<size_t>(user_size));
}

LIBC_INLINE void verify_header(uintptr_t seed, const BlockHeader *hdr) {
  if (LIBC_UNLIKELY(!alloc_primitives::verify_canary(
          hdr->canary, seed, hdr, static_cast<size_t>(hdr->user_size))))
    __builtin_trap();
}

} // namespace scratch_detail


// ---------------------------------------------------------------------------
// Global registry — enables fork_reinit to reclaim dead-thread VA
// ---------------------------------------------------------------------------
//
// Singly-linked list of all ThreadScratchState objects. Protected by a
// spinlock. Thread creation/destruction is infrequent, so contention
// is negligible. The spinlock (not a futex) avoids circular dependency
// with the signal/futex subsystem — same rationale as SlabPool.

namespace scratch_detail {

inline cpp::Atomic<ThreadScratchState *> g_scratch_head{nullptr};
inline cpp::Atomic<uint32_t> g_scratch_list_lock{0};

LIBC_INLINE void registry_lock() {
  uint32_t expected = 0;
  if (LIBC_LIKELY(g_scratch_list_lock.compare_exchange_weak(
          expected, 1, cpp::MemoryOrder::ACQUIRE, cpp::MemoryOrder::RELAXED)))
    return;
  // Contended: hardware address monitor on the lock word. UMWAIT/MWAITX
  // sleeps at near-zero power until the cache line is written (lock
  // release). TTAS: load before CAS to avoid unnecessary cache-line
  // writes when another thread still holds the lock.
  for (;;) {
    spin_wait::spin_until_changed(&g_scratch_list_lock, 1u);
    if (g_scratch_list_lock.load(cpp::MemoryOrder::RELAXED) != 0)
      continue;
    expected = 0;
    if (g_scratch_list_lock.compare_exchange_weak(
            expected, 1, cpp::MemoryOrder::ACQUIRE, cpp::MemoryOrder::RELAXED))
      return;
  }
}

LIBC_INLINE void registry_unlock() {
  g_scratch_list_lock.store(0, cpp::MemoryOrder::RELEASE);
}

LIBC_INLINE void registry_insert(ThreadScratchState *state) {
  registry_lock();
  state->next = g_scratch_head.load(cpp::MemoryOrder::RELAXED);
  g_scratch_head.store(state, cpp::MemoryOrder::RELAXED);
  registry_unlock();
}

LIBC_INLINE void registry_remove(ThreadScratchState *state) {
  registry_lock();
  auto *head = g_scratch_head.load(cpp::MemoryOrder::RELAXED);
  if (head == state) {
    g_scratch_head.store(state->next, cpp::MemoryOrder::RELAXED);
  } else {
    auto *prev = head;
    while (prev && prev->next != state)
      prev = prev->next;
    if (prev)
      prev->next = state->next;
  }
  registry_unlock();
}

} // namespace scratch_detail

// ---------------------------------------------------------------------------
// TLS slot management — process-wide, one-time init
// ---------------------------------------------------------------------------

namespace scratch_detail {

// -- Out-of-line hooks implemented in thread_scratch.cpp --------------------
//
// Declared here (not in a separate header) so the create_*_arena /
// scratch_thread_cleanup inline functions below can invoke them. The
// implementations pull in mapping_table.h and crystalline_domain_registry.h,
// which we deliberately keep out of this header to avoid cycles with
// subsystems that in turn consume ThreadScratch.

// Remove a ThreadScratch arena from the mapping table. Called from
// scratch_thread_cleanup before the VA is released back to NT. No-op
// if the table doesn't know the range (belt-and-suspenders for fork-
// child / exec-hollow paths that reset the table without walking
// ThreadScratch).
void scratch_arena_unregister(void *base);

// Drain the exiting thread's Crystalline retire batches (now living
// inline in ThreadScratchState::crystalline_batches[]) into each
// domain's slot chains. Called from scratch_thread_cleanup BEFORE the
// arena is released — batch anchor nodes whose only retire-path referrer
// is the exiting thread would leak if the arena VA disappears before
// the batches publish. Walks the CrystallineDomain registry internally.
void scratch_flush_crystalline_batches(ThreadScratchState *state);

// Release every per-domain Crystalline slot the exiting thread had
// claimed back to its domain's pool. Called from scratch_thread_cleanup
// AFTER the batch flush and BEFORE the arena VA release. Walks the
// CrystallineDomain registry.
void scratch_release_crystalline_slots(ThreadScratchState *state);

// Maximum number of pending internal-region receipts that can queue up
// between Tier A start and the walker's Pass 2 stamping loop. Two paths
// enqueue here: ThreadScratch's pre-INIT_READY `create_thread_state`
// (transient Windows-loader workers spawned during DllMain that call
// `get_thread_scratch` before the mapping table is observable) and the
// substrate's `reserve_new_arena` when called pre-INIT_READY. Cap is
// generous for the plausible worst case; overflow traps so an
// unexpected pattern surfaces immediately rather than silently
// dropping a receipt and leaving the arena unregistered.
inline constexpr uint32_t kPendingInternalReceiptsCap = 8;

// Enqueue an internal-region receipt to be stamped LIBC_INTERNAL by the
// walker's Pass 2. Used by the substrate's `reserve_new_arena` for the
// pre-INIT_READY path (mapping table not yet observable). Single-writer-
// safe via the same spinlock used by the bootstrap-tier scratch path
// and by `harvest_pending_internal_receipts`.
//
// Trap on cap overflow — see `kPendingInternalReceiptsCap`.
void enqueue_pending_internal_receipt(
    void *base, size_t size,
    ::LIBC_NAMESPACE::internal::InternalKind kind);

// Harvest every queued internal-region receipt into the walker's `out`
// buffer. Called from `mapping_table_init_fn` (in mapping_table.cpp)
// AFTER `g_mapping_table.ensure_init()` has driven the table to
// INIT_READY but BEFORE the walker's Pass 2 begins stamping. Returns
// the number of receipts written; never exceeds `cap` (caller passes
// the walker's remaining capacity). Bounded by
// `kPendingInternalReceiptsCap`, so a properly-sized walker buffer
// always has headroom.
//
// Serialization: the implementation takes a small spinlock that the
// enqueue paths also take. This closes the multi-thread race where an
// enqueuer could observe `is_init_ready() == false`, get preempted, and
// enqueue post-harvest. Combined with the post-Pass-2 final-drain in
// `mapping_table_finalize_init` (which holds the same lock across
// [mark-ready + drain]), every enqueue is either drained-and-stamped or
// observes ready=true under the lock and falls through to inline-stamp.
uint32_t harvest_pending_internal_receipts(
    ::LIBC_NAMESPACE::internal::Receipt *out, uint32_t cap);

// Lock helpers for callers that need to compose [check + drain + mark]
// or [check ready + dispatch] atomically against the enqueue path.
// The walker's `mapping_table_finalize_init` holds the lock across the
// final-drain and the `mark_mapping_table_ready` latch so any enqueuer
// that takes the lock afterwards sees ready=true and inline-stamps.
// Substrate's `slow_acquire_arena` takes the lock around its
// [is_mapping_table_ready check + enqueue-or-inline-stamp dispatch] so
// the dispatch is consistent with the walker's atomic latch.
void pending_internal_receipts_lock();
void pending_internal_receipts_unlock();

// Enqueue a pending internal-region receipt. Caller MUST hold the
// pending-internal-receipts lock; trap on cap overflow.
void enqueue_pending_internal_receipt_locked(
    void *base, size_t size,
    ::LIBC_NAMESPACE::internal::InternalKind kind);

// Drain the pending queue into `out` under the assumption that the
// caller holds `pending_internal_receipts_lock`. Returns the number of
// receipts drained; traps if `out_cap < kPendingInternalReceiptsCap`.
uint32_t drain_pending_internal_receipts_locked(
    ::LIBC_NAMESPACE::internal::Receipt *out, uint32_t out_cap);

// Sentinel: TLS index not yet allocated.
inline constexpr DWORD TLS_UNINITIALIZED = TLS_OUT_OF_INDEXES;

// Process-wide TLS slot index. Allocated once by the first thread that
// needs scratch space. The atomic guards against concurrent first-use
// from multiple threads (e.g., thread pool burst at startup).
inline cpp::Atomic<DWORD> g_scratch_tls_index{TLS_UNINITIALIZED};

// One-time init lock. 0 = unlocked, 1 = locked.
inline cpp::Atomic<uint32_t> g_scratch_init_lock{0};

// Thread-exit cleanup callback. Called by tls_cleanup_run_all() via
// the .CRT$XLC TLS callback on DLL_THREAD_DETACH.
//
// Out-of-line because the body reaches into mapping_table.h (which
// thread_scratch.h must not transitively include to avoid the header
// cycles documented at the top of the out-of-line hooks block above)
// to remove the per-arena LIBC_INTERNAL entries before page_freeing
// each arena.
//
// Address-taken for the TLS cleanup callback pointer.
void scratch_thread_cleanup(void *val);

// Allocate the process-wide TLS slot and register the cleanup callback.
// Returns the TLS index, or TLS_UNINITIALIZED on failure.
// Serialized by g_scratch_init_lock for the rare concurrent-first-use case.
LIBC_INLINE DWORD init_tls_slot() {
  // Fast check: already initialized?
  DWORD idx = g_scratch_tls_index.load(cpp::MemoryOrder::ACQUIRE);
  if (idx != TLS_UNINITIALIZED)
    return idx;

  // Acquire init lock.
  uint32_t expected = 0;
  if (!g_scratch_init_lock.compare_exchange_strong(
          expected, 1, cpp::MemoryOrder::ACQUIRE, cpp::MemoryOrder::RELAXED)) {
    // Another thread is initializing. Hardware monitor spin on the TLS
    // index — UMWAIT/MWAITX sleeps until the cache line is written
    // (init complete), then re-checks. Replaces NtYieldExecution.
    // Uses spin_on_raw because DWORD (unsigned long) != uint32_t
    // (unsigned int) on Windows — same size, different type.
    while (g_scratch_tls_index.load(cpp::MemoryOrder::ACQUIRE) ==
           TLS_UNINITIALIZED) {
      spin_wait::spin_on_raw(&g_scratch_tls_index.val, TLS_UNINITIALIZED);
    }
    return g_scratch_tls_index.load(cpp::MemoryOrder::ACQUIRE);
  }

  // We hold the lock. Double-check.
  idx = g_scratch_tls_index.load(cpp::MemoryOrder::RELAXED);
  if (idx != TLS_UNINITIALIZED) {
    g_scratch_init_lock.store(0, cpp::MemoryOrder::RELEASE);
    return idx;
  }

  // Runtime page size validation. The geometry constants are compiled
  // against 4KB pages / 64KB granularity. A mismatch means guard pages,
  // commit ranges, and the control/data layout are silently wrong.
  // Cannot be static_assert (runtime OS query); must survive NDEBUG.
  // Same trap pattern as SlabPool::init.
  if (LIBC_UNLIKELY(::LIBC_NAMESPACE::windows::get_page_size() !=
                        PAGE_SIZE ||
                    ::LIBC_NAMESPACE::windows::get_alloc_granularity() !=
                        ALLOC_GRANULARITY))
    __builtin_trap();

  // Allocate a TEB inline TLS slot.
  idx = tls_alloc();
  if (idx == TLS_OUT_OF_INDEXES) {
    g_scratch_init_lock.store(0, cpp::MemoryOrder::RELEASE);
    return TLS_UNINITIALIZED;
  }

  // Register thread-exit cleanup at the SUBSTRATE phase. The descending-
  // phase walk in tls_cleanup_run_all guarantees this runs LAST among the
  // thread-cleanup callbacks, so higher-phase teardowns (lifecycle ops,
  // Crystalline retires, slab abandons) see a live ThreadScratch arena
  // throughout their work. Releasing the arena VA here is the final step.
  tls_cleanup_register(idx, scratch_thread_cleanup,
                        kTlsCleanupPhaseSubstrate);

  // Publish. RELEASE ensures the cleanup registration is visible before
  // other threads see the index and start creating per-thread state.
  g_scratch_tls_index.store(idx, cpp::MemoryOrder::RELEASE);
  g_scratch_init_lock.store(0, cpp::MemoryOrder::RELEASE);
  return idx;
}

} // namespace scratch_detail

// ---------------------------------------------------------------------------
// Per-thread state creation (cold path, once per thread)
// ---------------------------------------------------------------------------

namespace scratch_detail {

// Reservation base from an ArenaHeader pointer. ArenaHeader sits at offset
// 0 of both ThreadScratchState and ScratchOverflowArena, which themselves
// sit at offset 0 of their 64 KB reservation — so the header pointer IS
// the reservation base. Used as the first argument to the GuardedRegion
// static typed API at every post-setup commit/decommit call site.
LIBC_INLINE char *arena_reservation_base(ArenaHeader *arena) {
  return reinterpret_cast<char *>(arena);
}

// Create a new ThreadScratchState for the calling thread.
//
// One path, branched only on whether the mapping table is INIT_READY at
// the moment of creation:
//
//   - `page_reserve(RESERVE_SIZE)` → 64 KB raw NT VA reservation.
//   - GuardedRegion eager-commits (control / Crystalline / initial data).
//   - If `g_mapping_table.is_init_ready()`: inline
//     `register_mapping_internal(base, RESERVE_SIZE)`.
//   - Else: enqueue a pending internal-region receipt
//     ({base, RESERVE_SIZE, BootstrapScratch}); the walker's
//     `mapping_table_init_fn` harvests it after `ensure_init` returns
//     INIT_READY, and Pass 2 stamps it LIBC_INTERNAL.
//
// No substrate involvement. The arena VA is owned by ThreadScratch end
// to end — thread exit `page_free`s it directly, returning the 64 KB
// to NT immediately.
//
// Out-of-line because the body reaches into mapping_table, whose
// header must not be pulled into thread_scratch.h.
//
// Returns the state pointer, or nullptr on failure.
ThreadScratchState *create_thread_state(DWORD tls_index);

} // namespace scratch_detail

// ---------------------------------------------------------------------------
// get_thread_scratch — fast TLS read + lazy init
// ---------------------------------------------------------------------------

// Returns the calling thread's scratch state. Lazy: first call per thread
// allocates the VA reservation and initializes the control block.
// Returns nullptr only on catastrophic failure (OOM or TLS exhaustion).
LIBC_INLINE ThreadScratchState *get_thread_scratch() {
  DWORD idx = scratch_detail::g_scratch_tls_index.load(
      cpp::MemoryOrder::ACQUIRE);
  if (LIBC_UNLIKELY(idx == scratch_detail::TLS_UNINITIALIZED)) {
    idx = scratch_detail::init_tls_slot();
    if (idx == scratch_detail::TLS_UNINITIALIZED)
      return nullptr;
  }

  auto *state =
      static_cast<ThreadScratchState *>(teb_tls_get(idx));
  if (LIBC_LIKELY(state != nullptr))
    return state;

  // First use on this thread — create the per-thread state.
  return scratch_detail::create_thread_state(idx);
}

// ---------------------------------------------------------------------------
// Arena alloc/free primitives — operate on ArenaHeader, shared by all arenas
// ---------------------------------------------------------------------------

namespace scratch_detail {

// Freelist pointer hardening: XOR-encode the next-free pointer stored in
// the first 8 bytes of user data. Uses canary_seed + storage address so
// that a corrupted pointer decodes to garbage (immediate fault or canary
// mismatch) rather than silently redirecting the allocator. Thin wrappers
// over alloc_primitives::xor_encode_next/xor_decode_next — preserved as
// uintptr_t-in/out helpers because freelist slots are stored as raw
// uintptr_t in user data and fl_decode_validated needs a char* return.
LIBC_INLINE uintptr_t fl_encode(uintptr_t ptr, uintptr_t seed,
                                const void *location) {
  return reinterpret_cast<uintptr_t>(alloc_primitives::xor_encode_next(
      reinterpret_cast<void *>(ptr), seed, location));
}
LIBC_INLINE char *fl_decode(uintptr_t encoded, uintptr_t seed,
                            const void *location) {
  return static_cast<char *>(alloc_primitives::xor_decode_next(
      reinterpret_cast<void *>(encoded), seed, location));
}

// Decode and bounds-validate a freelist pointer. Traps if the decoded
// pointer falls outside the arena's data region. Matches the validate_node
// discipline used in SlabPool::drain_xthread.
LIBC_INLINE char *fl_decode_validated(uintptr_t encoded, uintptr_t seed,
                                       const void *location,
                                       const ArenaHeader *arena) {
  char *ptr = fl_decode(encoded, seed, location);
  if (!ptr)
    return nullptr;
  if (LIBC_UNLIKELY(ptr < arena->data_base || ptr >= arena->data_limit))
    __builtin_trap(); // Corrupted freelist pointer.
  return ptr;
}

// Compute total allocation size (header + user data, 16-byte aligned).
LIBC_INLINE size_t alloc_total(size_t user_size) {
  return (sizeof(BlockHeader) + user_size + ALLOC_ALIGN - 1) &
         ~(ALLOC_ALIGN - 1);
}

// Try to allocate from the arena's free list (first-fit).
// Returns user pointer on success, nullptr on miss.
LIBC_INLINE void *try_alloc_from_freelist(ArenaHeader *arena,
                                           size_t user_size,
                                           size_t total) {
  uintptr_t seed = arena->canary_seed.seed;

  // Walk with explicit prev/curr tracking. free_list_head is a raw pointer
  // (protected by the control page guard). The next pointers stored in user
  // data are XOR-encoded with (seed ^ &storage_location).
  //
  // prev_storage: where to write the replacement pointer on unlink.
  //   - For head: &arena->free_list_head (raw write)
  //   - For non-head: user_data of previous block (encoded write)
  // prev_is_head: tracks which write mode to use.
  char *block = arena->free_list_head;
  char *prev_block = nullptr;
  bool prev_is_head = true;

  while (block) {
    auto *hdr = reinterpret_cast<BlockHeader *>(block);
    size_t block_total = alloc_total(hdr->user_size);
    char *user_data = block + sizeof(BlockHeader);

    // Decode + bounds-validate next pointer from this block's user data.
    char *next = fl_decode_validated(
        *reinterpret_cast<uintptr_t *>(user_data), seed, user_data, arena);

    if (block_total >= total) {
      // Found a fit. Unlink: patch prev to skip this block.
      if (prev_is_head) {
        arena->free_list_head = next;
      } else {
        // Re-encode `next` at prev_block's storage location.
        char *prev_ud = prev_block + sizeof(BlockHeader);
        *reinterpret_cast<uintptr_t *>(prev_ud) =
            fl_encode(reinterpret_cast<uintptr_t>(next), seed, prev_ud);
      }

      // Update header for the new allocation size. Canary goes through
      // the primitive so the formula stays in one place — if derive_canary
      // ever rotates away from pure XOR, this reuse site moves with it.
      hdr->user_size = static_cast<uint32_t>(user_size);
      hdr->flags = 0;
      hdr->canary = alloc_primitives::derive_canary(seed, hdr, user_size);

      // Zero the encoded freelist pointer (first 8 bytes of user data).
      *reinterpret_cast<uintptr_t *>(user_data) = 0;
      return user_data;
    }

    // Advance.
    prev_block = block;
    prev_is_head = false;
    block = next;
  }
  return nullptr;
}

// Demand-commit cold path for a single arena. Commits pages up to the
// new bump position. Returns user pointer, or nullptr on failure.
LIBC_INLINE void *arena_alloc_slow(ArenaHeader *arena, size_t user_size,
                                   size_t total) {
  char *alloc_base = arena->bump;
  char *new_bump = alloc_base + total;

  // Check against data region limit (trailing guard boundary).
  if (LIBC_UNLIKELY(new_bump > arena->data_limit))
    return nullptr;

  // Round commit target up to page boundary.
  char *new_commit = reinterpret_cast<char *>(
      (reinterpret_cast<uintptr_t>(new_bump) + PAGE_SIZE - 1) &
      ~(PAGE_SIZE - 1));
  if (new_commit > arena->data_limit)
    new_commit = arena->data_limit;

  // Commit the gap within the DATA region. Offset is relative to the
  // region's start (arena->data_base); both the offset and size are
  // page-aligned because commit_limit always lies on a page boundary and
  // new_commit was rounded up to PAGE_SIZE above, so the GuardedRegion
  // subrange asserts hold.
  size_t commit_size =
      static_cast<size_t>(new_commit - arena->commit_limit);
  if (commit_size > 0) {
    size_t offset_in_region =
        static_cast<size_t>(arena->commit_limit - arena->data_base);
    if (!alloc_primitives::GuardedRegion::commit_subrange(
            arena_reservation_base(arena), kArenaLayout, DATA_REGION_IDX,
            offset_in_region, commit_size))
      return nullptr;
    arena->commit_limit = new_commit;
  }

  // Store the caller's raw user_size (not the aligned-up total-header), so
  // scratch_free's size check matches what the caller passes back.
  write_header(arena->canary_seed.seed, alloc_base,
               static_cast<uint32_t>(user_size));
  arena->bump = new_bump;
  return alloc_base + sizeof(BlockHeader);
}

// Try to allocate from a single arena (freelist → bump → demand-commit).
// Returns user pointer on success, nullptr if this arena is exhausted.
LIBC_INLINE void *try_alloc_from_arena(ArenaHeader *arena, size_t user_size) {
  size_t total = alloc_total(user_size);

  // 1. Check free list.
  void *from_free = try_alloc_from_freelist(arena, user_size, total);
  if (from_free)
    return from_free;

  // 2. Bump allocate (hot path).
  char *alloc_base = arena->bump;
  char *new_bump = alloc_base + total;
  if (LIBC_LIKELY(new_bump <= arena->commit_limit)) {
    write_header(arena->canary_seed.seed, alloc_base,
                 static_cast<uint32_t>(user_size));
    arena->bump = new_bump;
    return alloc_base + sizeof(BlockHeader);
  }

  // 3. Demand-commit cold path.
  return arena_alloc_slow(arena, user_size, total);
}

// Create a new overflow arena with the same 64 KB geometry as the
// primary. Direct `page_reserve` + `register_mapping_internal` —
// overflow fires only after a thread has used 36 KB of scratch, by
// which point the mapping table is long past INIT_READY (`trap` if
// not). Returns the arena pointer, or nullptr on reservation /
// commit / mapping-table failure.
//
// Out-of-line because the body reaches into mapping_table.h, which
// must not pull into thread_scratch.h.
ScratchOverflowArena *create_overflow_arena(ThreadScratchState *state);

// Splice `ov` from `state->overflow`, drop its mapping-table entry,
// and `page_free` its 64 KB VA. Caller MUST guarantee the arena holds
// no live allocations (`ov->live_blocks == 0`) and is reachable
// through the chain. Out-of-line because the body reaches into
// mapping_table.h.
void release_empty_overflow(ThreadScratchState *state,
                            ScratchOverflowArena *ov);

// Handle pending cross-thread signals. Cold path — only called when
// word.any_pending() returns true (predicted not-taken on the alloc path).
LIBC_INLINE void scratch_handle_pending(ThreadScratchState *state) {
  if (state->word.test_flags(scratch_flags::DECOMMIT)) {
    char *floor =
        state->arena.data_base + INITIAL_DATA_COMMIT;
    if (state->arena.commit_limit > floor) {
      char *needed = reinterpret_cast<char *>(
          (reinterpret_cast<uintptr_t>(state->arena.bump) + PAGE_SIZE - 1) &
          ~(PAGE_SIZE - 1));
      if (needed < floor)
        needed = floor;
      if (state->arena.commit_limit > needed) {
        size_t offset_in_region =
            static_cast<size_t>(needed - state->arena.data_base);
        size_t decommit_size =
            static_cast<size_t>(state->arena.commit_limit - needed);
        alloc_primitives::GuardedRegion::decommit_subrange(
            arena_reservation_base(&state->arena), kArenaLayout,
            DATA_REGION_IDX, offset_in_region, decommit_size,
            /*reset_only=*/false);
        state->arena.commit_limit = needed;
      }
    }
    state->word.clear_flags(scratch_flags::DECOMMIT);
  }
}

// Scan the free list for blocks adjacent to the current bump position.
// Unlink them and rewind the bump further. This reclaims space from
// non-LIFO frees when subsequent LIFO frees bring the bump close.
// O(free_list_length²) worst case, but the list has ≤5 entries typical.
LIBC_INLINE void collapse_trailing_free(ArenaHeader *arena) {
  uintptr_t seed = arena->canary_seed.seed;
  bool found;
  do {
    found = false;
    char *block = arena->free_list_head;
    char *prev_block = nullptr;
    bool prev_is_head = true;

    while (block) {
      auto *hdr = reinterpret_cast<BlockHeader *>(block);
      size_t block_total = alloc_total(hdr->user_size);
      char *user_data = block + sizeof(BlockHeader);
      char *next = fl_decode_validated(
          *reinterpret_cast<uintptr_t *>(user_data), seed, user_data, arena);

      if (block + block_total == arena->bump) {
        // Adjacent to bump — unlink and rewind.
        if (prev_is_head) {
          arena->free_list_head = next;
        } else {
          char *prev_ud = prev_block + sizeof(BlockHeader);
          *reinterpret_cast<uintptr_t *>(prev_ud) =
              fl_encode(reinterpret_cast<uintptr_t>(next), seed, prev_ud);
        }
        arena->bump = block;
        found = true;
        break; // Restart scan (list changed).
      }
      prev_block = block;
      prev_is_head = false;
      block = next;
    }
  } while (found);
}

// High-watermark decommit for a single arena. Returns demand-committed
// pages above the initial level when the bump pointer rewinds.
LIBC_INLINE void maybe_decommit(ArenaHeader *arena) {
  char *floor = arena->data_base + INITIAL_DATA_COMMIT;
  if (arena->commit_limit > floor) {
    char *needed = reinterpret_cast<char *>(
        (reinterpret_cast<uintptr_t>(arena->bump) + PAGE_SIZE - 1) &
        ~(PAGE_SIZE - 1));
    if (needed < floor)
      needed = floor;
    if (arena->commit_limit > needed) {
      size_t offset_in_region =
          static_cast<size_t>(needed - arena->data_base);
      size_t decommit_size =
          static_cast<size_t>(arena->commit_limit - needed);
      alloc_primitives::GuardedRegion::decommit_subrange(
          arena_reservation_base(arena), kArenaLayout, DATA_REGION_IDX,
          offset_in_region, decommit_size, /*reset_only=*/false);
      arena->commit_limit = needed;
    }
  }
}

} // namespace scratch_detail

// Allocate `size` bytes from the thread's scratch arena chain.
// Returns a 16-byte aligned pointer, or nullptr on failure.
// The allocation is prefixed with a BlockHeader (not visible to caller).
//
// Order:
//   1. Primary arena (hot — covers every short-lived buffer).
//   2. Overflow arenas — newest at the chain head per head-insertion.
//      Walk youngest-first (chain head). Allocations concentrate on
//      the youngest arena; older arenas drain via release-on-empty
//      in `scratch_free` rather than gaining new bump traffic.
//   3. Create a fresh overflow arena and allocate from it.
//
// Every successful overflow allocation increments `ov->live_blocks` —
// the counter `scratch_free` drives back to zero before reclaiming the
// arena's 64 KB VA.
LIBC_INLINE void *scratch_alloc(ThreadScratchState *state, size_t size) {
  // Check for pending cross-thread signals. Zero cost when clear:
  // one RELAXED load (MOV) + TEST + predicted-not-taken Jcc.
  if (LIBC_UNLIKELY(state->word.any_pending()))
    scratch_detail::scratch_handle_pending(state);

  // 1. Try primary arena.
  void *result = scratch_detail::try_alloc_from_arena(&state->arena, size);
  if (LIBC_LIKELY(result != nullptr))
    return result;

  // 2. Try overflow arenas — youngest first.
  for (auto *ov = state->overflow; ov; ov = ov->next) {
    result = scratch_detail::try_alloc_from_arena(&ov->arena, size);
    if (result) {
      ++ov->live_blocks;
      return result;
    }
  }

  // 3. Create new overflow arena and alloc from it.
  auto *new_ov = scratch_detail::create_overflow_arena(state);
  if (!new_ov)
    return nullptr;
  result = scratch_detail::try_alloc_from_arena(&new_ov->arena, size);
  if (LIBC_LIKELY(result != nullptr))
    ++new_ov->live_blocks;
  return result;
}

// Free a scratch allocation. Supports both LIFO (fast rewind) and
// non-LIFO (freelist prepend) patterns. Derives the arena from the
// pointer via 64KB alignment — same code path for primary and overflow.
//
// LIFO fast path: if the block is at the top of the arena's bump pointer,
// rewinds the bump (identical to the old scratch_release). Additionally
// collapses any adjacent free blocks below the new bump position.
//
// Non-LIFO path: marks the block FREE and prepends to the arena's free
// list. Subsequent allocs will reuse it via first-fit.
//
// Release-on-empty: if the freed block lived in an OVERFLOW arena and
// that arena's `live_blocks` reaches zero, the arena is spliced from
// `state->overflow`, its mapping-table entry is removed, and its 64 KB
// VA is `page_free`d back to NT. Primary arena is exempt — it is the
// permanent per-thread base and is released only at thread exit.
LIBC_INLINE void scratch_free(void *ptr, size_t size) {
  if (!ptr)
    return;

  char *block = static_cast<char *>(ptr) - sizeof(scratch_detail::BlockHeader);
  auto *hdr = reinterpret_cast<scratch_detail::BlockHeader *>(block);

  // Derive the arena from the pointer. Both primary and overflow arenas
  // have ArenaHeader at offset 0 of a 64KB-aligned reservation.
  uintptr_t arena_base_addr =
      reinterpret_cast<uintptr_t>(block) &
      ~(static_cast<uintptr_t>(scratch_detail::ALLOC_GRANULARITY) - 1);
  auto *arena = reinterpret_cast<ArenaHeader *>(arena_base_addr);

  // Pointer range validation — trap on wild pointers.
  if (LIBC_UNLIKELY(block < arena->data_base || block >= arena->data_limit))
    __builtin_trap();

  // Validate canary — traps on corruption (including tampered user_size).
  scratch_detail::verify_header(arena->canary_seed.seed, hdr);

  // Use the canary-validated stored size for all operations. The canary
  // incorporates user_size, so a tampered size traps above. This makes
  // scratch_free immune to caller mistakes (wrong size parameter) —
  // undersize would partially zero (info leak), oversize would overflow
  // into adjacent blocks. Hard check: one compare + branch, negligible
  // cost, catches caller bugs in release builds.
  uint32_t user_size = hdr->user_size;
  if (LIBC_UNLIKELY(user_size != static_cast<uint32_t>(size)))
    __builtin_trap(); // Caller size does not match stored size.

  // Zero user data to prevent info leaks. The compiler barrier after
  // memset prevents dead-store elimination while allowing memset to use
  // rep stosb / SIMD — orders of magnitude faster than byte-at-a-time
  // volatile writes for buffers > 64 bytes.
  __builtin_memset(ptr, 0, user_size);
  __asm__ volatile("" ::: "memory");

  // Compute block end for LIFO-top check.
  size_t total = scratch_detail::alloc_total(user_size);
  char *block_end = block + total;

  if (block_end == arena->bump) {
    // LIFO fast path: zero header (16 bytes) + rewind bump pointer.
    // The header must be fully zeroed because the bump rewind exposes
    // the entire block (header + user data) as virgin space for future
    // allocations — stale canary/size values would confuse write_header.
    __builtin_memset(block, 0, sizeof(scratch_detail::BlockHeader));
    __asm__ volatile("" ::: "memory");
    arena->bump = block;
    // Collapse any adjacent free blocks below the new bump.
    scratch_detail::collapse_trailing_free(arena);
    // High-watermark decommit.
    scratch_detail::maybe_decommit(arena);
  } else {
    // Non-LIFO: mark free, prepend to free list.
    // Clear canary to enable double-free detection: a subsequent
    // scratch_free on this block will see canary != expected and trap.
    // No full header memset needed — flags/user_size are overwritten
    // immediately, and pad was zeroed at alloc time (write_header).
    hdr->canary = 0;
    hdr->flags = scratch_detail::FLAG_FREE;
    hdr->user_size = user_size;
    // Store XOR-encoded next-free pointer in first 8 bytes of user data.
    *reinterpret_cast<uintptr_t *>(ptr) = scratch_detail::fl_encode(
        reinterpret_cast<uintptr_t>(arena->free_list_head),
        arena->canary_seed.seed, ptr);
    arena->free_list_head = block;
  }

  // Overflow-arena lifecycle: decrement live_blocks; release on zero.
  // Primary arena (state at offset 0 of its 64 KB reservation) carries
  // no counter and is exempt from release here. Distinguish via TLS
  // state: the calling thread is by definition the same thread that
  // allocated the block (scratch is per-thread), so `get_thread_scratch()`
  // returns the owning state. arena_base_addr == state's base means
  // primary; anything else is one of the overflow arenas in
  // `state->overflow`.
  auto *state = get_thread_scratch();
  if (LIBC_UNLIKELY(state == nullptr))
    return; // Catastrophic — no per-thread state. Best-effort skip.
  if (arena_base_addr == reinterpret_cast<uintptr_t>(state))
    return; // Primary; no counter.

  auto *ov = reinterpret_cast<ScratchOverflowArena *>(arena_base_addr);
  // The decrement must underflow-trap if a caller frees more than was
  // allocated (a pre-existing canary failure should have caught it,
  // but cheap belt-and-suspenders).
  if (LIBC_UNLIKELY(ov->live_blocks == 0))
    __builtin_trap();
  if (--ov->live_blocks == 0)
    scratch_detail::release_empty_overflow(state, ov);
}

// ---------------------------------------------------------------------------
// ScratchAlloc<T> — RAII scratch allocation with move semantics
// ---------------------------------------------------------------------------
//
// Move-only. Supports both LIFO (stack-scoped) and non-LIFO (struct member,
// returned from function) patterns. When used in LIFO order (the common
// case), destruction hits the fast bump-rewind path. When freed out of
// order, the block goes to the arena's freelist for reuse.
//
// 16 bytes per instance (ptr + count). The arena is derived from the
// pointer on free — no state pointer stored.
//
// Usage:
//   auto buf = byte_scratch(4096);       // LIFO — same as old ScratchBuf
//   if (!buf) return -ENOMEM;
//
//   ScratchAlloc<Record> records(128);   // Non-LIFO — can be moved
//   some_struct.records = cpp::move(records);

template <typename T>
class ScratchAlloc {
  T *ptr_ = nullptr;
  size_t count_ = 0;

public:
  LIBC_INLINE explicit ScratchAlloc(size_t count) : count_(count) {
    // count==0 stays a true no-op: a 0-byte alloc still consumes a header
    // and, if later non-LIFO freed, the freelist link write at user-data
    // offset would overflow into the adjacent block's header.
    if (count == 0)
      return;
    auto *state = get_thread_scratch();
    if (LIBC_LIKELY(state != nullptr)) {
      void *raw = scratch_alloc(state, count * sizeof(T));
      ptr_ = static_cast<T *>(raw);
      if (!ptr_)
        count_ = 0;
    }
  }

  LIBC_INLINE ~ScratchAlloc() {
    if (LIBC_LIKELY(ptr_ != nullptr))
      scratch_free(ptr_, count_ * sizeof(T));
  }

  // Move-only — transfers ownership. Source is nulled out.
  // noexcept enables use in containers that require nothrow-movable types.
  LIBC_INLINE ScratchAlloc(ScratchAlloc &&o) noexcept
      : ptr_(o.ptr_), count_(o.count_) {
    o.ptr_ = nullptr;
    o.count_ = 0;
  }
  LIBC_INLINE ScratchAlloc &operator=(ScratchAlloc &&o) noexcept {
    if (this != &o) {
      if (ptr_)
        scratch_free(ptr_, count_ * sizeof(T));
      ptr_ = o.ptr_;
      count_ = o.count_;
      o.ptr_ = nullptr;
      o.count_ = 0;
    }
    return *this;
  }

  ScratchAlloc(const ScratchAlloc &) = delete;
  ScratchAlloc &operator=(const ScratchAlloc &) = delete;

  /// True if the allocation succeeded.
  LIBC_INLINE explicit operator bool() const { return ptr_ != nullptr; }

  /// Pointer to the allocated buffer.
  LIBC_INLINE T *data() { return ptr_; }
  LIBC_INLINE const T *data() const { return ptr_; }

  /// Number of T elements in the buffer.
  LIBC_INLINE size_t size() const { return count_; }

  /// Byte size of the allocation.
  LIBC_INLINE size_t size_bytes() const { return count_ * sizeof(T); }

  /// Element access.
  LIBC_INLINE T &operator[](size_t i) {
    LIBC_ASSERT(i < count_ && "ScratchAlloc: index out of bounds");
    return ptr_[i];
  }
  LIBC_INLINE const T &operator[](size_t i) const {
    LIBC_ASSERT(i < count_ && "ScratchAlloc: index out of bounds");
    return ptr_[i];
  }
};

// ---------------------------------------------------------------------------
// Convenience factories and size constants
// ---------------------------------------------------------------------------
//
// Each factory returns a ScratchAlloc that allocates from the same
// per-thread arena chain. Multiple outstanding allocations nest correctly
// and support both LIFO and non-LIFO free patterns.

// POSIX PATH_MAX in WCHARs (UTF-16). One full path conversion buffer.
// 4096 UTF-8 bytes → at most 4096 WCHARs (one UTF-8 byte → at most one
// UTF-16 code unit for BMP characters, which covers all path characters).
// Plus room for the \??\ prefix and NUL terminator.
inline constexpr size_t PATH_SCRATCH_WCHARS = 4096 + 8;

// -- path_scratch() ----------------------------------------------------------

// Allocate a WCHAR buffer for one path conversion (~8 KB).
//
// Usage:
//   auto s = path_scratch();
//   if (!s) return -ENOMEM;
//   size_t len = to_nt_path(path, s.data(), s.size());
LIBC_INLINE ScratchAlloc<WCHAR> path_scratch() {
  return ScratchAlloc<WCHAR>(PATH_SCRATCH_WCHARS);
}

// -- byte_scratch(n) ---------------------------------------------------------

// Generic byte-buffer scratch. Covers walk buffers, topology queries,
// security descriptors, reparse data, and other temporary allocations.
// Returned pointer is 16-byte aligned (sufficient for SSE, NT structs).
//
// Usage:
//   auto s = byte_scratch(8192);
//   if (!s) return -ENOMEM;
//   NtQuerySystemInformation(..., s.data(), s.size(), ...);
LIBC_INLINE ScratchAlloc<char> byte_scratch(size_t bytes) {
  return ScratchAlloc<char>(bytes);
}

// -- info_scratch<S>() -------------------------------------------------------

// Scratch for NT info struct with a trailing wide-char filename field.
// Allocates sizeof(S) + PATH_SCRATCH_WCHARS * sizeof(WCHAR) bytes.
// Covers FILE_LINK_INFORMATION, FILE_RENAME_INFORMATION,
// OBJECT_NAME_INFORMATION, and similar variable-length NT structures.
//
// Usage:
//   auto s = info_scratch<FILE_RENAME_INFORMATION>();
//   if (!s) return -ENOMEM;
//   auto *info = reinterpret_cast<FILE_RENAME_INFORMATION *>(s.data());
//   info->FileNameLength = ...;
template <typename S>
LIBC_INLINE ScratchAlloc<char> info_scratch() {
  static_assert(alignof(S) <= scratch_detail::ALLOC_ALIGN,
                "struct alignment exceeds scratch allocation alignment");
  return ScratchAlloc<char>(sizeof(S) + PATH_SCRATCH_WCHARS * sizeof(WCHAR));
}

// ---------------------------------------------------------------------------
// Cross-thread signaling
// ---------------------------------------------------------------------------
//
// These functions operate on another thread's scratch state. The caller
// obtains the ThreadScratchState pointer from the global registry walk
// or a thread registry lookup. Lock-free, O(1).

/// Request a thread to decommit demand-committed scratch pages on its
/// next allocation. Non-blocking. If the target thread is kernel-parked
/// in wait_for_change(), the alert wakes it immediately; otherwise the
/// cache-line write wakes any hardware monitor, or the flag is picked up
/// on the next scratch_alloc() call.
LIBC_INLINE void scratch_request_decommit(ThreadScratchState *target) {
  ThreadLocalWord::signal_or(&target->word, scratch_flags::DECOMMIT);
}

// ---------------------------------------------------------------------------
// Fork safety
// ---------------------------------------------------------------------------

// Reset scratch state after fork. Only the forking thread survives.
//
// Actions:
//   1. Reset all locks (may have been held by dead threads at fork snapshot).
//   2. Walk global registry:
//      - Surviving thread's state: preserved (same VA, same bump position).
//      - Dead threads' states: VA released directly via
//        `g_mapping_table.remove(base)` + `page_free(base)` for the
//        primary arena, and the same pair for every overflow in the
//        thread's overflow chain.
//   3. Rebuild registry with only the surviving thread's entry.
//
// Single-threaded at this point (only the forking thread runs in the child).
// No lock acquisition needed for the walk — we reset the locks first.
//
// Out-of-line because the dead-thread branch reaches into mapping_table.h,
// which thread_scratch.h must not transitively pull into every TU that
// uses ScratchAlloc<T>. See thread_scratch.cpp for the body.
void scratch_fork_reinit();

} // namespace internal

namespace windows {

using internal::PATH_SCRATCH_WCHARS;

LIBC_INLINE internal::ScratchAlloc<WCHAR> path_scratch() {
  return internal::path_scratch();
}

LIBC_INLINE internal::ScratchAlloc<char> byte_scratch(size_t bytes) {
  return internal::byte_scratch(bytes);
}

template <typename S> LIBC_INLINE internal::ScratchAlloc<char> info_scratch() {
  return internal::info_scratch<S>();
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_THREAD_SCRATCH_H
