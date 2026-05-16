//===-- Indexed pool allocator for Windows -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Power-of-2-indexed pool with guard pages, chunk-level decommit, and
// occupancy bitmap. Provides O(1) integer-indexed access with SlabPool-grade
// lifecycle management.
//
// Chunk layout (for 8-byte T, ChunkShift=13 -> 8192 slots):
//   [guard 4KB] [slots 64KB] [guard 4KB] [metadata 4KB] [unused 52KB]
//   |<----------------------- 128KB reservation ---------------------->|
//
// Total reservation: rounded up to Windows allocation granularity (64KB).
// Guard pages are uncommitted regions within the reservation -- MMU-enforced,
// zero runtime cost. The metadata page is separated from slot data by a
// guard page, preventing overflow from corrupting occupancy tracking.
//
// Lifecycle:
//   Chunk created:   slots + metadata committed (68KB physical for 8-byte T)
//   Chunk dormant:   slots decommitted, metadata retained (4KB physical)
//   Chunk freed:     entire reservation released (process exit only)
//
// When all slots in a chunk are freed (live_count reaches 0), the slot data
// pages are decommitted -- physical memory returned to the OS, virtual address
// space preserved. On the next allocation into that chunk, the pages are
// recommitted (zero-filled by the OS, so all slots appear free).
//
// The occupancy bitmap enables tzcnt/blsr-based iteration that skips 64 empty
// slots per instruction -- used by exec CLOEXEC scans and fork_reinit.
//
// Depends only on page_alloc.h (ntdll.h). No libc, no mmap, no malloc.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_LEGACY_INDEXED_POOL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_LEGACY_INDEXED_POOL_H

#include "src/__support/CPP/array.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/primitives/guarded_region.h"
#include "src/__support/OSUtil/windows/alloc/primitives/init_latch.h"
#include "src/__support/OSUtil/windows/alloc/primitives/occupancy_bitmap.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/spin_wait.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

/// IndexedPool<T, ChunkShift> -- hardened, dynamically-growable, integer-indexed
/// container with no hard capacity limit.
///
/// Features (SlabPool-grade hardening):
///   - Guard pages per chunk (leading + trailing, MMU-enforced)
///   - Zero-on-free (prevents info leaks from stale slot data)
///   - Debug-mode double-alloc/double-free detection (bitmap assertions)
///   - Full-chunk MEM_RESET when empty (physical memory -> OS, ~1.8x faster
///     than hard decommit; page_reset_undo fast path on recommit)
///   - Atomic occupancy bitmap (tzcnt/blsr iteration, skips 64 empty slots)
///   - Metadata separated from data by guard page (corruption isolation)
///   - Growable directory (inline 16 entries, VA-backed on overflow —
///     zero-leak in-place growth via demand-committed pages)
///   - Lock-free hot path (2 atomic loads: directory + chunk pointer)
///   - Thread-safe idempotent init() (CAS-guarded, matches SlabPool)
///   - LIBC_ASSERT guards on all slot lifecycle operations
///   - fork_reinit (walk directory, reset locks, decommit empty chunks)
///
/// T must be trivially constructible/destructible. Zero-init = empty slot.
/// ChunkShift is log2(slots per chunk). Power-of-2 enables shift/mask indexing.
template <typename T, unsigned ChunkShift>
class alignas(64) IndexedPool {
public:
  // --- Geometry constants (public for callers that need chunk-aware scanning) ---

  static constexpr unsigned CHUNK_SHIFT = ChunkShift;
  static constexpr unsigned SLOTS_PER_CHUNK = 1u << ChunkShift;
  static constexpr unsigned CHUNK_MASK = SLOTS_PER_CHUNK - 1;
  static constexpr unsigned BITMAP_WORDS = SLOTS_PER_CHUNK / 64;

  // Compile-time geometry constants. Intentionally shadow the runtime
  // page_size.h accessors — enables constexpr layout math and static_assert
  // validation. Mismatch with the runtime OS values traps in init().
  static constexpr size_t PAGE_SIZE = 4096;
  static constexpr size_t ALLOC_GRANULARITY = 65536;

  // Slot data region size.
  static constexpr size_t DATA_BYTES =
      static_cast<size_t>(SLOTS_PER_CHUNK) * sizeof(T);

  // Guard page size (one page on each side of slot data).
  static constexpr size_t GUARD_BYTES = PAGE_SIZE;

  // Offsets within the VA reservation.
  static constexpr size_t DATA_OFFSET = GUARD_BYTES;
  static constexpr size_t META_OFFSET =
      DATA_OFFSET + DATA_BYTES + GUARD_BYTES;

  // Total reservation, rounded up to allocation granularity.
  static constexpr size_t RESERVE_BYTES =
      (META_OFFSET + PAGE_SIZE + ALLOC_GRANULARITY - 1) &
      ~(ALLOC_GRANULARITY - 1);

  // Number of data pages per chunk.
  static constexpr unsigned DATA_PAGES = DATA_BYTES / PAGE_SIZE;

  // Trailing bytes between the meta page and the 64 KB-rounded reservation
  // edge. For T=8, ChunkShift=13 this is 52 KB. For layouts where the
  // committable regions already fill exactly RESERVE_BYTES, this is 0.
  static constexpr size_t UNUSED_TAIL_BYTES =
      RESERVE_BYTES - (META_OFFSET + PAGE_SIZE);

  // Static layout description consumed by GuardedRegion::reserve().
  //
  // Entries (every byte of RESERVE_BYTES is accounted for):
  //   [0] GUARD  at 0                          , 4 KB       (leading guard)
  //   [1] DATA   at DATA_OFFSET                , DATA_BYTES (eager commit)
  //   [2] GUARD  at DATA_OFFSET + DATA_BYTES   , 4 KB       (inter-guard)
  //   [3] META   at META_OFFSET                , 4 KB       (eager commit)
  //   [4] GUARD  at META_OFFSET + PAGE_SIZE    , UNUSED_TAIL_BYTES (if > 0)
  //
  // The array size is chosen at compile time by UNUSED_TAIL_BYTES: when
  // the meta page already lands on the 64 KB boundary the tail guard
  // entry is elided entirely (4 entries), otherwise it covers the tail
  // (5 entries). This replaces the older pattern that kept a 5-entry
  // array with a placeholder size on the unused tail and a separate
  // LAYOUT_COUNT — that shape had two sources of truth (the placeholder
  // tail size and LAYOUT_COUNT) that only is_valid() reconciled.
  static constexpr bool HAS_TAIL_GUARD = UNUSED_TAIL_BYTES > 0;

  LIBC_INLINE static constexpr auto make_chunk_entries() {
    using alloc_primitives::RegionKind;
    using alloc_primitives::RegionSpec;
    if constexpr (HAS_TAIL_GUARD) {
      return cpp::array<RegionSpec, 5>{{
          {0, GUARD_BYTES, RegionKind::GUARD, /*commit_eager=*/false},
          {DATA_OFFSET, DATA_BYTES, RegionKind::DATA, /*commit_eager=*/true},
          {DATA_OFFSET + DATA_BYTES, GUARD_BYTES, RegionKind::GUARD,
           /*commit_eager=*/false},
          {META_OFFSET, PAGE_SIZE, RegionKind::META, /*commit_eager=*/true},
          {META_OFFSET + PAGE_SIZE, UNUSED_TAIL_BYTES, RegionKind::GUARD,
           /*commit_eager=*/false},
      }};
    } else {
      return cpp::array<RegionSpec, 4>{{
          {0, GUARD_BYTES, RegionKind::GUARD, /*commit_eager=*/false},
          {DATA_OFFSET, DATA_BYTES, RegionKind::DATA, /*commit_eager=*/true},
          {DATA_OFFSET + DATA_BYTES, GUARD_BYTES, RegionKind::GUARD,
           /*commit_eager=*/false},
          {META_OFFSET, PAGE_SIZE, RegionKind::META, /*commit_eager=*/true},
      }};
    }
  }

  static constexpr auto kChunkEntries = make_chunk_entries();
  static constexpr alloc_primitives::Layout kChunkLayout{
      kChunkEntries.data(), static_cast<unsigned>(kChunkEntries.size())};
  static_assert(kChunkLayout.total_size() == RESERVE_BYTES,
                "IndexedPool chunk layout total size must equal RESERVE_BYTES");
  static_assert(kChunkLayout.is_valid(),
                "IndexedPool chunk layout failed GuardedRegion validation -- "
                "check guard placement, page alignment, or 64 KB rounding");

  // Layout-entry indices. All commit/decommit/reset ops on chunk memory
  // pass these to the GuardedRegion static typed API, which asserts the
  // indexed entry is not a GUARD — making guard-page commits
  // unrepresentable at the call site. DATA/META indices are the same
  // regardless of HAS_TAIL_GUARD because the tail guard (when present)
  // is appended after META.
  static constexpr unsigned DATA_REGION_IDX = 1;
  static constexpr unsigned META_REGION_IDX = 3;
  static_assert(kChunkEntries[DATA_REGION_IDX].kind ==
                    alloc_primitives::RegionKind::DATA,
                "DATA_REGION_IDX must name the DATA entry");
  static_assert(kChunkEntries[META_REGION_IDX].kind ==
                    alloc_primitives::RegionKind::META,
                "META_REGION_IDX must name the META entry");

  // -------------------------------------------------------------------------
  // ChunkMeta -- per-chunk metadata, lives in the metadata page within the
  // chunk reservation. Separated from slot data by a guard page.
  // -------------------------------------------------------------------------

  struct ChunkMeta {
    // Total live (allocated) slots in this chunk. Atomic for concurrent
    // alloc/release across threads. Drives chunk decommit decision:
    // when live_count reaches 0, slot data pages are decommitted.
    cpp::Atomic<uint32_t> live_count;

    // Whether slot data pages are committed. 1 = committed, 0 = decommitted.
    // Protected by transition_lock for state transitions. Readers (hot path)
    // load with ACQUIRE; writers hold transition_lock and store with RELEASE.
    cpp::Atomic<uint8_t> committed;

    // Spinlock serializing decommit/recommit transitions. Prevents races
    // between a release path decommitting and an alloc path recommitting
    // the same chunk. Only contended when both operations happen
    // simultaneously on the same chunk -- an extremely rare event.
    // alignas(64): separate from live_count/committed (hot, every
    // alloc/free) to prevent spinlock write-traffic from invalidating
    // the hot counter's cache line.
    alignas(64) cpp::Atomic<uint32_t> transition_lock;

    // Occupancy bitmap: one bit per slot. Set atomically on alloc
    // (mark_live / mark_live_bitmap_only), cleared atomically on release
    // (mark_dead). Enables tzcnt/blsr-based iteration that skips 64 empty
    // slots per instruction.
    //
    // trap_on_collision=true: mark_live traps on double-alloc and mark_dead
    // traps on double-free via the prev-value returned by the RMW at zero
    // extra cost. mark_live_bitmap_only uses the non-trapping `set()` path
    // because acquire_for_scan→CAS→mark_live_bitmap_only can observe a
    // stale set bit from mark_dead's memset-before-bit-clear ordering.
    //
    // For ChunkShift=13 (8192 slots): 128 words x 8 bytes = 1024 bytes.
    // Same layout as the pre-migration `cpp::Atomic<uint64_t>[BITMAP_WORDS]`
    // — AtomicBitmap has no extra state beyond the backing words array.
    alloc_primitives::AtomicBitmap<SLOTS_PER_CHUNK,
                                   /*trap_on_collision=*/true>
        bitmap;

    // Initialize all fields. Called once when a chunk is first created
    // or recommitted after dormancy. RELAXED stores are safe: the chunk's
    // slot-base pointer is published via a CAS (ACQ_REL) in
    // ensure_chunk_slow, ensuring all init() writes are visible to
    // readers who load the pointer with ACQUIRE.
    LIBC_INLINE void init() {
      live_count.store(0, cpp::MemoryOrder::RELAXED);
      committed.store(1, cpp::MemoryOrder::RELAXED);
      transition_lock.store(0, cpp::MemoryOrder::RELAXED);
      bitmap.clear_all();
    }
  };

  static_assert(sizeof(ChunkMeta) <= PAGE_SIZE,
                "ChunkMeta exceeds metadata page -- reduce ChunkShift "
                "or sizeof(T)");
  static_assert(offsetof(ChunkMeta, transition_lock) == 64,
                "transition_lock must be on a separate cache line from "
                "live_count/committed");

  // Callback type for for_each_live iteration.
  // Receives slot index, slot pointer, and caller-supplied context.
  using SlotCallback = void (*)(unsigned idx, T *slot, void *ctx);

  // -------------------------------------------------------------------------
  // Directory geometry (public so callers can compute their own scan caps)
  // -------------------------------------------------------------------------
  //
  // INLINE_CAP and DIR_MAX_ENTRIES are exposed because callers that scan
  // the directory linearly (e.g. RegionPool::acquire_resolved walking
  // every chunk to find a free slot) need the true ceiling. Hard-coded
  // bounds at call sites drift from the layout below and silently cap the
  // pool short of its real capacity.
  static constexpr unsigned INLINE_CAP_PUBLIC = 16;
  static constexpr size_t DIR_VA_SIZE_PUBLIC = ALLOC_GRANULARITY;
  // Mirror the private DIR_MAX_ENTRIES computation; both must stay in sync
  // (a static_assert below verifies). 16 bytes is sizeof(Dir) — kept as a
  // raw integer here to avoid a circular dependency on the still-private
  // Dir struct.
  static constexpr unsigned DIR_MAX_ENTRIES_PUBLIC = static_cast<unsigned>(
      (DIR_VA_SIZE_PUBLIC - 16) / sizeof(void *));
  static constexpr unsigned DIRECTORY_CAPACITY =
      INLINE_CAP_PUBLIC + DIR_MAX_ENTRIES_PUBLIC;

private:
  // -------------------------------------------------------------------------
  // Directory -- growable array of chunk slot-base pointers
  // -------------------------------------------------------------------------
  //
  // Two-phase design: inline directory for the common case, VA-backed
  // directory for overflow. No memory leaks on growth.
  //
  // Phase 1 (inline): INLINE_CAP entries embedded in the pool struct.
  //   Covers INLINE_CAP * SLOTS_PER_CHUNK indices with zero VA cost.
  //   For ChunkShift=13: 16 * 8192 = 131,072 indices.
  //
  // Phase 2 (VA-backed): 64KB VA reservation with demand-committed pages.
  //   Inline entries are copied once on migration. Subsequent growths
  //   commit additional pages in-place — no copies, no leaks, no old
  //   directories to reclaim. Capacity: ~8191 chunks × SLOTS_PER_CHUNK
  //   indices (67M for ChunkShift=13).
  //
  // Reader protocol: lock-free. Readers load dir_ with ACQUIRE, check
  // ci < count, then load the entry. After VA migration, dir_ points to
  // the same VA forever — growth re-stores dir_ with RELEASE to ensure
  // visibility of the updated count and zero-filled new entries.
  //
  // Dir header is immediately followed by `count` atomic T* entries.
  // Layout-compatible between InlineDir (embedded) and VA-backed dirs.

  // Default-member-initializers throughout the pool struct: required so
  // `inline IndexedPool<...> pool;` at namespace scope stays constant-
  // initialized under `-Werror=-Wglobal-constructors`. cpp::Atomic's
  // defaulted ctor leaves `val` indeterminate, which disqualifies the
  // compound implicit ctor from constant initialization — each atomic
  // must get an explicit `{0}` / `{nullptr}` to trigger the constexpr
  // value ctor. Zero-valued init preserves the original zero-init
  // semantics (every namespace-scope static starts zero anyway); the
  // only behavioural effect is keeping constant initialization intact.
  struct Dir {
    unsigned count{0};
    unsigned reserved_{0};
    // Stored pointer to the entries array. Eliminates the old `this + 1`
    // pattern which was technically UB per [expr.add] (pointer arithmetic
    // past a subobject boundary). For InlineDir, this points to the
    // embedded entries array. For VA-backed dirs, this points to the
    // memory immediately after the header (set once during growth).
    cpp::Atomic<T *> *entries_ptr{nullptr};

    LIBC_INLINE cpp::Atomic<T *> *entries() { return entries_ptr; }
    LIBC_INLINE const cpp::Atomic<T *> *entries() const { return entries_ptr; }
  };
  static_assert(sizeof(Dir) == 16, "Dir header must be 16 bytes");

  // VA reservation for the overflow directory. One allocation granularity
  // unit (64KB). Capacity: (65536 - 16) / 8 = 8190 chunk pointers.
  static constexpr size_t DIR_VA_SIZE = ALLOC_GRANULARITY;
  static constexpr unsigned DIR_MAX_ENTRIES = static_cast<unsigned>(
      (DIR_VA_SIZE - sizeof(Dir)) / sizeof(cpp::Atomic<T *>));

  // Inline directory: first INLINE_CAP entries embedded in the pool struct.
  // Covers INLINE_CAP * SLOTS_PER_CHUNK indices without any heap allocation.
  // For ChunkShift=13: 16 * 8192 = 131,072 fds.
  static constexpr unsigned INLINE_CAP = 16;

  // Mirrors of the public surrogate constants — verifies the public copy
  // tracks the true private layout. If sizeof(Dir) or
  // sizeof(cpp::Atomic<T*>) ever changes such that this trips, update
  // DIR_MAX_ENTRIES_PUBLIC's hard-coded `16` and `sizeof(void *)` to
  // match.
  static_assert(INLINE_CAP_PUBLIC == INLINE_CAP);
  static_assert(DIR_VA_SIZE_PUBLIC == DIR_VA_SIZE);
  static_assert(DIR_MAX_ENTRIES_PUBLIC == DIR_MAX_ENTRIES,
                "Public directory ceiling diverged from private layout");

  struct alignas(64) InlineDir {
    Dir header;
    // Aggregate `{}` value-initializes every atomic in the array via
    // cpp::Atomic's constexpr value ctor — required for constant init of
    // namespace-scope IndexedPool instances.
    cpp::Atomic<T *> entries[INLINE_CAP]{};
  };
  static_assert(offsetof(InlineDir, entries) == sizeof(Dir),
                "InlineDir entries must immediately follow Dir header");

  // inline_dir_ on its own cache line: hot-path slot_for() reads entries
  // without being polluted by dir_lock_ write traffic during growth.
  InlineDir inline_dir_;
  // dir_, dir_lock_, and init_done_ on a separate cache line from
  // inline_dir_ entries.
  alignas(64) cpp::Atomic<Dir *> dir_{nullptr};
  cpp::Atomic<int> dir_lock_{0}; // Spinlock for rare directory growth.
  // One-shot infallible init latch. init() uses the decomposed primitives
  // (try_begin / publish_ready / wait_ready) because the init body is
  // pure inline assignments and cannot fail — no bounded-retry needed.
  alloc_primitives::InitLatch init_done_;

  // -------------------------------------------------------------------------
  // Chunk lifecycle helpers
  // -------------------------------------------------------------------------

  // Compute ChunkMeta pointer from a chunk's slot-base pointer.
  // slot_base = reservation_base + DATA_OFFSET. Meta is at META_OFFSET.
  LIBC_INLINE static ChunkMeta *meta_from_slots(T *slot_base) {
    return reinterpret_cast<ChunkMeta *>(
        reinterpret_cast<char *>(slot_base) - DATA_OFFSET + META_OFFSET);
  }

  // Reservation base (offset 0 of the 128 KB VA region) from a chunk's
  // slot-base pointer. Inverse of `slot_base = reservation + DATA_OFFSET`.
  // Used at every commit/decommit/reset/release call site as the first
  // argument to the GuardedRegion static typed API.
  LIBC_INLINE static void *reservation_base_from(T *slot_base) {
    return reinterpret_cast<char *>(slot_base) - DATA_OFFSET;
  }

  // Allocate a new chunk: reserve VA and commit slot-data + metadata pages
  // via the shared GuardedRegion primitive. The layout (kChunkLayout) is
  // validated at compile time; reserve() performs the full reserve +
  // eager-commit + rollback-on-failure sequence. We then detach the raw
  // base pointer, because the chunk's identity inside the lock-free
  // directory is a raw T* slot pointer. Subsequent ops (decommit_data,
  // recommit_data, decommit/reset, free_chunk) all re-enter GuardedRegion
  // via its static typed API — each operation asserts kind != GUARD at
  // the API boundary, so a guard-page commit is unrepresentable even
  // if a future refactor miscomputes offsets at the call site.
  //
  // Returns the slot-base pointer (reservation + DATA_OFFSET), or nullptr
  // on reserve/commit failure.
  LIBC_INLINE static T *alloc_chunk() {
    // Runtime page-size validation. kChunkLayout is compiled against
    // PAGE_SIZE = 4096 / ALLOC_GRANULARITY = 65536; a runtime OS
    // reporting anything else would mean guards are misplaced relative
    // to the true page grain and metadata overlaps slot data — silent
    // corruption. Same trap pattern as SlabPool::init. Cold path
    // (ensure_chunk_slow), so the branch is fine.
    if (LIBC_UNLIKELY(::LIBC_NAMESPACE::windows::get_page_size() !=
                          PAGE_SIZE ||
                      ::LIBC_NAMESPACE::windows::get_alloc_granularity() !=
                          ALLOC_GRANULARITY))
      __builtin_trap();

    alloc_primitives::GuardedRegion region;
    if (!region.reserve(kChunkLayout))
      return nullptr;

    // Hand off the raw base pointer. The detached GuardedRegion instance
    // goes out of scope without releasing the memory; free_chunk takes
    // over lifetime management for the rest of the chunk's life.
    auto *b = static_cast<char *>(region.detach());

    // Meta and slot-data pages are already committed (commit_eager=true).
    // Initialize ChunkMeta in-place. RELAXED stores in init() are safe:
    // publication to other threads happens via the ACQ_REL CAS on the
    // directory entry in ensure_chunk_slow.
    auto *meta = reinterpret_cast<ChunkMeta *>(b + META_OFFSET);
    meta->init();

    return reinterpret_cast<T *>(b + DATA_OFFSET);
  }

  // Decommit a chunk's slot data pages (hard MEM_DECOMMIT). Metadata
  // stays committed so live_count/committed/bitmap remain accessible.
  // Returns physical memory to the OS; VA stays reserved. Guard pages
  // are unaffected — they are not in the DATA region, and the typed
  // API rejects attempts to touch GUARD entries via LIBC_ASSERT.
  LIBC_INLINE static void decommit_data(T *slot_base) {
    alloc_primitives::GuardedRegion::decommit_subrange(
        reservation_base_from(slot_base), kChunkLayout, DATA_REGION_IDX,
        /*offset=*/0, /*size=*/DATA_BYTES, /*reset_only=*/false);
  }

  // Recommit a chunk's slot data pages. Zero-filled by the OS --
  // all slots appear free (zero tagged pointer = empty slot).
  LIBC_INLINE static bool recommit_data(T *slot_base) {
    return alloc_primitives::GuardedRegion::commit_subrange(
        reservation_base_from(slot_base), kChunkLayout, DATA_REGION_IDX,
        /*offset=*/0, /*size=*/DATA_BYTES);
  }

  // Fully release a chunk's entire VA reservation (slot data + metadata +
  // guard pages). Used at process exit or when permanently shrinking.
  // Symmetric with alloc_chunk's GuardedRegion::reserve().
  LIBC_INLINE static void free_chunk(T *slot_base) {
    alloc_primitives::GuardedRegion::release(reservation_base_from(slot_base));
  }

  // Take/release the transition lock for a chunk's decommit/recommit.
  LIBC_INLINE static void lock_transition(ChunkMeta *meta) {
    uint32_t expected = 0;
    if (LIBC_LIKELY(meta->transition_lock.compare_exchange_weak(
            expected, 1, cpp::MemoryOrder::ACQUIRE,
            cpp::MemoryOrder::RELAXED)))
      return;
    // TTAS: hardware-monitor spin until the lock word changes, then
    // CAS. compare_exchange_weak avoids unconditional cache-line writes
    // that exchange() would cause even when the lock is already held.
    for (;;) {
      spin_wait::spin_on_raw(&meta->transition_lock.val, 1u);
      expected = 0;
      if (meta->transition_lock.load(cpp::MemoryOrder::RELAXED) == 0 &&
          meta->transition_lock.compare_exchange_weak(
              expected, 1, cpp::MemoryOrder::ACQUIRE,
              cpp::MemoryOrder::RELAXED))
        return;
    }
  }

  LIBC_INLINE static void unlock_transition(ChunkMeta *meta) {
    meta->transition_lock.store(0, cpp::MemoryOrder::RELEASE);
  }

  // -------------------------------------------------------------------------
  // Directory growth
  // -------------------------------------------------------------------------

  LIBC_INLINE void lock_dir() {
    int expected = 0;
    if (LIBC_LIKELY(dir_lock_.compare_exchange_weak(
            expected, 1, cpp::MemoryOrder::ACQUIRE,
            cpp::MemoryOrder::RELAXED)))
      return;
    // TTAS: hardware-monitor spin, then CAS. compare_exchange_weak avoids
    // unconditional cache-line writes when the lock is already held.
    for (;;) {
      spin_wait::spin_on_raw(&dir_lock_.val, 1);
      expected = 0;
      if (dir_lock_.load(cpp::MemoryOrder::RELAXED) == 0 &&
          dir_lock_.compare_exchange_weak(expected, 1,
                                          cpp::MemoryOrder::ACQUIRE,
                                          cpp::MemoryOrder::RELAXED))
        return;
    }
  }

  LIBC_INLINE void unlock_dir() {
    dir_lock_.store(0, cpp::MemoryOrder::RELEASE);
  }

  // Grow directory to hold at least min_cap entries. Serialized by dir_lock_.
  //
  // Two modes:
  //   1. Inline → VA migration: reserve 64KB, commit pages, copy inline
  //      entries once. The inline directory remains valid (embedded in the
  //      struct) so concurrent readers on the old dir_ see correct data.
  //   2. VA in-place growth: commit additional pages within the existing
  //      64KB reservation. No copies, no allocations, no leaks.
  //
  // Zero leaked memory: the inline directory is part of the struct
  // (never freed, never leaked). The VA directory is grown in-place
  // (no old copies to discard).
  LIBC_INLINE void grow_dir(unsigned min_cap) {
    lock_dir();

    Dir *cur = dir_.load(cpp::MemoryOrder::ACQUIRE);
    if (min_cap <= cur->count) {
      unlock_dir();
      return; // Already grown by another thread.
    }

    // Compute how many pages to commit for min_cap entries.
    // Round up to page boundary.
    auto commit_for = [](unsigned cap) -> size_t {
      size_t bytes = sizeof(Dir) +
                     static_cast<size_t>(cap) * sizeof(cpp::Atomic<T *>);
      size_t commit = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
      return commit > DIR_VA_SIZE ? DIR_VA_SIZE : commit;
    };

    if (cur == &inline_dir_.header) {
      // Phase 1 → Phase 2: migrate from inline to VA-backed directory.
      if (min_cap > DIR_MAX_ENTRIES) {
        unlock_dir();
        return; // Exceeds maximum capacity.
      }

      void *mem = page_reserve(DIR_VA_SIZE);
      if (!mem) {
        unlock_dir();
        return;
      }

      size_t commit_bytes = commit_for(min_cap);
      if (!page_commit(mem, commit_bytes)) {
        page_free(mem);
        unlock_dir();
        return;
      }

      auto *nd = static_cast<Dir *>(mem);
      unsigned new_count = static_cast<unsigned>(
          (commit_bytes - sizeof(Dir)) / sizeof(cpp::Atomic<T *>));
      nd->count = new_count;
      nd->reserved_ = 0;
      // Point entries_ptr into the same allocation, past the Dir header.
      // char* arithmetic on allocated storage is well-defined — no
      // subobject boundary crossing.
      nd->entries_ptr = reinterpret_cast<cpp::Atomic<T *> *>(
          static_cast<char *>(mem) + sizeof(Dir));

      // Copy inline entries. The inline directory remains valid for
      // concurrent readers that loaded dir_ before this store.
      for (unsigned i = 0; i < INLINE_CAP; ++i)
        nd->entries()[i].store(
            cur->entries()[i].load(cpp::MemoryOrder::RELAXED),
            cpp::MemoryOrder::RELAXED);

      // Zero entries sharing the first committed page with copied data.
      // Entries on freshly committed pages are already zero-filled by
      // the OS, but the first page may have entries beyond INLINE_CAP
      // that need explicit zeroing.
      unsigned first_page_entries = static_cast<unsigned>(
          (PAGE_SIZE - sizeof(Dir)) / sizeof(cpp::Atomic<T *>));
      unsigned zero_end =
          first_page_entries < new_count ? first_page_entries : new_count;
      for (unsigned i = INLINE_CAP; i < zero_end; ++i)
        nd->entries()[i].store(nullptr, cpp::MemoryOrder::RELAXED);

      // Publish. RELEASE ensures all copies/zeros are visible before
      // readers can reach the new directory.
      dir_.store(nd, cpp::MemoryOrder::RELEASE);
      unlock_dir();
      return;
    }

    // Phase 2 in-place growth: commit more pages within the existing
    // 64KB VA reservation. No copies, no leaks.
    if (min_cap > DIR_MAX_ENTRIES) {
      unlock_dir();
      return; // Exceeds maximum capacity.
    }

    size_t commit_bytes = commit_for(min_cap);
    // page_commit is idempotent on already-committed pages, so we can
    // pass the full range including previously committed pages.
    if (!page_commit(cur, commit_bytes)) {
      unlock_dir();
      return;
    }

    unsigned new_count = static_cast<unsigned>(
        (commit_bytes - sizeof(Dir)) / sizeof(cpp::Atomic<T *>));

    // Freshly committed pages are zero-filled by the OS — new entries
    // are already nullptr. Update the count.
    cur->count = new_count;

    // Re-publish dir_ with RELEASE. The pointer value is unchanged, but
    // the RELEASE/ACQUIRE pair ensures readers see the updated count
    // and zero-filled new entries. Without this, a reader could cache
    // the old count and miss the newly-available chunks.
    dir_.store(cur, cpp::MemoryOrder::RELEASE);

    unlock_dir();
  }

  // -------------------------------------------------------------------------
  // Slow path: ensure chunk exists and is committed
  // -------------------------------------------------------------------------

  LIBC_INLINE T *ensure_chunk_slow(unsigned ci) {
    // Grow directory if needed.
    Dir *d = dir_.load(cpp::MemoryOrder::ACQUIRE);
    if (ci >= d->count) {
      grow_dir(ci + 1);
      d = dir_.load(cpp::MemoryOrder::ACQUIRE);
      if (ci >= d->count)
        return nullptr; // Growth failed (OOM).
    }

    // Allocate chunk if needed.
    T *slots = d->entries()[ci].load(cpp::MemoryOrder::ACQUIRE);
    if (!slots) {
      T *fresh = alloc_chunk();
      if (!fresh)
        return nullptr;

      // CAS into directory -- another thread may have raced us.
      T *expected = nullptr;
      if (d->entries()[ci].compare_exchange_strong(
              expected, fresh, cpp::MemoryOrder::ACQ_REL)) {
        slots = fresh;
      } else {
        // Lost race -- free our allocation, use winner's chunk.
        free_chunk(fresh);
        slots = expected;
      }
    }

    // Ensure slot data is committed (may have been dormant via MEM_RESET).
    ChunkMeta *meta = meta_from_slots(slots);
    if (!meta->committed.load(cpp::MemoryOrder::ACQUIRE)) {
      lock_transition(meta);
      if (!meta->committed.load(cpp::MemoryOrder::RELAXED)) {
        // Try reset_undo first: if the kernel hasn't reclaimed the
        // MEM_RESET pages, their content (all-zero = empty slots) is
        // intact and we skip the recommit syscall (~1.8x faster).
        // Same protocol as acquire_for_scan — avoids a performance
        // asymmetry where alloc-via-ensure_chunk pays full recommit
        // while alloc-via-acquire_for_scan gets the fast undo path.
        if (!alloc_primitives::GuardedRegion::reset_undo_subrange(
                reservation_base_from(slots), kChunkLayout, DATA_REGION_IDX,
                /*offset=*/0, /*size=*/DATA_BYTES)) {
          // Pages were reclaimed — fall back to full recommit.
          if (!recommit_data(slots)) {
            unlock_transition(meta);
            return nullptr; // Recommit failed (OOM).
          }
        }
        // Clear bitmap -- pages are zero-filled (either preserved or
        // recommitted), so all slots are free. The bitmap must match.
        meta->bitmap.clear_all();
        // Publish committed=1 with RELEASE. Ensures bitmap clears are
        // visible to readers who load committed with ACQUIRE.
        meta->committed.store(1, cpp::MemoryOrder::RELEASE);
      }
      unlock_transition(meta);
    }

    return slots;
  }

public:
  // -------------------------------------------------------------------------
  // Initialization
  // -------------------------------------------------------------------------

  // Set up inline directory. No chunks allocated until first use.
  // Thread-safe and idempotent — concurrent callers are safe (second
  // caller spins until the first finishes). Matching SlabPool::init().
  // After init(), the pool is zero-cost: no physical memory committed,
  // only the inline directory (embedded in the pool struct) is live.
  LIBC_INLINE void init() {
    // Decomposed primitives: init body is infallible so we don't need
    // the bounded-retry ensure_init() protocol. wait_ready() uses
    // spin_wait::spin_on_slot_state (hardware UMWAIT/MWAITX) instead of
    // the previous ::NtYieldExecution() — strictly faster (user-mode
    // monitor vs syscall), same correctness.
    if (!init_done_.try_begin()) {
      // Another thread is initializing or already done.
      init_done_.wait_ready();
      return;
    }
    inline_dir_.header.count = INLINE_CAP;
    inline_dir_.header.reserved_ = 0;
    inline_dir_.header.entries_ptr = inline_dir_.entries;
    for (unsigned i = 0; i < INLINE_CAP; ++i)
      inline_dir_.entries[i].store(nullptr, cpp::MemoryOrder::RELAXED);
    dir_.store(&inline_dir_.header, cpp::MemoryOrder::RELAXED);
    dir_lock_.store(0, cpp::MemoryOrder::RELAXED);
    // publish_ready() RELEASE store pairs with wait_ready()'s ACQUIRE
    // load and with the ACQUIRE load that early-returns on the fast
    // path (caller calls init() once, then dir_.load(ACQUIRE) observes
    // the inline directory).
    init_done_.publish_ready();
  }

  // -------------------------------------------------------------------------
  // Hot path: look up slot by index
  // -------------------------------------------------------------------------

  // Returns slot pointer for a known-live index. Returns nullptr if the
  // chunk is not allocated. Does NOT check committed state -- the caller
  // guarantees the index is live (e.g., an open fd). If the index is live,
  // the chunk's live_count > 0 and it cannot be decommitted.
  LIBC_INLINE T *slot_for(unsigned idx) {
    unsigned ci = idx >> ChunkShift;
    Dir *d = dir_.load(cpp::MemoryOrder::ACQUIRE);
    if (ci >= d->count)
      return nullptr;
    T *slots = d->entries()[ci].load(cpp::MemoryOrder::ACQUIRE);
    if (!slots)
      return nullptr;
    return &slots[idx & CHUNK_MASK];
  }

  // -------------------------------------------------------------------------
  // Chunk-level access (for callers that scan within chunks)
  // -------------------------------------------------------------------------

  // Ensure chunk ci exists and its data pages are committed. Returns
  // the slot-base pointer (the T* for slot 0 of the chunk). Allocates
  // the chunk and/or recommits dormant data pages as needed.
  // Returns nullptr on allocation failure.
  LIBC_INLINE T *ensure_chunk(unsigned ci) {
    Dir *d = dir_.load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_LIKELY(ci < d->count)) {
      T *slots = d->entries()[ci].load(cpp::MemoryOrder::ACQUIRE);
      if (LIBC_LIKELY(slots != nullptr)) {
        ChunkMeta *meta = meta_from_slots(slots);
        if (LIBC_LIKELY(meta->committed.load(cpp::MemoryOrder::ACQUIRE) != 0))
          return slots;
      }
    }
    return ensure_chunk_slow(ci);
  }

  // Convenience: ensure slot exists and is committed.
  LIBC_INLINE T *ensure_slot(unsigned idx) {
    T *slots = ensure_chunk(idx >> ChunkShift);
    if (!slots)
      return nullptr;
    return &slots[idx & CHUNK_MASK];
  }

  // Get the slot-base pointer for a chunk (no allocation, no committed check).
  // Returns nullptr if chunk not yet allocated. For iteration over known
  // chunks only -- caller must check committed state via chunk_meta().
  LIBC_INLINE T *chunk_slots(unsigned ci) {
    Dir *d = dir_.load(cpp::MemoryOrder::ACQUIRE);
    if (ci >= d->count)
      return nullptr;
    return d->entries()[ci].load(cpp::MemoryOrder::ACQUIRE);
  }

  // Get metadata for a chunk by slot-base pointer.
  LIBC_INLINE static ChunkMeta *meta_for(T *slot_base) {
    return meta_from_slots(slot_base);
  }

  // -------------------------------------------------------------------------
  // Slot lifecycle: mark_live / mark_dead
  // -------------------------------------------------------------------------

  // Mark a slot as live. Call after a successful alloc CAS on the slot.
  // Updates the occupancy bitmap (atomic fetch_or) and live count
  // (atomic fetch_add). Both are RELAXED -- the CAS on the slot itself
  // provides the cross-thread synchronization.
  //
  // bitmap.mark_live() is the trapping variant: fetch_or returns prev,
  // traps via __builtin_trap if the bit was already 1 (double-alloc).
  // Same semantics as the pre-migration inline fetch_or+check.
  LIBC_INLINE void mark_live(unsigned idx) {
    unsigned ci = idx >> ChunkShift;
    T *slots = dir_.load(cpp::MemoryOrder::RELAXED)
                   ->entries()[ci]
                   .load(cpp::MemoryOrder::RELAXED);
    LIBC_ASSERT(slots != nullptr && "mark_live: chunk not allocated");
    ChunkMeta *meta = meta_from_slots(slots);

    unsigned local = idx & CHUNK_MASK;
    meta->bitmap.mark_live(local);
    meta->live_count.fetch_add(1, cpp::MemoryOrder::RELAXED);
  }

  // Mark a slot as dead. Clears the bitmap bit (atomic fetch_and) and
  // decrements live_count (atomic fetch_sub).
  //
  // Caller contract: the slot's storage must already be in its release
  // state (e.g. FdTable::release_slot CAS'd ptr=null before calling).
  // This function does NOT memset the slot. A post-release memset would
  // race with concurrent allocators taking the slot via CAS between the
  // caller's release and our memset — the memset would clobber the new
  // allocation. Callers that need stale-data scrubbing beyond the release
  // CAS must do it before signalling the slot as free.
  //
  // Bitmap clear is non-trapping (`bitmap.clear()`, not `mark_dead()`).
  // Rationale: a sibling install_fd/dup2 path on the same index can run
  // `mark_live_bitmap_only` between this thread's release-CAS and the
  // bit-clear below. That racer's fetch_or then this thread's fetch_and
  // leaves bit=0 with slot live — the "double-free" a later close would
  // observe is not a real double-free but a stale race-window artifact.
  // Real double-frees are caught upstream: release_slot's CAS only
  // succeeds once per live incarnation, so mark_dead is called at most
  // once per close.
  //
  // If live_count reaches 0, marks the chunk's slot data pages discardable
  // (MEM_RESET) — returning physical memory to the OS while preserving
  // the VA reservation. The transition is serialized by the chunk's
  // transition_lock to prevent races with a concurrent recommit.
  //
  // Decommit is opportunistic: if the transition_lock is already held
  // (another thread is recommitting), we skip the decommit. The
  // recommitting thread will see live_count > 0 (from its own alloc)
  // and won't re-decommit.
  LIBC_INLINE void mark_dead(unsigned idx) {
    unsigned ci = idx >> ChunkShift;
    T *slots = dir_.load(cpp::MemoryOrder::RELAXED)
                   ->entries()[ci]
                   .load(cpp::MemoryOrder::RELAXED);
    LIBC_ASSERT(slots != nullptr && "mark_dead: chunk not allocated");
    ChunkMeta *meta = meta_from_slots(slots);

    meta->bitmap.clear(idx & CHUNK_MASK);

    decrement_live_maybe_decommit(slots, meta);
  }

  // -------------------------------------------------------------------------
  // Scan pinning: prevent decommit during slot scanning
  // -------------------------------------------------------------------------
  //
  // alloc_slot scans within a chunk by loading tagged pointers and doing
  // CAS. Between ensure_chunk (which verifies committed=1) and the CAS,
  // another thread could close the last live fd and trigger decommit --
  // the CAS would fault on a decommitted page.
  //
  // acquire_for_scan pre-increments live_count to prevent decommit. If
  // an alloc succeeds, the increment becomes the slot's live reference
  // (caller only sets the bitmap via mark_live_bitmap_only, not the count).
  // If the scan moves on without allocating, release_scan_ref decrements
  // the count and may trigger decommit.

  // Ensure chunk is committed and pin it (increment live_count).
  // Returns the slot-base pointer, or nullptr on OOM.
  //
  // The pin prevents mark_dead from decommitting the data pages while
  // the caller is scanning slots. If the optimistic fetch_add sees
  // prev==0, a concurrent decommit may have already run — take the
  // transition_lock to verify and recommit if necessary.
  LIBC_INLINE T *acquire_for_scan(unsigned ci) {
    T *slots = ensure_chunk(ci);
    if (!slots)
      return nullptr;
    ChunkMeta *meta = meta_from_slots(slots);

    // Optimistic pin: increment live_count to block future decommits.
    uint32_t prev =
        meta->live_count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);

    if (LIBC_LIKELY(prev > 0))
      return slots; // Other live slots exist — no decommit possible.

    // prev was 0: a concurrent mark_dead may have decommitted between
    // ensure_chunk and our fetch_add. Serialize with the decommit path
    // via the transition_lock to resolve.
    lock_transition(meta);
    if (!meta->committed.load(cpp::MemoryOrder::RELAXED)) {
      // Data pages were MEM_RESET'd (advisory reclaim). Try to undo:
      // if pages survived, their content (all-zero = empty slots) is
      // intact. If reclaimed, reset_undo fails and we recommit (OS
      // zero-fills, same semantics). Avoids the recommit syscall for
      // hot chunks that haven't been reclaimed (~1.8x faster).
      if (!alloc_primitives::GuardedRegion::reset_undo_subrange(
              reservation_base_from(slots), kChunkLayout, DATA_REGION_IDX,
              /*offset=*/0, /*size=*/DATA_BYTES)) {
        // Pages were reclaimed — fall back to full recommit.
        if (!recommit_data(slots)) {
          meta->live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
          unlock_transition(meta);
          return nullptr;
        }
      }
      meta->bitmap.clear_all();
      meta->committed.store(1, cpp::MemoryOrder::RELEASE);
    }
    unlock_transition(meta);
    return slots;
  }

  // Set bitmap bit only (live_count already incremented by acquire_for_scan).
  // Call after a successful alloc CAS within a pinned chunk.
  //
  // Uses bitmap.set() (non-trapping fetch_or) instead of mark_live: a
  // concurrent mark_dead on the previous occupant may have cleared the
  // slot but not yet cleared the bit. Observing prev==1 here is a
  // legitimate race outcome, not a double-alloc. Matches the pre-migration
  // raw fetch_or(RELAXED) that did not inspect prev.
  LIBC_INLINE void mark_live_bitmap_only(unsigned idx) {
    unsigned ci = idx >> ChunkShift;
    T *slots = dir_.load(cpp::MemoryOrder::RELAXED)
                   ->entries()[ci]
                   .load(cpp::MemoryOrder::RELAXED);
    LIBC_ASSERT(slots != nullptr &&
                "mark_live_bitmap_only: chunk not allocated");
    ChunkMeta *meta = meta_from_slots(slots);
    meta->bitmap.set(idx & CHUNK_MASK);
  }

  // Release scan pin. Decrements live_count; may trigger decommit.
  // Call when the scan moves to the next chunk without allocating.
  LIBC_INLINE void release_scan_ref(unsigned ci) {
    T *slots = dir_.load(cpp::MemoryOrder::RELAXED)
                   ->entries()[ci]
                   .load(cpp::MemoryOrder::RELAXED);
    LIBC_ASSERT(slots != nullptr && "release_scan_ref: chunk not allocated");
    ChunkMeta *meta = meta_from_slots(slots);
    decrement_live_maybe_decommit(slots, meta);
  }

private:
  // Shared recycle logic for mark_dead and release_scan_ref.
  //
  // When the last live slot is freed, uses MEM_RESET (advisory reclaim)
  // instead of hard decommit. MEM_RESET is ~1.8x faster than decommit
  // and avoids the recommit syscall on reuse (RA14.5). The kernel may
  // reclaim pages under memory pressure (zero-filled on re-access, which
  // is correct since zero tagged pointer = empty slot), or preserve them
  // for immediate reuse if memory is available.
  LIBC_INLINE void decrement_live_maybe_decommit(T *slots, ChunkMeta *meta) {
    uint32_t old = meta->live_count.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
    if (old == 1) {
      // Was 1, now 0 -- chunk is fully empty. Try to recycle.
      // Use CAS for the lock: avoids unconditional cache-line write when
      // already held. If we get 0 → 1, we took it. Otherwise skip.
      uint32_t lock_expected = 0;
      if (meta->transition_lock.compare_exchange_strong(
              lock_expected, 1, cpp::MemoryOrder::ACQUIRE,
              cpp::MemoryOrder::RELAXED)) {
        // Double-check: between our fetch_sub and taking the lock, another
        // thread may have allocated a slot (incrementing live_count).
        if (meta->live_count.load(cpp::MemoryOrder::ACQUIRE) == 0) {
          // MEM_RESET via the typed API: mark pages discardable. Content
          // may survive (fast reuse) or be zero-filled by the kernel
          // (same as recommit). The DATA_REGION_IDX + LIBC_ASSERT guard
          // makes it impossible to reset a guard page by mistake even
          // if a future refactor changes the offset math.
          alloc_primitives::GuardedRegion::decommit_subrange(
              reservation_base_from(slots), kChunkLayout, DATA_REGION_IDX,
              /*offset=*/0, /*size=*/DATA_BYTES, /*reset_only=*/true);
          meta->committed.store(0, cpp::MemoryOrder::RELEASE);
        }
        meta->transition_lock.store(0, cpp::MemoryOrder::RELEASE);
      }
    }
  }

public:

  // -------------------------------------------------------------------------
  // Iteration via occupancy bitmap
  // -------------------------------------------------------------------------

  // Iterate all live slots via the occupancy bitmap. Uses tzcnt/blsr to
  // skip 64 empty slots per iteration. Skips unallocated chunks.
  //
  // Each chunk is pinned (live_count incremented) before the callback
  // touches its data pages, preventing concurrent decommit. Safe to call
  // without any external lock — concurrent close on another thread will
  // not fault the iteration.
  //
  // The bitmap snapshot is taken once per chunk. Concurrent alloc/release
  // may cause the iteration to miss newly-opened fds or visit
  // just-closed fds. For exec CLOEXEC (which holds the atfork lock),
  // this is not a concern.
  LIBC_INLINE void for_each_live(SlotCallback cb, void *ctx) {
    Dir *d = dir_.load(cpp::MemoryOrder::ACQUIRE);
    for (unsigned ci = 0; ci < d->count; ++ci) {
      T *slots = d->entries()[ci].load(cpp::MemoryOrder::ACQUIRE);
      if (!slots)
        continue;
      ChunkMeta *meta = meta_from_slots(slots);

      // Pin the chunk: prevents decommit while we iterate its slots.
      // Same protocol as acquire_for_scan — optimistic fetch_add, lock
      // only when prev==0 to handle the decommit race.
      uint32_t prev =
          meta->live_count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
      if (prev == 0) {
        // Possible concurrent decommit. Serialize with the decommit path.
        lock_transition(meta);
        if (!meta->committed.load(cpp::MemoryOrder::RELAXED)) {
          // Chunk is dormant and has no live slots (only our pin).
          // Nothing to iterate. Undo pin, skip this chunk.
          meta->live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
          unlock_transition(meta);
          continue;
        }
        unlock_transition(meta);
      }

      unsigned base_idx = ci << ChunkShift;
      for (unsigned w = 0; w < BITMAP_WORDS; ++w) {
        // ACQUIRE ordering pairs with mark_live_bitmap_only / mark_live's
        // RMWs: cross-thread iteration needs the set-bits to be published
        // alongside the chunk pin. word_at<ACQUIRE> compiles to the same
        // LOCK-free load as the pre-migration explicit ACQUIRE load.
        uint64_t bits =
            meta->bitmap.template word_at<cpp::MemoryOrder::ACQUIRE>(w);
        while (bits) {
          unsigned b = static_cast<unsigned>(__builtin_ctzll(bits));
          unsigned local = w * 64 + b;
          // Prefetch next live slot. The bitmap-driven access pattern is
          // scattered -- the hardware prefetcher can't predict it.
          // Locality hint 3 (L1 temporal): the callback will read the
          // slot immediately, so bring data all the way to L1 for
          // minimum-latency access. Safe even if the target slot was
          // concurrently freed: the chunk is pinned (live_count > 0)
          // so data pages remain committed. A stale prefetch is harmless.
          // Write hint (rw=1 → PREFETCHW with +prfchw): every caller of
          // for_each_live is a lifecycle sweep whose callback writes the
          // slot itself or calls mark_dead(idx), which RMWs the bitmap
          // word on the same chunk: fd_table_fini / reactor drain /
          // reactor fork reinit / timer unwatch / epoll+inotify reinit.
          // Bringing the line in M-state avoids the S→M coherence
          // upgrade the CAS/store would otherwise pay.
          uint64_t next_bits = bits & (bits - 1);
          if (LIBC_LIKELY(next_bits != 0)) {
            unsigned nb =
                static_cast<unsigned>(__builtin_ctzll(next_bits));
            __builtin_prefetch(&slots[w * 64 + nb], 1, 3);
          }
          cb(base_idx + local, &slots[local], ctx);
          bits = next_bits;
        }
      }

      // Unpin the chunk. May trigger decommit if all slots were closed
      // during our iteration.
      decrement_live_maybe_decommit(slots, meta);
    }
  }

  // -------------------------------------------------------------------------
  // Fork reinit
  // -------------------------------------------------------------------------

  // Reset after fork. Only the forking thread survives -- all locks that
  // may have been held by dead threads must be reset. Empty chunks are
  // decommitted to return physical memory inherited from the parent.
  LIBC_INLINE void fork_reinit() {
    // Reset locks (may have been held by dead threads at fork snapshot).
    dir_lock_.store(0, cpp::MemoryOrder::RELAXED);
    // init_done_ is READY — leave it. If INITIALIZING (dead thread
    // mid-init at fork time), reset to UNINIT so the surviving thread
    // can re-init.
    init_done_.fork_reinit();

    Dir *d = dir_.load(cpp::MemoryOrder::RELAXED);
    for (unsigned ci = 0; ci < d->count; ++ci) {
      T *slots = d->entries()[ci].load(cpp::MemoryOrder::RELAXED);
      if (!slots)
        continue;
      ChunkMeta *meta = meta_from_slots(slots);

      // Reset transition lock (parent may have been mid-decommit/recommit).
      meta->transition_lock.store(0, cpp::MemoryOrder::RELAXED);

      // Decommit any chunk that is committed but fully empty.
      if (meta->committed.load(cpp::MemoryOrder::RELAXED) &&
          meta->live_count.load(cpp::MemoryOrder::RELAXED) == 0) {
        decommit_data(slots);
        meta->committed.store(0, cpp::MemoryOrder::RELAXED);
        meta->bitmap.clear_all();
      }
    }
  }
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_LEGACY_INDEXED_POOL_H
