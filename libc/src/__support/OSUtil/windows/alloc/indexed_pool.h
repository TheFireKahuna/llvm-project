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

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_INDEXED_POOL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_INDEXED_POOL_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
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
/// Features (carried from SlabPool where applicable):
///   - Guard pages per chunk (leading + trailing, MMU-enforced)
///   - Full-chunk decommit when empty (physical memory -> OS)
///   - Recommit on demand (zero-filled by OS, all slots appear free)
///   - Atomic occupancy bitmap (tzcnt/blsr iteration, skips 64 empty slots)
///   - Metadata separated from data by guard page (corruption isolation)
///   - Growable directory (inline 16 entries, doubles on overflow)
///   - Lock-free hot path (2 atomic loads: directory + chunk pointer)
///   - fork_reinit (walk directory, reset locks, decommit empty chunks)
///
/// T must be trivially constructible/destructible. Zero-init = empty slot.
/// ChunkShift is log2(slots per chunk). Power-of-2 enables shift/mask indexing.
template <typename T, unsigned ChunkShift>
class IndexedPool {
public:
  // --- Geometry constants (public for callers that need chunk-aware scanning) ---

  static constexpr unsigned CHUNK_SHIFT = ChunkShift;
  static constexpr unsigned SLOTS_PER_CHUNK = 1u << ChunkShift;
  static constexpr unsigned CHUNK_MASK = SLOTS_PER_CHUNK - 1;
  static constexpr unsigned BITMAP_WORDS = SLOTS_PER_CHUNK / 64;

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
    cpp::Atomic<uint32_t> transition_lock;

    // Occupancy bitmap: one bit per slot. Set atomically on alloc
    // (mark_live), cleared atomically on release (mark_dead). Enables
    // tzcnt/blsr-based iteration that skips 64 empty slots per instruction.
    //
    // For ChunkShift=13 (8192 slots): 128 words x 8 bytes = 1024 bytes.
    cpp::Atomic<uint64_t> bitmap[BITMAP_WORDS];

    // Initialize all fields. Called once when a chunk is first created
    // or recommitted after dormancy.
    LIBC_INLINE void init() {
      live_count.store(0, cpp::MemoryOrder::RELAXED);
      committed.store(1, cpp::MemoryOrder::RELAXED);
      transition_lock.store(0, cpp::MemoryOrder::RELAXED);
      for (unsigned i = 0; i < BITMAP_WORDS; ++i)
        bitmap[i].store(0, cpp::MemoryOrder::RELAXED);
    }
  };

  static_assert(sizeof(ChunkMeta) <= PAGE_SIZE,
                "ChunkMeta exceeds metadata page -- reduce ChunkShift "
                "or sizeof(T)");

  // Callback type for for_each_live iteration.
  // Receives slot index, slot pointer, and caller-supplied context.
  using SlotCallback = void (*)(unsigned idx, T *slot, void *ctx);

private:
  // -------------------------------------------------------------------------
  // Directory -- growable array of chunk slot-base pointers
  // -------------------------------------------------------------------------
  //
  // Dir header is immediately followed by `count` atomic T* entries.
  // Layout-compatible between InlineDir (embedded) and heap-allocated dirs.

  struct Dir {
    unsigned count;
    unsigned reserved_;
    LIBC_INLINE cpp::Atomic<T *> *entries() {
      return reinterpret_cast<cpp::Atomic<T *> *>(this + 1);
    }
    LIBC_INLINE const cpp::Atomic<T *> *entries() const {
      return reinterpret_cast<const cpp::Atomic<T *> *>(this + 1);
    }
  };
  static_assert(sizeof(Dir) == 8, "Dir header must be 8 bytes");

  // Inline directory: first INLINE_CAP entries embedded in the pool struct.
  // Covers INLINE_CAP * SLOTS_PER_CHUNK indices without any heap allocation.
  // For ChunkShift=13: 16 * 8192 = 131,072 fds.
  static constexpr unsigned INLINE_CAP = 16;

  struct InlineDir {
    Dir header;
    cpp::Atomic<T *> entries[INLINE_CAP];
  };
  static_assert(offsetof(InlineDir, entries) == sizeof(Dir),
                "InlineDir entries must immediately follow Dir header");

  InlineDir inline_dir_;
  cpp::Atomic<Dir *> dir_;
  cpp::Atomic<int> dir_lock_; // Spinlock for rare directory growth.

  // -------------------------------------------------------------------------
  // Chunk lifecycle helpers
  // -------------------------------------------------------------------------

  // Compute ChunkMeta pointer from a chunk's slot-base pointer.
  // slot_base = reservation_base + DATA_OFFSET. Meta is at META_OFFSET.
  LIBC_INLINE static ChunkMeta *meta_from_slots(T *slot_base) {
    return reinterpret_cast<ChunkMeta *>(
        reinterpret_cast<char *>(slot_base) - DATA_OFFSET + META_OFFSET);
  }

  // Allocate a new chunk: reserve VA, commit slot data + metadata pages.
  // Guard pages are the uncommitted regions at offset 0 (leading) and
  // DATA_OFFSET + DATA_BYTES (trailing). Both fault on access -- MMU-
  // enforced, zero runtime cost.
  // Returns the slot-base pointer (reservation + DATA_OFFSET), or nullptr.
  LIBC_INLINE static T *alloc_chunk() {
    void *base = page_reserve(RESERVE_BYTES);
    if (!base)
      return nullptr;

    auto *b = static_cast<char *>(base);

    // Commit slot data pages (zero-filled by OS).
    if (!page_commit(b + DATA_OFFSET, DATA_BYTES)) {
      page_free(base);
      return nullptr;
    }

    // Commit metadata page (separated from slots by the trailing guard).
    if (!page_commit(b + META_OFFSET, PAGE_SIZE)) {
      page_free(base);
      return nullptr;
    }

    auto *meta = reinterpret_cast<ChunkMeta *>(b + META_OFFSET);
    meta->init();

    return reinterpret_cast<T *>(b + DATA_OFFSET);
  }

  // Decommit a chunk's slot data pages. Metadata stays committed so
  // live_count/committed/bitmap remain accessible. Returns physical
  // memory to the OS; VA stays reserved. Guard pages are unaffected
  // (they were never committed).
  LIBC_INLINE static void decommit_data(T *slot_base) {
    page_decommit(slot_base, DATA_BYTES);
  }

  // Recommit a chunk's slot data pages. Zero-filled by the OS --
  // all slots appear free (zero tagged pointer = empty slot).
  LIBC_INLINE static bool recommit_data(T *slot_base) {
    return page_commit(slot_base, DATA_BYTES);
  }

  // Fully release a chunk's entire VA reservation (slot data + metadata +
  // guard pages). Used at process exit or when permanently shrinking.
  LIBC_INLINE static void free_chunk(T *slot_base) {
    page_free(reinterpret_cast<char *>(slot_base) - DATA_OFFSET);
  }

  // Take/release the transition lock for a chunk's decommit/recommit.
  LIBC_INLINE static void lock_transition(ChunkMeta *meta) {
    if (LIBC_LIKELY(!meta->transition_lock.exchange(1,
                                                     cpp::MemoryOrder::ACQUIRE)))
      return;
    for (;;) {
      spin_wait::spin_on_raw_u32(&meta->transition_lock.val, 1u);
      if (!meta->transition_lock.exchange(1, cpp::MemoryOrder::ACQUIRE))
        return;
      ::NtYieldExecution();
    }
  }

  LIBC_INLINE static void unlock_transition(ChunkMeta *meta) {
    meta->transition_lock.store(0, cpp::MemoryOrder::RELEASE);
  }

  // -------------------------------------------------------------------------
  // Directory growth
  // -------------------------------------------------------------------------

  LIBC_INLINE void lock_dir() {
    if (LIBC_LIKELY(!dir_lock_.exchange(1, cpp::MemoryOrder::ACQUIRE)))
      return;
    for (;;) {
      spin_wait::spin_on_raw_u32(&dir_lock_.val, 1);
      if (!dir_lock_.exchange(1, cpp::MemoryOrder::ACQUIRE))
        return;
      ::NtYieldExecution();
    }
  }

  LIBC_INLINE void unlock_dir() {
    dir_lock_.store(0, cpp::MemoryOrder::RELEASE);
  }

  // Grow directory to hold at least min_cap entries. Serialized by dir_lock_.
  // Old directories are intentionally leaked for lock-free reader safety
  // (growth is O(log n) so total leaked memory is negligible).
  LIBC_INLINE void grow_dir(unsigned min_cap) {
    lock_dir();

    Dir *old = dir_.load(cpp::MemoryOrder::ACQUIRE);
    if (min_cap <= old->count) {
      unlock_dir();
      return; // Already grown by another thread.
    }

    // Double until sufficient.
    unsigned new_cap = old->count;
    while (new_cap < min_cap)
      new_cap *= 2;

    size_t bytes = sizeof(Dir) + new_cap * sizeof(cpp::Atomic<T *>);
    void *mem = page_alloc(bytes);
    if (!mem) {
      unlock_dir();
      return; // OOM -- caller sees ci >= d->count and returns nullptr.
    }

    auto *nd = static_cast<Dir *>(mem);
    nd->count = new_cap;
    nd->reserved_ = 0;

    // Copy existing chunk pointers.
    for (unsigned i = 0; i < old->count; ++i)
      nd->entries()[i].store(
          old->entries()[i].load(cpp::MemoryOrder::RELAXED),
          cpp::MemoryOrder::RELAXED);

    // Zero new entries.
    for (unsigned i = old->count; i < new_cap; ++i)
      nd->entries()[i].store(nullptr, cpp::MemoryOrder::RELAXED);

    // Publish. RELEASE ensures all pointer copies are visible before the
    // new directory is reachable by readers.
    dir_.store(nd, cpp::MemoryOrder::RELEASE);

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

    // Ensure slot data is committed (may have been dormant).
    ChunkMeta *meta = meta_from_slots(slots);
    if (!meta->committed.load(cpp::MemoryOrder::ACQUIRE)) {
      lock_transition(meta);
      if (!meta->committed.load(cpp::MemoryOrder::RELAXED)) {
        if (recommit_data(slots)) {
          // Clear bitmap -- recommitted pages are zero-filled by the OS,
          // so all slots are free. The bitmap must match.
          for (unsigned i = 0; i < BITMAP_WORDS; ++i)
            meta->bitmap[i].store(0, cpp::MemoryOrder::RELAXED);
          // Publish committed=1 with RELEASE. Ensures bitmap clears are
          // visible to readers who load committed with ACQUIRE.
          meta->committed.store(1, cpp::MemoryOrder::RELEASE);
        } else {
          unlock_transition(meta);
          return nullptr; // Recommit failed (OOM).
        }
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
  // Must be called once before any other method. After init(), the pool
  // is zero-cost: no physical memory committed, only the inline directory
  // (embedded in the pool struct) is live.
  LIBC_INLINE void init() {
    inline_dir_.header.count = INLINE_CAP;
    inline_dir_.header.reserved_ = 0;
    for (unsigned i = 0; i < INLINE_CAP; ++i)
      inline_dir_.entries[i].store(nullptr, cpp::MemoryOrder::RELAXED);
    dir_.store(&inline_dir_.header, cpp::MemoryOrder::RELAXED);
    dir_lock_.store(0, cpp::MemoryOrder::RELAXED);
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
  LIBC_INLINE void mark_live(unsigned idx) {
    unsigned ci = idx >> ChunkShift;
    T *slots = dir_.load(cpp::MemoryOrder::RELAXED)
                   ->entries()[ci]
                   .load(cpp::MemoryOrder::RELAXED);
    ChunkMeta *meta = meta_from_slots(slots);

    unsigned local = idx & CHUNK_MASK;
    meta->bitmap[local / 64].fetch_or(1ULL << (local % 64),
                                       cpp::MemoryOrder::RELAXED);
    meta->live_count.fetch_add(1, cpp::MemoryOrder::RELAXED);
  }

  // Mark a slot as dead. Call after a successful release CAS on the slot.
  // Updates the bitmap (atomic fetch_and) and live count (atomic fetch_sub).
  //
  // If live_count reaches 0, decommits the chunk's slot data pages --
  // returning physical memory to the OS while preserving the VA reservation.
  // The transition is serialized by the chunk's transition_lock to prevent
  // races with a concurrent recommit from ensure_chunk_slow.
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
    ChunkMeta *meta = meta_from_slots(slots);

    unsigned local = idx & CHUNK_MASK;
    meta->bitmap[local / 64].fetch_and(~(1ULL << (local % 64)),
                                        cpp::MemoryOrder::RELAXED);

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
      // intact. If reclaimed, page_reset_undo fails and we recommit
      // (OS zero-fills, same semantics). This avoids the recommit
      // syscall for hot chunks that haven't been reclaimed (~1.8x faster).
      if (!page_reset_undo(slots, DATA_BYTES)) {
        // Pages were reclaimed — fall back to full recommit.
        if (!recommit_data(slots)) {
          meta->live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
          unlock_transition(meta);
          return nullptr;
        }
      }
      for (unsigned i = 0; i < BITMAP_WORDS; ++i)
        meta->bitmap[i].store(0, cpp::MemoryOrder::RELAXED);
      meta->committed.store(1, cpp::MemoryOrder::RELEASE);
    }
    unlock_transition(meta);
    return slots;
  }

  // Set bitmap bit only (live_count already incremented by acquire_for_scan).
  // Call after a successful alloc CAS within a pinned chunk.
  LIBC_INLINE void mark_live_bitmap_only(unsigned idx) {
    unsigned ci = idx >> ChunkShift;
    T *slots = dir_.load(cpp::MemoryOrder::RELAXED)
                   ->entries()[ci]
                   .load(cpp::MemoryOrder::RELAXED);
    ChunkMeta *meta = meta_from_slots(slots);
    unsigned local = idx & CHUNK_MASK;
    meta->bitmap[local / 64].fetch_or(1ULL << (local % 64),
                                       cpp::MemoryOrder::RELAXED);
  }

  // Release scan pin. Decrements live_count; may trigger decommit.
  // Call when the scan moves to the next chunk without allocating.
  LIBC_INLINE void release_scan_ref(unsigned ci) {
    T *slots = dir_.load(cpp::MemoryOrder::RELAXED)
                   ->entries()[ci]
                   .load(cpp::MemoryOrder::RELAXED);
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
      // Use exchange (not CAS) for the lock: if we get 0, we took it.
      // If we get 1, another thread holds it (recommitting) -- skip.
      if (meta->transition_lock.exchange(1, cpp::MemoryOrder::ACQUIRE) == 0) {
        // Double-check: between our fetch_sub and taking the lock, another
        // thread may have allocated a slot (incrementing live_count).
        if (meta->live_count.load(cpp::MemoryOrder::ACQUIRE) == 0) {
          // MEM_RESET: mark pages discardable. Content may survive (fast
          // reuse) or be zero-filled by the kernel (same as recommit).
          page_reset(slots, DATA_BYTES);
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
        uint64_t bits = meta->bitmap[w].load(cpp::MemoryOrder::ACQUIRE);
        while (bits) {
          unsigned b = static_cast<unsigned>(__builtin_ctzll(bits));
          unsigned local = w * 64 + b;
          // Prefetch next live slot. The bitmap-driven access pattern is
          // scattered -- the hardware prefetcher can't predict it.
          uint64_t next_bits = bits & (bits - 1);
          if (LIBC_LIKELY(next_bits != 0)) {
            unsigned nb =
                static_cast<unsigned>(__builtin_ctzll(next_bits));
            __builtin_prefetch(&slots[w * 64 + nb], 0, 1);
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
    // Reset directory growth lock (parent thread may have been mid-growth).
    dir_lock_.store(0, cpp::MemoryOrder::RELAXED);

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
        for (unsigned i = 0; i < BITMAP_WORDS; ++i)
          meta->bitmap[i].store(0, cpp::MemoryOrder::RELAXED);
      }
    }
  }
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_INDEXED_POOL_H
