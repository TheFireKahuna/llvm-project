//===-- Slab pool allocator for Windows ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Runtime-configurable slab pool. Per-slab freelists with thread ownership
// eliminate atomics on the alloc/free hot path (SLUB/mimalloc model).
// Placeholder-backed VA lifecycle prevents use-after-release races.
//
// Slab layout (64KB, one allocation-granularity unit):
//   [header 4KB] [guard 4KB] [slots 52KB] [guard 4KB]
// Header and body are split placeholders with independent lifecycles.
// Guard pages are never-committed placeholder regions (zero cost).
//
// Hardening:
//   - Freelist pointers XOR'd with per-slab cookie + storage address on
//     BOTH local_free and xthread chains (uniform encoding)
//   - Zero-on-free prevents info leaks from reused slots
//   - Double-free canary from independent per-slab key + slot address
//   - Canary cleared on alloc to prevent false positives
//   - Guard pages isolate header from slots (underflow) and catch
//     overflow past the last slot — both MMU-enforced, zero runtime cost
//   - Released slabs become placeholders (VA stays reserved) —
//     concurrent access faults cleanly, foreign allocations blocked
//
// Lifecycle (epoch-based):
//   free() never triggers slab release. Empty abandoned slabs are
//   detected and released during alloc_slow() adoption or abandon().
//   This eliminates the class of release races between concurrent
//   freeing threads — the release decision is always single-threaded.
//
// Depends only on page_alloc.h (ntdll.h). No libc, no mmap, no malloc.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SLAB_POOL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SLAB_POOL_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/libc_assert.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Overlaid on a freed slot for freelist linkage.
struct SlabFreeNode {
  SlabFreeNode *next;
};

// Tagged-pointer constants for xthread ABA safety.
inline constexpr int kTagShift = 48;
inline constexpr uintptr_t kPtrMask = (1ULL << kTagShift) - 1;
static_assert(sizeof(void *) == 8, "tagged-pointer requires 64-bit");

// Slab layout constants.
inline constexpr size_t kSlabBytes = 65536; // Windows allocation granularity.
inline constexpr size_t kPageSize = 4096;
// [header 4KB] [guard 4KB] [slots 52KB] [guard 4KB]
inline constexpr size_t kLeadingGuardOffset = kPageSize;
inline constexpr size_t kSlotStartOffset = 2 * kPageSize;
inline constexpr size_t kTrailingGuardOffset = kSlabBytes - kPageSize;
inline constexpr size_t kUsableBytes =
    kTrailingGuardOffset - kSlotStartOffset; // 53248
inline constexpr size_t kBodyOffset = kPageSize;
inline constexpr size_t kBodySize = kSlabBytes - kPageSize; // 60KB

// Canary lives at offset 8 (after the freelist pointer).
inline constexpr size_t kCanaryOffset = sizeof(SlabFreeNode);

class SlabPool; // Forward declaration for backpointer.

// Number of slot-region pages: 52KB / 4KB = 13.
inline constexpr unsigned kSlotPages =
    (kTrailingGuardOffset - kSlotStartOffset) / kPageSize;

// Per-slab header. Cache-line split: owner fields (line 0) vs
// cross-thread atomic (line 1) to prevent false sharing.
// Page occupancy counters on line 2 (owner-only, separate from xthread).
struct SlabHeader {
  // -- Cache line 0: owner-only (no atomics) --
  SlabFreeNode *local_free;   // Owner-only freelist (raw head pointer).
  uint16_t bump;              // Next virgin slot index.
  uint16_t slots_per_slab;    // Computed from slot_size at init.
  uint32_t owner_tid;         // Thread ID (0 = abandoned).
  SlabHeader *next_abandoned; // Abandoned/recycled stack link.
  SlabHeader *all_next;       // Doubly-linked all-slabs list.
  SlabHeader *all_prev;       // Enables O(1) unlink on release.
  uintptr_t freelist_cookie;  // Per-slab XOR cookie for hardening.
  uint16_t returned;          // Slots returned (owner-only, plain).
  uint16_t slot_size;         // Byte size of each slot.
  uint8_t class_index;        // Pool identifier for multi-class dispatch.
  uint8_t pad_;
  uint16_t bump_offset;       // Random start offset for bump allocation.
  SlabPool *pool;             // Backpointer for TLS cleanup callback.

  // -- Cache line 1: cross-thread (CAS) + read-only metadata --
  alignas(64) cpp::Atomic<uintptr_t> xthread_free;
  uintptr_t canary_key;   // Independent of freelist_cookie. Read-only after init.
  // Reciprocal multiplier for slot index computation. Set once at init.
  // slot_index = (byte_offset * slot_size_recip) >> 32. Avoids a
  // runtime division (20-90 cycles) on the alloc/free bitmap hot path.
  // Formula: slot_size_recip = ceil(2^32 / slot_size).
  uint32_t slot_size_recip;

  // -- Cache line 2: per-page occupancy + state (owner-only) --
  //
  // page_occupancy: live (allocated) slots per slot-region page.
  // page_state: 2 bits per page packed in one uint32_t.
  //   0 = COMMITTED  — normal, accessible
  //   1 = SEALED     — PAGE_NOACCESS, data preserved (freelist nodes intact)
  //   2 = DECOMMITTED — PAGE_NOACCESS, physical returned, data destroyed
  //
  // Lifecycle: COMMITTED → SEALED (on last free) → DECOMMITTED (on compact)
  //            DECOMMITTED → COMMITTED (on recommit for reuse)
  //            SEALED → COMMITTED (on unseal for alloc from freelist)
  alignas(64) uint16_t page_occupancy[kSlotPages];
  uint32_t page_state; // 2 bits per page, kSlotPages * 2 ≤ 32.

  // -- Cache lines 3+: per-slot occupancy bitmap (owner-only) --
  //
  // One bit per slot. Set on alloc, cleared on free (owner path) or
  // drain_xthread (cross-thread path). Enables O(1)-per-64-slots
  // iteration via tzcnt/blsr without walking the freelist.
  //
  // Max slots = kUsableBytes / min_slot_size. With 16-byte minimum:
  //   53248 / 16 = 3328 slots → 52 uint64_t words = 416 bytes.
  // Total header footprint: 192 + 416 = 608 bytes, well within the
  // 4KB header page.
  static constexpr unsigned kMaxBitmapWords = 52;
  alignas(64) uint64_t occupancy[kMaxBitmapWords];

  // -- Accessors --
  uint32_t tid() const { return owner_tid; }
  void claim(uint32_t t) { owner_tid = t; }
  void release() { owner_tid = 0; }
};
static_assert(offsetof(SlabHeader, xthread_free) == 64,
              "xthread_free must be on a separate cache line");
static_assert(offsetof(SlabHeader, page_occupancy) == 128,
              "page_occupancy must be on its own cache line");
static_assert(offsetof(SlabHeader, occupancy) == 192,
              "occupancy bitmap must start on its own cache line");
static_assert(sizeof(SlabHeader) <= 4096,
              "SlabHeader must fit in the 4KB header page");

// ===----------------------------------------------------------------------===//
// Slab Registry — two-level radix bitmap of active slab bases.
//
// Shared by all SlabPool instances. Enables O(1) slab identification
// for free-path dispatch (slab vs large) without trusting magic values.
//
// Keys are 64KB-aligned slab base addresses. With 48-bit user VA and
// 16-bit alignment, there are 32 significant key bits (addr >> 16).
// These are split into two 16-bit levels:
//
//   L1: 65536 pointers to L2 bitmaps  (512KB VA, demand-committed)
//   L2: 65536-bit bitmap per L1 slot  (8KB each, page_alloc'd on demand)
//
// No growth, no copying, no leaks. The structure covers the full 48-bit
// user-mode address space by construction. Physical memory is proportional
// to populated 4GB VA regions only (one 8KB L2 per region).
//
// L2 bitmaps are retained for process lifetime once allocated. Reclaiming
// them would require serializing against concurrent insert() to prevent
// atomic ops on decommitted pages. Since each L2 is only 8KB and covers
// a 4GB region, the retained cost is negligible for realistic workloads.
//
// Lock-free: insert/remove use atomic bit ops. L2 allocation uses CAS
// on the L1 slot (one-time cost per 4GB region). contains() is two loads.
// ===----------------------------------------------------------------------===//

struct SlabRegistry {
  using Bitmap = cpp::Atomic<uint64_t>;

  // L2 bitmap: 65536 bits = 1024 uint64_t words = 8KB.
  // One L2 covers a 4GB VA region (65536 slabs × 64KB).
  static constexpr unsigned kL2Words = 1024;
  static constexpr size_t kL2Bytes = kL2Words * sizeof(Bitmap);

  // L1 directory: 65536 pointers. 512KB VA, demand-committed.
  static constexpr unsigned kL1Slots = 65536;
  static constexpr size_t kL1Bytes = kL1Slots * sizeof(Bitmap *);

  // L1 directory — demand-committed VA. nullptr until init().
  Bitmap **l1_{nullptr};
  mutable cpp::Atomic<uint8_t> init_state_{0}; // 0=uninit, 1=initializing, 2=ready.

  // Tracks which L1 pages have been committed (one bit per page).
  // 512KB / 4KB = 128 pages → 2 uint64_t words. Avoids redundant
  // page_commit syscalls on the contains() hot path.
  mutable cpp::Atomic<uint64_t> l1_committed_[2]{0, 0};
  static_assert(kL1Bytes / 4096 <= 128, "L1 committed bitmap too small");

  // Decompose a 64KB-aligned address into L1 index and L2 word/bit.
  struct Key {
    unsigned l1;   // L1 slot index (bits [31:16] of addr >> 16).
    unsigned word; // L2 word index (bits [15:6]).
    unsigned bit;  // Bit within the word (bits [5:0]).
  };

  LIBC_INLINE static Key decompose(uintptr_t base) {
    uint32_t k = static_cast<uint32_t>(base >> 16);
    return {k >> 16, (k & 0xFFFF) >> 6, k & 63};
  }

  LIBC_INLINE bool ensure_init() {
    uint8_t state = init_state_.load(cpp::MemoryOrder::ACQUIRE);
    if (state == 2)
      return true;
    if (state == 0) {
      uint8_t expected = 0;
      if (init_state_.compare_exchange_strong(
              expected, 1, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::ACQUIRE)) {
        void *mem = page_reserve(kL1Bytes);
        if (!mem) {
          init_state_.store(0, cpp::MemoryOrder::RELEASE);
          return false;
        }
        l1_ = static_cast<Bitmap **>(mem);
        init_state_.store(2, cpp::MemoryOrder::RELEASE);
        return true;
      }
    }
    // Another thread is initializing — wait with yield instead of bare spin.
    // Prevents priority inversion on single-core systems.
    while (init_state_.load(cpp::MemoryOrder::ACQUIRE) != 2)
      ::NtYieldExecution();
    return true;
  }

  LIBC_INLINE static unsigned l1_page_index(unsigned idx) {
    return (idx * sizeof(Bitmap *)) / 4096;
  }

  // Check if the L1 page containing slot `idx` has been committed.
  // Pure userspace — no syscall.
  LIBC_INLINE bool is_l1_committed(unsigned idx) const {
    unsigned pg = l1_page_index(idx);
    return (l1_committed_[pg / 64].load(cpp::MemoryOrder::ACQUIRE) >>
            (pg % 64)) & 1;
  }

  // Ensure the L1 page containing slot `idx` is committed.
  // Checks the bitmap first to skip the syscall on the hot path.
  LIBC_INLINE bool ensure_l1_committed(unsigned idx) {
    unsigned pg = l1_page_index(idx);
    if ((l1_committed_[pg / 64].load(cpp::MemoryOrder::ACQUIRE) >>
         (pg % 64)) & 1)
      return true;
    auto *page = reinterpret_cast<char *>(l1_) + pg * 4096;
    if (!page_commit(page, 4096))
      return false;
    l1_committed_[pg / 64].fetch_or(1ULL << (pg % 64),
                                     cpp::MemoryOrder::RELEASE);
    return true;
  }

  // Allocate an L2 bitmap for a 4GB region. CAS into L1[idx].
  // Returns the winning pointer (ours or the racer's).
  LIBC_INLINE Bitmap *ensure_l2(unsigned l1_idx) {
    if (!ensure_l1_committed(l1_idx))
      return nullptr;

    // Atomic load of the L1 slot. l1_ entries are zero-initialized
    // by page_commit (OS guarantees zero-fill on fresh commit).
    auto *l1_atomic = reinterpret_cast<cpp::Atomic<Bitmap *> *>(&l1_[l1_idx]);
    Bitmap *l2 = l1_atomic->load(cpp::MemoryOrder::ACQUIRE);
    if (l2)
      return l2;

    // Allocate fresh L2 (page_alloc returns zeroed memory).
    void *mem = page_alloc(kL2Bytes);
    if (!mem)
      return nullptr;

    auto *new_l2 = static_cast<Bitmap *>(mem);
    Bitmap *expected = nullptr;
    if (l1_atomic->compare_exchange_strong(expected, new_l2,
                                           cpp::MemoryOrder::ACQ_REL,
                                           cpp::MemoryOrder::ACQUIRE))
      return new_l2;

    // Lost the race — free ours, use the winner's.
    page_free(new_l2);
    return expected;
  }

  LIBC_INLINE void insert(uintptr_t base) {
    if (!ensure_init())
      __builtin_trap();
    Key k = decompose(base);
    Bitmap *l2 = ensure_l2(k.l1);
    if (!l2)
      __builtin_trap();
    l2[k.word].fetch_or(1ULL << k.bit, cpp::MemoryOrder::RELEASE);
  }

  LIBC_INLINE bool contains(uintptr_t base) const {
    if (init_state_.load(cpp::MemoryOrder::ACQUIRE) != 2)
      return false;
    Key k = decompose(base);

    // Fast path: if the L1 page was never committed, no slab exists
    // in this 4GB region. Pure userspace check — no syscall.
    if (!is_l1_committed(k.l1))
      return false;

    auto *l1_atomic =
        reinterpret_cast<cpp::Atomic<Bitmap *> *>(&l1_[k.l1]);
    Bitmap *l2 = l1_atomic->load(cpp::MemoryOrder::ACQUIRE);
    if (!l2)
      return false;
    return (l2[k.word].load(cpp::MemoryOrder::ACQUIRE) >> k.bit) & 1;
  }

  LIBC_INLINE void remove(uintptr_t base) {
    if (init_state_.load(cpp::MemoryOrder::ACQUIRE) != 2)
      return;
    Key k = decompose(base);

    auto *l1_atomic =
        reinterpret_cast<cpp::Atomic<Bitmap *> *>(&l1_[k.l1]);
    Bitmap *l2 = l1_atomic->load(cpp::MemoryOrder::ACQUIRE);
    if (!l2)
      return;

    l2[k.word].fetch_and(~(1ULL << k.bit), cpp::MemoryOrder::RELEASE);

    // L2 pages (8KB each) are not reclaimed when a region empties.
    // Reclamation would require serializing against concurrent insert()
    // to prevent fetch_or on decommitted memory (PAGE_NOACCESS fault).
    // Since each L2 covers a 4GB VA region and slab allocations cluster
    // in few regions, the retained cost is negligible in practice.
  }
};

// Global registry — defined in slab_registry.cpp.
extern SlabRegistry slab_registry;

// ===----------------------------------------------------------------------===//
// SlabPool — runtime-configurable slab allocator
// ===----------------------------------------------------------------------===//

class SlabPool {
public:
  using ThreadSlab = SlabHeader *;

private:
  // ABA-safe tagged-pointer stack of abandoned slabs.
  cpp::Atomic<uintptr_t> abandoned_head_{0};

  // Recycled placeholders (body released, header committed for linking).
  SlabHeader *recycled_head_{nullptr};

  // All-slabs list (doubly-linked, for fork_reinit). Cold path only.
  SlabHeader *all_slabs_head_{nullptr};
  // Spinlock (not RawMutex — avoids futex/signal circular dep).
  cpp::Atomic<int> all_slabs_lock_{0};

  // Pool configuration (set once at init, guarded by init_done_).
  uint16_t slot_size_{0};
  uint16_t slots_per_slab_{0};
  uint8_t class_index_{0};
  cpp::Atomic<uint8_t> init_done_{0}; // 0=uninit, 1=initializing, 2=ready.
  cpp::Atomic<uint8_t> tls_init_done_{0}; // Same pattern for init_tls().

  // TLS slot for per-thread slab pointer. TLS_OUT_OF_INDEXES if not enabled.
  DWORD tls_index_{TLS_OUT_OF_INDEXES};

  // -- TLS helpers (for pool-managed TLS) --

  LIBC_INLINE ThreadSlab tls_get_slab() {
    if (tls_index_ != TLS_OUT_OF_INDEXES)
      return static_cast<ThreadSlab>(teb_tls_get(tls_index_));
    return nullptr;
  }

  LIBC_INLINE void tls_set_slab(ThreadSlab slab) {
    if (tls_index_ != TLS_OUT_OF_INDEXES)
      teb_tls_set(tls_index_, slab);
  }

  // -- Lock helpers --

  LIBC_INLINE void lock_all_slabs() {
    int expected = 0;
    if (LIBC_LIKELY(all_slabs_lock_.compare_exchange_weak(
            expected, 1, cpp::MemoryOrder::ACQUIRE, cpp::MemoryOrder::RELAXED)))
      return;
    // Contended: hardware-monitor spin on the lock word, then yield.
    for (;;) {
      spin_wait::spin_on_raw_u32(&all_slabs_lock_.val, 1);
      expected = 0;
      if (all_slabs_lock_.compare_exchange_weak(
              expected, 1, cpp::MemoryOrder::ACQUIRE,
              cpp::MemoryOrder::RELAXED))
        return;
      ::NtYieldExecution();
    }
  }

  LIBC_INLINE void unlock_all_slabs() {
    all_slabs_lock_.store(0, cpp::MemoryOrder::RELEASE);
  }

  LIBC_INLINE void all_slabs_insert(SlabHeader *slab) {
    lock_all_slabs();
    slab->all_prev = nullptr;
    slab->all_next = all_slabs_head_;
    if (all_slabs_head_)
      all_slabs_head_->all_prev = slab;
    all_slabs_head_ = slab;
    unlock_all_slabs();
  }

  LIBC_INLINE void all_slabs_remove(SlabHeader *slab) {
    lock_all_slabs();
    if (slab->all_prev)
      slab->all_prev->all_next = slab->all_next;
    else
      all_slabs_head_ = slab->all_next;
    if (slab->all_next)
      slab->all_next->all_prev = slab->all_prev;
    unlock_all_slabs();
  }

  // Body → placeholder (physical returned, VA preserved).
  // Header stays committed for recycle list linkage.
  LIBC_INLINE void full_release(SlabHeader *slab) {
    all_slabs_remove(slab);
    slab_registry.remove(reinterpret_cast<uintptr_t>(slab));
    auto *body = reinterpret_cast<char *>(slab) + kBodyOffset;
    placeholder_preserve(body, kBodySize);
    lock_all_slabs();
    slab->next_abandoned = recycled_head_;
    recycled_head_ = slab;
    unlock_all_slabs();
  }

  // -- Freelist encoding (uniform across local_free and xthread) --
  //
  // Every chain link (node->next) is encoded:
  //   stored = real_next ^ cookie ^ &node->next
  //
  // Head pointers (SlabHeader::local_free, xthread head value) are RAW.
  // Since &node->next == node (first field), this simplifies to:
  //   stored = real_next ^ cookie ^ node
  //
  // Both local_free and xthread use the same encoding. Draining xthread
  // into local_free requires no per-node re-encoding — only the tail's
  // next pointer is patched to link the two chains.

  LIBC_INLINE static SlabFreeNode *encode_next(SlabFreeNode *ptr,
                                                uintptr_t cookie,
                                                SlabFreeNode **location) {
    return reinterpret_cast<SlabFreeNode *>(
        reinterpret_cast<uintptr_t>(ptr) ^ cookie ^
        reinterpret_cast<uintptr_t>(location));
  }

  LIBC_INLINE static SlabFreeNode *decode_next(SlabFreeNode *encoded,
                                                uintptr_t cookie,
                                                SlabFreeNode **location) {
    return reinterpret_cast<SlabFreeNode *>(
        reinterpret_cast<uintptr_t>(encoded) ^ cookie ^
        reinterpret_cast<uintptr_t>(location));
  }

  // -- Helpers --

  LIBC_INLINE static char *slab_slot(SlabHeader *slab, unsigned idx) {
    return reinterpret_cast<char *>(slab) + kSlotStartOffset +
           static_cast<size_t>(idx) * slab->slot_size;
  }

  // Map a slot pointer to its starting page index within the slot region.
  LIBC_INLINE static unsigned slot_page_index(SlabHeader *slab, void *slot) {
    auto offset = static_cast<size_t>(
        static_cast<char *>(slot) - reinterpret_cast<char *>(slab));
    return static_cast<unsigned>((offset - kSlotStartOffset) / kPageSize);
  }

  // Map a slot pointer to its ending page index (inclusive).
  LIBC_INLINE static unsigned slot_end_page_index(SlabHeader *slab,
                                                   void *slot) {
    auto end = static_cast<size_t>(
        static_cast<char *>(slot) + slab->slot_size - 1 -
        reinterpret_cast<char *>(slab));
    return static_cast<unsigned>((end - kSlotStartOffset) / kPageSize);
  }

  // -- Occupancy bitmap helpers --
  //
  // Physical slot index from a pointer within the slab's slot region.
  // Uses precomputed reciprocal multiplication instead of runtime
  // division: (offset * recip) >> 32. The reciprocal is exact for all
  // slot sizes that evenly divide kUsableBytes (enforced by init).
  LIBC_INLINE static unsigned slot_index(SlabHeader *slab, void *slot) {
    uint32_t byte_offset = static_cast<uint32_t>(
        static_cast<char *>(slot) -
        (reinterpret_cast<char *>(slab) + kSlotStartOffset));
    return static_cast<unsigned>(
        (static_cast<uint64_t>(byte_offset) * slab->slot_size_recip) >> 32);
  }

  // Number of bitmap words needed for this slab's slot count.
  LIBC_INLINE static unsigned bitmap_words(SlabHeader *slab) {
    return (slab->slots_per_slab + 63) / 64;
  }

  // Mark a slot as live in the occupancy bitmap.
  LIBC_INLINE static void bitmap_set(SlabHeader *slab, void *slot) {
    unsigned idx = slot_index(slab, slot);
    slab->occupancy[idx / 64] |= 1ULL << (idx % 64);
  }

  // Mark a slot as dead in the occupancy bitmap.
  LIBC_INLINE static void bitmap_clear(SlabHeader *slab, void *slot) {
    unsigned idx = slot_index(slab, slot);
    slab->occupancy[idx / 64] &= ~(1ULL << (idx % 64));
  }

  // Increment page_occupancy for all pages a slot spans.
  LIBC_INLINE static void slot_occupy(SlabHeader *slab, void *slot) {
    unsigned first = slot_page_index(slab, slot);
    unsigned last = slot_end_page_index(slab, slot);
    for (unsigned pg = first; pg <= last && pg < kSlotPages; pg++)
      slab->page_occupancy[pg]++;
  }

  // Decrement page_occupancy for all pages a slot spans.
  // Returns true if ANY page hit zero (caller may want to seal).
  LIBC_INLINE static bool slot_vacate(SlabHeader *slab, void *slot) {
    unsigned first = slot_page_index(slab, slot);
    unsigned last = slot_end_page_index(slab, slot);
    bool any_empty = false;
    for (unsigned pg = first; pg <= last && pg < kSlotPages; pg++) {
      if (--slab->page_occupancy[pg] == 0) {
        seal_slot_page(slab, pg);
        any_empty = true;
      }
    }
    return any_empty;
  }

  // Base address of a slot-region page.
  LIBC_INLINE static void *slot_page_base(SlabHeader *slab, unsigned pg) {
    return reinterpret_cast<char *>(slab) + kSlotStartOffset + pg * kPageSize;
  }

  // Page states (2 bits each, packed in page_state).
  static constexpr unsigned kPageCommitted = 0;
  static constexpr unsigned kPageSealed = 1;
  static constexpr unsigned kPageDecommitted = 2;

  LIBC_INLINE static unsigned get_page_state(SlabHeader *slab, unsigned pg) {
    return (slab->page_state >> (pg * 2)) & 3;
  }

  LIBC_INLINE static void set_page_state(SlabHeader *slab, unsigned pg,
                                          unsigned state) {
    uint32_t mask = 3U << (pg * 2);
    slab->page_state = (slab->page_state & ~mask) | (state << (pg * 2));
  }

  // Seal a slot-region page (PAGE_NOACCESS). Data preserved.
  LIBC_INLINE static void seal_slot_page(SlabHeader *slab, unsigned pg) {
    page_protect(slot_page_base(slab, pg), kPageSize, PAGE_NOACCESS);
    set_page_state(slab, pg, kPageSealed);
  }

  // Unseal a sealed page (restore PAGE_READWRITE). Freelist data intact.
  LIBC_INLINE static void unseal_slot_page(SlabHeader *slab, unsigned pg) {
    page_protect(slot_page_base(slab, pg), kPageSize, PAGE_READWRITE);
    set_page_state(slab, pg, kPageCommitted);
  }

  // Decommit a sealed page. Physical memory returned. Data destroyed.
  // UAF protection persists (page stays PAGE_NOACCESS).
  LIBC_INLINE static void decommit_slot_page(SlabHeader *slab, unsigned pg) {
    page_decommit(slot_page_base(slab, pg), kPageSize);
    set_page_state(slab, pg, kPageDecommitted);
  }

  // Recommit a decommitted page. Zero-filled by OS. Slots available
  // for bump-style allocation (no freelist nodes — they were destroyed).
  LIBC_INLINE static bool recommit_slot_page(SlabHeader *slab, unsigned pg) {
    if (!page_commit(slot_page_base(slab, pg), kPageSize))
      return false;
    set_page_state(slab, pg, kPageCommitted);
    return true;
  }

  // Ensure a slot's page is accessible for allocator-internal reads.
  // Only unseals sealed pages. Decommitted pages are NOT touched here —
  // they require recommit + freelist reconstruction (handled by caller).
  LIBC_INLINE static void ensure_slot_accessible(SlabHeader *slab,
                                                  void *slot) {
    unsigned pg = slot_page_index(slab, slot);
    if (get_page_state(slab, pg) == kPageSealed)
      unseal_slot_page(slab, pg);
  }

  LIBC_INLINE static uint32_t current_tid() {
#if defined(__x86_64__)
    uint32_t tid;
    LIBC_INLINE_ASM("movl %%gs:0x48, %0" : "=r"(tid));
    return tid;
#elif defined(__aarch64__)
    uint64_t tid;
    LIBC_INLINE_ASM("ldr %0, [x18, #0x48]" : "=r"(tid));
    return static_cast<uint32_t>(tid);
#endif
  }

  LIBC_INLINE void init_slab(SlabHeader *slab) {
    slab->local_free = nullptr;
    slab->xthread_free.store(0, cpp::MemoryOrder::RELAXED);
    slab->bump = 0;
    slab->slots_per_slab = slots_per_slab_;
    slab->slot_size = slot_size_;
    // Reciprocal: ceil(2^32 / slot_size). Exact for power-of-2 sizes;
    // for non-power-of-2, rounding up ensures (offset * recip) >> 32
    // gives the correct floor(offset / slot_size) for all in-range offsets.
    slab->slot_size_recip =
        static_cast<uint32_t>((0x100000000ULL + slot_size_ - 1) / slot_size_);
    slab->class_index = class_index_;
    slab->returned = 0;
    slab->pool = this;
    slab->claim(current_tid());
    slab->next_abandoned = nullptr;
    slab->all_next = nullptr;
    slab->all_prev = nullptr;
    // Seed per-slab secrets from OS entropy (independent keys).
    ::ProcessPrng(reinterpret_cast<unsigned char *>(&slab->freelist_cookie),
                  sizeof(slab->freelist_cookie));
    if (slab->freelist_cookie == 0)
      slab->freelist_cookie = 0xDEAD'BEEF'CAFE'BABE;
    ::ProcessPrng(reinterpret_cast<unsigned char *>(&slab->canary_key),
                  sizeof(slab->canary_key));
    if (slab->canary_key == 0)
      slab->canary_key = 0xBADC'0FFE'E0DD'F00D;
    // Randomized bump start for heap spray resistance.
    uint16_t rand_val;
    ::ProcessPrng(reinterpret_cast<unsigned char *>(&rand_val),
                  sizeof(rand_val));
    slab->bump_offset = rand_val % slots_per_slab_;
    // Zero page occupancy, state, and slot bitmap — all pages start
    // committed with zero live slots. (Fresh slabs have just been
    // committed; bump alloc will populate them.)
    for (unsigned i = 0; i < kSlotPages; i++)
      slab->page_occupancy[i] = 0;
    slab->page_state = 0; // All pages kPageCommitted.
    unsigned bwords = bitmap_words(slab);
    for (unsigned i = 0; i < bwords; i++)
      slab->occupancy[i] = 0;
  }

  // Drain xthread_free into local_free. Both chains use the same
  // encoding (cookie + &node->next), so internal links need no
  // re-encoding. Only the tail is patched to connect to local_free.
  // Decrement page_occupancy without sealing. Used by drain_xthread
  // where we must keep pages accessible while walking the freelist chain.
  LIBC_INLINE static void slot_vacate_noseal(SlabHeader *slab, void *slot) {
    unsigned first = slot_page_index(slab, slot);
    unsigned last = slot_end_page_index(slab, slot);
    for (unsigned pg = first; pg <= last && pg < kSlotPages; pg++)
      --slab->page_occupancy[pg];
  }

  LIBC_INLINE static unsigned drain_xthread(SlabHeader *slab) {
    uintptr_t ck = slab->freelist_cookie;
    uintptr_t old = slab->xthread_free.exchange(0, cpp::MemoryOrder::ACQUIRE);
    auto *node = reinterpret_cast<SlabFreeNode *>(old & kPtrMask);
    if (!node)
      return 0;

    // Walk to tail and count. Unseal pages as needed — cross-thread
    // frees may have returned slots to pages the owner already sealed.
    // Decrement page occupancy for each drained slot (cross-thread frees
    // don't decrement the owner-only counter).
    // Clear occupancy bitmap bits deferred from the non-owner free path.
    //
    // Page sealing is NOT done here. slot_vacate can seal a page
    // (PAGE_NOACCESS) when occupancy drops to zero, but the walk needs
    // to read node->next pointers and the tail-patch writes tail->next,
    // so all chain nodes must remain accessible throughout. Sealing is
    // deferred to the next compact_sealed_pages pass (abandon, alloc
    // slow path, etc.) where no live freelist pointers are being chased.
    ensure_slot_accessible(slab, node);
    bitmap_clear(slab, node);
    slot_vacate_noseal(slab, node);
    unsigned count = 1;
    auto *tail = node;
    for (;;) {
      auto *next = decode_next(tail->next, ck, &tail->next);
      if (!next)
        break;
      ensure_slot_accessible(slab, next);
      bitmap_clear(slab, next);
      slot_vacate_noseal(slab, next);
      tail = next;
      count++;
    }

    // Patch tail to link to existing local_free.
    if (slab->local_free)
      tail->next = encode_next(slab->local_free, ck, &tail->next);
    // else: tail->next already encodes nullptr — leave it.

    slab->local_free = node;
    slab->returned += count;
    return count;
  }

  // Pop one node from local_free. Unseals the page if sealed,
  // verifies canary (detects UAF writes), then clears it.
  // Decommitted pages never have freelist nodes (compaction removes them).
  LIBC_INLINE static void *local_free_pop(SlabHeader *slab) {
    SlabFreeNode *node = slab->local_free;

    // Unseal all pages the slot spans. Must happen before reading node->next.
    unsigned first_pg = slot_page_index(slab, node);
    unsigned last_pg = slot_end_page_index(slab, node);
    for (unsigned pg = first_pg; pg <= last_pg && pg < kSlotPages; pg++) {
      if (get_page_state(slab, pg) == kPageSealed)
        unseal_slot_page(slab, pg);
      slab->page_occupancy[pg]++;
    }
    // Re-allocating a returned slot: adjust epoch counter so
    // returned == bump never fires while slots are still live.
    slab->returned--;

    uintptr_t ck = slab->freelist_cookie;
    slab->local_free = decode_next(node->next, ck, &node->next);

    // Verify then clear canary. A mismatch means a UAF write corrupted
    // the slot between free() and this alloc.
    if (slab->slot_size >= kCanaryOffset + sizeof(uintptr_t)) {
      auto *canary_ptr = reinterpret_cast<uintptr_t *>(
          reinterpret_cast<char *>(node) + kCanaryOffset);
      if (*canary_ptr != make_canary(slab, node))
        __builtin_trap(); // UAF write detected.
      *canary_ptr = 0;
    }
    bitmap_set(slab, node);
    return node;
  }

  // Compact sealed pages: unlink their freelist nodes, then decommit.
  // Returns physical memory to the OS while preserving UAF protection
  // (decommitted pages are still PAGE_NOACCESS). Called on the slow path.
  LIBC_INLINE static void compact_sealed_pages(SlabHeader *slab) {
    // Quick check: any sealed pages?
    if (slab->page_state == 0)
      return; // All pages committed, nothing to compact.

    // Snapshot which pages are sealed, then unseal them all so the
    // freelist walk can read node data.
    uint16_t sealed_mask = 0;
    for (unsigned pg = 0; pg < kSlotPages; pg++) {
      if (get_page_state(slab, pg) == kPageSealed) {
        sealed_mask |= static_cast<uint16_t>(1U << pg);
        page_protect(slot_page_base(slab, pg), kPageSize, PAGE_READWRITE);
      }
    }
    if (sealed_mask == 0)
      return;

    uintptr_t ck = slab->freelist_cookie;

    // Walk local_free, rebuild chain excluding nodes on sealed pages.
    SlabFreeNode *new_head = nullptr;
    SlabFreeNode *new_tail = nullptr;
    SlabFreeNode *node = slab->local_free;
    unsigned removed = 0;

    while (node) {
      unsigned pg = slot_page_index(slab, node);
      SlabFreeNode *next = decode_next(node->next, ck, &node->next);

      if (sealed_mask & (1U << pg)) {
        removed++;
      } else {
        if (new_tail) {
          new_tail->next = encode_next(node, ck, &new_tail->next);
        } else {
          new_head = node;
        }
        new_tail = node;
        node->next = encode_next(nullptr, ck, &node->next);
      }
      node = next;
    }

    slab->local_free = new_head;
    slab->returned -= removed;

    // Decommit sealed pages. Physical memory returned, UAF protection
    // preserved (decommitted pages are PAGE_NOACCESS).
    for (unsigned pg = 0; pg < kSlotPages; pg++) {
      if (sealed_mask & (1U << pg))
        decommit_slot_page(slab, pg);
    }
  }

  // Recommit a decommitted page and add its slots to local_free.
  // Returns the first slot (for immediate use by alloc), or nullptr.
  LIBC_INLINE static void *recommit_page_slots(SlabHeader *slab) {
    for (unsigned pg = 0; pg < kSlotPages; pg++) {
      if (get_page_state(slab, pg) != kPageDecommitted)
        continue;
      if (!recommit_slot_page(slab, pg))
        continue;

      // Enumerate valid slot offsets on this page.
      size_t page_start = kSlotStartOffset + pg * kPageSize;
      size_t page_end = page_start + kPageSize;
      uint16_t ss = slab->slot_size;
      uintptr_t ck = slab->freelist_cookie;

      void *first = nullptr;
      unsigned pushed = 0;
      for (size_t off = page_start; off + ss <= page_end; off += ss) {
        size_t slot_off = off - kSlotStartOffset;
        if (slot_off % ss != 0)
          continue;
        auto *slot = reinterpret_cast<char *>(slab) + off;

        if (!first) {
          // First slot is returned directly to the caller.
          first = slot;
          slot_occupy(slab, slot);
          bitmap_set(slab, slot);
          continue;
        }
        // Remaining slots go to local_free with proper canary.
        auto *node = reinterpret_cast<SlabFreeNode *>(slot);
        node->next = encode_next(slab->local_free, ck, &node->next);
        if (ss >= kCanaryOffset + sizeof(uintptr_t)) {
          *reinterpret_cast<uintptr_t *>(slot + kCanaryOffset) =
              make_canary(slab, slot);
        }
        slab->local_free = node;
        pushed++;
      }
      // Restore returned count for slots re-added to freelist.
      // (Compaction subtracted them; they're free again now.)
      slab->returned += pushed;
      return first;
    }
    return nullptr;
  }

  // Allocate from a specific slab (owner thread).
  LIBC_INLINE static void *alloc_from_slab(SlabHeader *slab) {
    // 1. Pop from local_free (zero atomics, decode hardened pointer).
    if (slab->local_free)
      return local_free_pop(slab);

    // 2. Drain cross-thread returns (one atomic exchange).
    if (slab->xthread_free.load(cpp::MemoryOrder::RELAXED) != 0) {
      drain_xthread(slab);
      if (slab->local_free)
        return local_free_pop(slab);
    }

    // 3. Bump allocate from virgin slots (zero atomics).
    // bump_offset randomizes the starting slot within the slab.
    if (slab->bump < slab->slots_per_slab) {
      unsigned raw = slab->bump++;
      unsigned idx = (raw + slab->bump_offset) % slab->slots_per_slab;
      char *slot = slab_slot(slab, idx);
      slot_occupy(slab, slot);
      bitmap_set(slab, slot);
      return slot;
    }

    // 4. Compact sealed pages → decommit (returns physical memory,
    // preserves UAF protection). Then recommit one for fresh slots.
    compact_sealed_pages(slab);
    void *slot = recommit_page_slots(slab);
    if (slot)
      return slot;

    return nullptr; // Slab fully exhausted (all pages live).
  }

  // Push to xthread_free (encoded, ABA-safe tagged CAS).
  LIBC_INLINE static void xthread_push(SlabHeader *slab, SlabFreeNode *node) {
    uintptr_t ck = slab->freelist_cookie;
    uintptr_t old = slab->xthread_free.load(cpp::MemoryOrder::RELAXED);
    for (;;) {
      auto *old_head = reinterpret_cast<SlabFreeNode *>(old & kPtrMask);
      node->next = encode_next(old_head, ck, &node->next);
      uintptr_t tag = (old >> kTagShift) + 1;
      uintptr_t desired =
          reinterpret_cast<uintptr_t>(node) | (tag << kTagShift);
      if (slab->xthread_free.compare_exchange_weak(
              old, desired, cpp::MemoryOrder::RELEASE,
              cpp::MemoryOrder::RELAXED))
        return;
      spin_wait::relax_processor();
    }
  }

  // Push an encoded chain [head..tail] to xthread_free (one CAS).
  // Tail's next is re-encoded on each retry to point at the current head.
  LIBC_INLINE static void xthread_push_chain(SlabHeader *slab,
                                              SlabFreeNode *head,
                                              SlabFreeNode *tail) {
    uintptr_t ck = slab->freelist_cookie;
    uintptr_t old = slab->xthread_free.load(cpp::MemoryOrder::RELAXED);
    for (;;) {
      auto *old_head = reinterpret_cast<SlabFreeNode *>(old & kPtrMask);
      tail->next = encode_next(old_head, ck, &tail->next);
      uintptr_t tag = (old >> kTagShift) + 1;
      uintptr_t desired =
          reinterpret_cast<uintptr_t>(head) | (tag << kTagShift);
      if (slab->xthread_free.compare_exchange_weak(
              old, desired, cpp::MemoryOrder::RELEASE,
              cpp::MemoryOrder::RELAXED))
        return;
      spin_wait::relax_processor();
    }
  }

  // Double-free check + canary write. Shared by both harden paths.
  LIBC_INLINE static void check_and_set_canary(void *slot, SlabHeader *slab) {
    if (slab->slot_size >= kCanaryOffset + sizeof(uintptr_t)) {
      auto *cp = reinterpret_cast<uintptr_t *>(static_cast<char *>(slot) +
                                                kCanaryOffset);
      if (*cp == make_canary(slab, slot))
        __builtin_trap(); // Double-free detected.
      *cp = make_canary(slab, slot);
    }
  }

  // Full hardening: zero + canary. Used when the page stays committed
  // (other slots are live — data must be scrubbed for info leak prevention).
  LIBC_INLINE static void harden_slot(void *slot, SlabHeader *slab) {
    check_and_set_canary(slot, slab);
    __builtin_memset(slot, 0, slab->slot_size);
    // Re-set canary (memset cleared it).
    if (slab->slot_size >= kCanaryOffset + sizeof(uintptr_t)) {
      *reinterpret_cast<uintptr_t *>(static_cast<char *>(slot) +
                                     kCanaryOffset) =
          make_canary(slab, slot);
    }
  }

  // Canary-only hardening: no memset. Used when this free will seal the
  // page (PAGE_NOACCESS) — no info leak window. The kernel zeros the
  // page on eventual recommit from the zero-page list.
  LIBC_INLINE static void harden_slot_seal(void *slot, SlabHeader *slab) {
    check_and_set_canary(slot, slab);
  }

public:
  /// Derive slab header from any pointer within the slab.
  LIBC_INLINE static SlabHeader *ptr_to_slab(void *ptr) {
    return reinterpret_cast<SlabHeader *>(
        reinterpret_cast<uintptr_t>(ptr) &
        ~static_cast<uintptr_t>(kSlabBytes - 1));
  }

  /// Encode a freelist next pointer (public for posix_alloc thread cache).
  LIBC_INLINE static SlabFreeNode *
  encode_free_next(SlabFreeNode *ptr, uintptr_t cookie,
                   SlabFreeNode **location) {
    return encode_next(ptr, cookie, location);
  }

  /// Decode a freelist next pointer (public for posix_alloc thread cache).
  LIBC_INLINE static SlabFreeNode *
  decode_free_next(SlabFreeNode *encoded, uintptr_t cookie,
                   SlabFreeNode **location) {
    return decode_next(encoded, cookie, location);
  }

  /// Read thread ID from TEB without syscall (public for posix_alloc).
  LIBC_INLINE static uint32_t get_current_tid() { return current_tid(); }

  /// Compute per-slot canary (public for posix_alloc alloc-time verification).
  LIBC_INLINE static uintptr_t make_canary(SlabHeader *slab, void *slot) {
    return slab->canary_key ^ reinterpret_cast<uintptr_t>(slot);
  }

  /// Push an encoded chain [head..tail] onto xthread_free with one CAS.
  /// Public for posix_alloc batched drain.
  LIBC_INLINE static void push_chain_xthread(SlabHeader *slab,
                                              SlabFreeNode *head,
                                              SlabFreeNode *tail) {
    xthread_push_chain(slab, head, tail);
  }

  /// Apply hardening (canary check + zero + canary place) to a slot.
  /// Public for posix_alloc which manages its own thread cache.
  LIBC_INLINE static void harden_freed_slot(void *slot, SlabHeader *slab) {
    harden_slot(slot, slab);
  }

  /// Validate that a slot pointer falls within the slab's slot range
  /// and that the slab's pool/class metadata is self-consistent.
  LIBC_INLINE static void validate_slot(void *slot, SlabHeader *slab) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(slot);
    uintptr_t base = reinterpret_cast<uintptr_t>(slab);
    if (addr < base + kSlotStartOffset || addr >= base + kTrailingGuardOffset)
      __builtin_trap(); // Pointer outside slab slot range.
    // Cross-pool / corruption check: slab's class must match its pool's.
    if (slab->pool->class_index_ != slab->class_index)
      __builtin_trap();
  }

  /// Count live (allocated) slots in a slab via occupancy bitmap.
  LIBC_INLINE static unsigned popcount_live(SlabHeader *slab) {
    unsigned count = 0;
    unsigned words = bitmap_words(slab);
    for (unsigned w = 0; w < words; w++)
      count += static_cast<unsigned>(__builtin_popcountll(slab->occupancy[w]));
    return count;
  }

  /// Verify bitmap consistency after drain_xthread(). Two checks:
  ///   1. Total popcount must equal (bump - returned).
  ///   2. Every slot on local_free must have its bit CLEAR.
  /// Compiled out in release (LIBC_ASSERT). Catches both missing
  /// set/clear calls and swapped-bit corruption that total-only
  /// checking would miss.
  LIBC_INLINE static void assert_bitmap_consistent(
      [[maybe_unused]] SlabHeader *slab) {
#ifndef NDEBUG
    // Check 1: totals.
    LIBC_ASSERT(popcount_live(slab) ==
                    static_cast<unsigned>(slab->bump - slab->returned) &&
                "occupancy bitmap popcount diverged from alloc/free count");
    // Check 2: every free slot has its bit clear.
    uintptr_t ck = slab->freelist_cookie;
    SlabFreeNode *node = slab->local_free;
    while (node) {
      unsigned idx = slot_index(slab, node);
      LIBC_ASSERT(((slab->occupancy[idx / 64] >> (idx % 64)) & 1) == 0 &&
                  "free slot has occupancy bit set");
      // Unseal if needed to read next pointer (sealed pages are
      // PAGE_NOACCESS). Only needed for the debug walk.
      ensure_slot_accessible(slab, node);
      node = decode_next(node->next, ck, &node->next);
    }
#endif
  }

  /// Callback type for for_each_live. Function pointer + opaque context,
  /// matching the ReactorCallback pattern used throughout the codebase.
  /// No templates — avoids code bloat from the bitmap scan loop.
  using SlabSlotCallback = void (*)(void *slot, void *ctx);

  /// Iterate all live slots in a slab. Scans the occupancy bitmap with
  /// tzcnt/blsr — skips 64 dead slots per word in a single instruction.
  ///
  /// The bitmap is owner-only. Cross-thread frees are not reflected
  /// until drain_xthread() runs. Callers that need an exact view
  /// should drain first (fork_reinit, compaction, diagnostics).
  ///
  /// Early-terminates when all live slots have been visited (avoids
  /// scanning trailing empty bitmap words on sparse slabs).
  /// Prefetches the next slot's cache line while processing the
  /// current one to hide memory latency on the scattered access pattern.
  LIBC_INLINE static void for_each_live(SlabHeader *slab,
                                         SlabSlotCallback cb, void *ctx) {
    unsigned words = bitmap_words(slab);
    unsigned remaining = static_cast<unsigned>(slab->bump - slab->returned);
    for (unsigned w = 0; w < words && remaining > 0; w++) {
      uint64_t bits = slab->occupancy[w];
      while (bits) {
        unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
        unsigned idx = w * 64 + bit;
        // Prefetch next live slot while processing current one.
        // The bitmap-driven access pattern is scattered — the hardware
        // prefetcher can't predict it. One cache line ahead is enough.
        uint64_t next_bits = bits & (bits - 1);
        if (LIBC_LIKELY(next_bits != 0)) {
          unsigned next_bit = static_cast<unsigned>(__builtin_ctzll(next_bits));
          __builtin_prefetch(slab_slot(slab, w * 64 + next_bit), 0, 1);
        }
        cb(static_cast<void *>(slab_slot(slab, idx)), ctx);
        bits = next_bits;
        if (--remaining == 0)
          return;
      }
    }
  }

  /// Walk all live slots across every slab owned by this pool.
  /// Acquires the all-slabs spinlock for the duration of the walk.
  /// The callback may return early by setting *ctx to a sentinel —
  /// use a FindResult pattern (see named_semaphore.cpp for example).
  LIBC_INLINE void for_each_live_all(SlabSlotCallback cb, void *ctx) {
    lock_all_slabs();
    SlabHeader *slab = all_slabs_head_;
    while (slab) {
      for_each_live(slab, cb, ctx);
      slab = slab->all_next;
    }
    unlock_all_slabs();
  }

  // TLS cleanup callback for pool-managed TLS. The slab's pool
  // backpointer routes the abandon to the correct pool instance.
  static void NTAPI fls_abandon_callback(PVOID val) {
    auto *slab = static_cast<SlabHeader *>(val);
    if (slab && slab->pool)
      slab->pool->abandon(slab);
  }

  /// Initialize with a given slot size. Thread-safe and idempotent —
  /// concurrent calls with the same arguments are safe (second caller
  /// spins until the first finishes).
  LIBC_INLINE void init(size_t slot_size,
                        size_t slot_align = sizeof(void *),
                        uint8_t class_index = 0) {
    uint8_t expected = 0;
    if (!init_done_.compare_exchange_strong(expected, 1,
                                            cpp::MemoryOrder::ACQ_REL,
                                            cpp::MemoryOrder::RELAXED)) {
      // Another thread is initializing or already done — spin if needed.
      if (expected == 1) {
        while (init_done_.load(cpp::MemoryOrder::ACQUIRE) < 2)
          ::NtYieldExecution();
      }
      return;
    }
    // Slab layout is compiled against kPageSize. Trap unconditionally if
    // the runtime page size differs — guard/commit math silently corrupts.
    // Cannot be static_assert (runtime OS query); must survive NDEBUG.
    if (LIBC_UNLIKELY(windows::get_page_size() != kPageSize ||
                      windows::get_alloc_granularity() != kSlabBytes))
      __builtin_trap();

    slot_size_ = static_cast<uint16_t>(slot_size);
    slots_per_slab_ = static_cast<uint16_t>(kUsableBytes / slot_size);
    class_index_ = class_index;
    (void)slot_align;
    init_done_.store(2, cpp::MemoryOrder::RELEASE);
  }

  /// Enable pool-managed TLS. Allocates a TEB TLS slot for
  /// single-instruction hot path reads + .CRT$XLC cleanup.
  /// Thread-safe and idempotent.
  LIBC_INLINE void init_tls() {
    uint8_t expected = 0;
    if (!tls_init_done_.compare_exchange_strong(
            expected, 1, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::RELAXED)) {
      if (expected == 1)
        while (tls_init_done_.load(cpp::MemoryOrder::ACQUIRE) < 2)
          ::NtYieldExecution();
      return;
    }
    tls_index_ = internal::tls_alloc();
    if (tls_index_ != TLS_OUT_OF_INDEXES)
      internal::tls_cleanup_register(tls_index_, fls_abandon_callback);
    tls_init_done_.store(2, cpp::MemoryOrder::RELEASE);
  }

  /// Allocate using pool-managed TLS (call init_tls() first).
  /// Reads the thread's slab from TEB, allocates, updates TLS on slow path.
  LIBC_INLINE void *tls_alloc() {
    ThreadSlab slab = tls_get_slab();
    void *slot = alloc(slab);
    if (!slot) {
      slot = alloc_slow(&slab);
      if (slot)
        tls_set_slab(slab);
    }
    return slot;
  }

  /// Release all recycled placeholders.
  LIBC_INLINE void destroy() {
    lock_all_slabs();
    while (recycled_head_) {
      SlabHeader *slab = recycled_head_;
      recycled_head_ = slab->next_abandoned;
      page_free(slab);
    }
    unlock_all_slabs();
  }

  /// Allocate from the thread's current slab (fast path).
  /// Returns nullptr if slab is null or exhausted — call alloc_slow().
  LIBC_INLINE static void *alloc(ThreadSlab slab) {
    if (!slab)
      return nullptr;
    return alloc_from_slab(slab);
  }

  // Push a slab onto the abandoned stack (one CAS).
  LIBC_INLINE void push_abandoned(SlabHeader *slab) {
    uintptr_t head = abandoned_head_.load(cpp::MemoryOrder::RELAXED);
    for (;;) {
      slab->next_abandoned =
          reinterpret_cast<SlabHeader *>(head & kPtrMask);
      uintptr_t tag = (head >> kTagShift) + 1;
      uintptr_t desired =
          reinterpret_cast<uintptr_t>(slab) | (tag << kTagShift);
      if (abandoned_head_.compare_exchange_weak(
              head, desired, cpp::MemoryOrder::RELEASE,
              cpp::MemoryOrder::RELAXED))
        return;
      spin_wait::relax_processor();
    }
  }

  /// Slow path: try adopting an abandoned slab, then allocate fresh.
  /// Updates *current_slab for future fast-path allocs.
  LIBC_INLINE void *alloc_slow(ThreadSlab *current_slab) {
    // 0. Compact sealed pages on the current slab. This decommits pages
    // whose slots are all freed, returning physical memory to the OS.
    // Then alloc_from_slab step 4 can recommit one for fresh slots.
    if (*current_slab) {
      compact_sealed_pages(*current_slab);
      void *slot = alloc_from_slab(*current_slab);
      if (slot)
        return slot;
    }

    // 1. Try to adopt abandoned slabs (bounded attempts).
    constexpr unsigned kMaxAdoptAttempts = 3;
    unsigned attempts = 0;

    uintptr_t head = abandoned_head_.load(cpp::MemoryOrder::ACQUIRE);
    while ((head & kPtrMask) && attempts < kMaxAdoptAttempts) {
      auto *slab = reinterpret_cast<SlabHeader *>(head & kPtrMask);
      uintptr_t tag = (head >> kTagShift) + 1;
      uintptr_t next = reinterpret_cast<uintptr_t>(slab->next_abandoned) |
                        (tag << kTagShift);
      if (abandoned_head_.compare_exchange_weak(head, next,
                                                cpp::MemoryOrder::ACQ_REL)) {
        // Adopt: take ownership.
        slab->claim(current_tid());
        slab->next_abandoned = nullptr;

        // Drain cross-thread returns.
        drain_xthread(slab);

        // Epoch check: if fully empty after drain, release entirely.
        // Single-threaded — slab is off the abandoned stack and claimed.
        if (slab->returned == slab->bump) {
          full_release(slab);
          attempts++;
          head = abandoned_head_.load(cpp::MemoryOrder::ACQUIRE);
          continue;
        }

        *current_slab = slab;
        void *slot = alloc_from_slab(slab);
        if (slot)
          return slot;
        // Slab fully consumed — re-abandon and try next.
        slab->release();
        push_abandoned(slab);
        attempts++;
        head = abandoned_head_.load(cpp::MemoryOrder::ACQUIRE);
        continue;
      }
    }

    // 2. Try recycled placeholder (header committed, body is placeholder).
    {
      lock_all_slabs();
      SlabHeader *recycled = recycled_head_;
      if (recycled) {
        recycled_head_ = recycled->next_abandoned;
        unlock_all_slabs();

        auto *base = reinterpret_cast<char *>(recycled);
        if (placeholder_commit(base + kBodyOffset, kBodySize)) {
          // Decommit guard pages within the body.
          page_decommit(base + kLeadingGuardOffset, kPageSize);
          page_decommit(base + kTrailingGuardOffset, kPageSize);
          // Reinitialize header (header page was committed throughout).
          init_slab(recycled);
          all_slabs_insert(recycled);
          slab_registry.insert(reinterpret_cast<uintptr_t>(recycled));
          *current_slab = recycled;
          return alloc_from_slab(recycled);
        }
        // Commit failed — fully release this placeholder.
        page_free(recycled);
      } else {
        unlock_all_slabs();
      }
    }

    // 3. Allocate a fresh slab via placeholder.
    void *mem = placeholder_reserve(kSlabBytes);
    if (!mem)
      return nullptr;
    auto *base = static_cast<char *>(mem);

    // Split at 4KB: [header 4KB placeholder] [body 60KB placeholder]
    if (!placeholder_split(base, kPageSize)) {
      page_free(mem);
      return nullptr;
    }

    // Commit header.
    if (!placeholder_commit(base, kPageSize)) {
      // After split: [header 4KB] [body 60KB]. Free both halves.
      page_free(base + kBodyOffset);
      page_free(mem);
      return nullptr;
    }

    // Commit body.
    if (!placeholder_commit(base + kBodyOffset, kBodySize)) {
      // Header is committed (no longer a placeholder) — free it directly.
      // Body is still a placeholder — free it separately.
      page_free(base + kBodyOffset);
      page_free(mem);
      return nullptr;
    }

    // Decommit guard pages (PAGE_NOACCESS).
    page_decommit(base + kLeadingGuardOffset, kPageSize);
    page_decommit(base + kTrailingGuardOffset, kPageSize);

    auto *slab = static_cast<SlabHeader *>(mem);
    init_slab(slab);
    all_slabs_insert(slab);
    slab_registry.insert(reinterpret_cast<uintptr_t>(slab));

    *current_slab = slab;
    return alloc_from_slab(slab);
  }

  /// Free a slot. Zeros, checks canary, routes to local_free (owner) or
  /// xthread (non-owner). Epoch-based: never triggers slab release.
  /// Seals the slot's page if all slots on it are now freed.
  LIBC_INLINE static void free(void *slot) {
    if (!slot)
      return;

    SlabHeader *slab = ptr_to_slab(slot);
    validate_slot(slot, slab);

    if (slab->tid() == current_tid()) {
      // Owner path: check if ALL pages the slot spans will seal.
      unsigned first_pg = slot_page_index(slab, slot);
      unsigned last_pg = slot_end_page_index(slab, slot);
      bool will_seal = true;
      for (unsigned pg = first_pg; pg <= last_pg && pg < kSlotPages; pg++) {
        if (slab->page_occupancy[pg] != 1) {
          will_seal = false;
          break;
        }
      }

      if (will_seal)
        harden_slot_seal(slot, slab); // Canary only — kernel zeros on recommit.
      else
        harden_slot(slot, slab);      // Full zero + canary.

      auto *node = static_cast<SlabFreeNode *>(slot);
      node->next = encode_next(slab->local_free, slab->freelist_cookie,
                               &node->next);
      slab->local_free = node;
      slab->returned++;

      bitmap_clear(slab, slot);
      slot_vacate(slab, slot);
    } else {
      // Non-owner: always full zero (can't read owner-only counter).
      // Bitmap bit stays set — cleared when owner drains xthread.
      harden_slot(slot, slab);
      xthread_push(slab, static_cast<SlabFreeNode *>(slot));
    }
  }

  /// Push a slot to local_free or xthread with canary but no memset.
  /// For use by posix_alloc drain paths (slots already zeroed-on-free
  /// when the user originally freed them).
  LIBC_INLINE static void raw_free(void *slot) {
    SlabHeader *slab = ptr_to_slab(slot);
    validate_slot(slot, slab);

    // Set canary so alloc-time verification passes.
    if (slab->slot_size >= kCanaryOffset + sizeof(uintptr_t)) {
      *reinterpret_cast<uintptr_t *>(static_cast<char *>(slot) +
                                     kCanaryOffset) =
          make_canary(slab, slot);
    }

    auto *node = static_cast<SlabFreeNode *>(slot);
    uintptr_t ck = slab->freelist_cookie;

    if (slab->tid() == current_tid()) {
      node->next = encode_next(slab->local_free, ck, &node->next);
      slab->local_free = node;
      slab->returned++;
      bitmap_clear(slab, slot);
      slot_vacate(slab, slot);
    } else {
      // Bitmap bit stays set — cleared when owner drains xthread.
      xthread_push(slab, node);
    }
  }

  /// Abandon a slab (thread exit). Drains xthread, compacts, checks empty.
  LIBC_INLINE void abandon(ThreadSlab slab) {
    if (!slab)
      return;

    // Drain pending xthread frees (updates returned + bitmap).
    drain_xthread(slab);
    assert_bitmap_consistent(slab);
    // Compact sealed pages before abandoning — return physical memory.
    compact_sealed_pages(slab);
    slab->release();

    // Epoch check: if all slots returned, release immediately.
    if (slab->returned == slab->bump) {
      full_release(slab);
      return;
    }

    // Outstanding slots exist — push to abandoned stack for adoption.
    push_abandoned(slab);
  }

  /// Release a slab if fully empty (returned == bump). For use by
  /// external drain paths that detect empty non-active slabs.
  /// Caller must be the owner thread.
  LIBC_INLINE void release_if_empty(SlabHeader *slab) {
    if (slab->returned == slab->bump)
      full_release(slab);
  }

  /// Reset after fork. Only the forking thread survives.
  LIBC_INLINE void fork_reinit() {
    uint32_t my_tid = current_tid();

    // Clear the abandoned stack.
    abandoned_head_.store(0, cpp::MemoryOrder::RELAXED);

    // Reset the spinlock (may have been held by a dead thread).
    all_slabs_lock_.store(0, cpp::MemoryOrder::RELAXED);

    // Walk all slabs. Single-threaded — no lock needed.
    SlabHeader *slab = all_slabs_head_;
    while (slab) {
      SlabHeader *next = slab->all_next;
      if (slab->tid() != 0 && slab->tid() != my_tid) {
        // Dead-thread slab: release ownership, drain xthread returns,
        // then either free (if fully empty) or push to abandoned.
        slab->release();
        drain_xthread(slab);
        if (slab->returned == slab->bump) {
          full_release(slab);
        } else {
          push_abandoned(slab);
        }
      } else if (slab->tid() == my_tid) {
        // Self slab: drain any cross-thread returns that were pushed
        // by dead threads before the fork snapshot.  Without this,
        // those slots are stranded in xthread_free until the next
        // alloc_from_slab triggers a drain — leaking slots and
        // preventing page compaction.
        drain_xthread(slab);
      }
      slab = next;
    }
  }
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SLAB_POOL_H
