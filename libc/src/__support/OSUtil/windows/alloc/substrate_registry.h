//===-- VaSubstrate arena-membership registry -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Three-level radix bitmap that answers `contains(ptr)` in three atomic
// loads. Used by VaSubstrate::release() as the first step before any
// dereference derived from a caller-supplied pointer — if the pointer
// is not inside a registered arena, the release path traps before the
// access violation turns a caller's bug into our problem.
//
// Layout modelled on SlabRegistry (slab_pool.h:505-811). The two live
// side by side and share no state: SlabRegistry tracks per-slab 64 KB
// cells, the substrate registry tracks per-arena 64 KB cells (arena
// sizes are always multiples of the 64 KB allocation granularity, so
// insert_range stamps every cell in the arena's VA range).
//
// Key = (arena_base >> 16). L1 is runtime-sized from PCB max_address,
// with one 4 KB page covering a 4 TB region; L2 is a 1024-entry
// directory covering a 4 TB region; L3 is an 8 KB bitmap covering a
// 4 GB region. L2 and L3 pages are demand-allocated and never reclaimed
// — reclaiming would require serializing against concurrent insert()
// against a decommitted page (PAGE_NOACCESS fault); since the substrate
// holds ≤ a few dozen arenas in practice, the retained L2/L3 footprint
// is bounded by a single-digit number of 8 KB pages.
//
// Lock-free: insert/remove are single `fetch_or` / `fetch_and`; contains
// is three ACQUIRE loads plus one bit test. L2/L3 first-populate uses
// CAS to install the new page and gracefully retries on lost race.
//
// Depends only on page_alloc.h (NT primitives) and PCB Zone 0 accessors
// — no libc dependencies, safe to call during Phase 0a bootstrap.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SUBSTRATE_REGISTRY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SUBSTRATE_REGISTRY_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/primitives/init_latch.h"
#include "src/__support/OSUtil/windows/process_control_block_access.h"
#include "src/__support/common.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

class SubstrateRegistry {
public:
  LIBC_INLINE SubstrateRegistry() = default;
  SubstrateRegistry(const SubstrateRegistry &) = delete;
  SubstrateRegistry &operator=(const SubstrateRegistry &) = delete;
  SubstrateRegistry(SubstrateRegistry &&) = delete;
  SubstrateRegistry &operator=(SubstrateRegistry &&) = delete;

  // Lazy init: allocate the L1 directory and compute the clamp mask.
  // Safe to call repeatedly; infallible by construction (every failure
  // inside traps immediately). Idempotent via InitLatch.
  LIBC_INLINE void ensure_init() {
    if (LIBC_LIKELY(init_latch_.is_ready()))
      return;
    if (!init_latch_.try_begin()) {
      init_latch_.wait_ready();
      return;
    }
    // Winner path. All failure edges trap.
    size_t l1_count = required_l1_size();
    size_t l1_rounded = round_up_pow2(l1_count);
    size_t l1_bytes = l1_rounded * sizeof(L1Entry);
    void *mem = internal::page_reserve(l1_bytes);
    if (!mem)
      __builtin_trap();
    if (!internal::page_commit(mem, l1_bytes))
      __builtin_trap();
    l1_.store(static_cast<L1Entry *>(mem), cpp::MemoryOrder::RELAXED);
    l1_mask_ = l1_rounded - 1;
    // l1_ and l1_mask_ are transitively published by publish_ready().
    init_latch_.publish_ready();
  }

  // Single-cell insert. `base` must be 64 KB-aligned. One fetch_or RMW
  // at steady state; may trigger at most one L2 and one L3 first-populate.
  LIBC_INLINE void insert(uintptr_t base) {
    ensure_init();
    Key k = decompose(base);
    BitmapWord *l3 = locate_or_create_l3(k);
    if (!l3)
      __builtin_trap();
    l3[k.word].fetch_or(bit_mask(k.bit), cpp::MemoryOrder::RELEASE);
  }

  // Multi-cell insert — stamps every 64 KB cell in [base, base + size).
  // `base` must be 64 KB-aligned; `size` must be a positive multiple of
  // 64 KB. Coalesces bits that share the same L3 word (up to 64 cells =
  // 4 MB span per atomic op).
  LIBC_INLINE void insert_range(uintptr_t base, size_t size) {
    LIBC_ASSERT((base & (kCellBytes - 1)) == 0 &&
                "SubstrateRegistry::insert_range: base must be 64 KB-aligned");
    LIBC_ASSERT(size > 0 && (size & (kCellBytes - 1)) == 0 &&
                "SubstrateRegistry::insert_range: size must be a positive "
                "multiple of 64 KB");
    ensure_init();
    uintptr_t cursor = base;
    uintptr_t end = base + size;
    while (cursor < end) {
      Key k = decompose(cursor);
      BitmapWord *l3 = locate_or_create_l3(k);
      if (!l3)
        __builtin_trap();
      uint64_t mask = compute_word_mask(k.bit, cursor, end);
      l3[k.word].fetch_or(mask, cpp::MemoryOrder::RELEASE);
      cursor = advance_cursor(cursor, k.bit, end);
    }
  }

  // Single-cell remove. Silent no-op if the cell was never inserted.
  LIBC_INLINE void remove(uintptr_t base) {
    if (!init_latch_.is_ready())
      return;
    Key k = decompose(base);
    BitmapWord *l3 = locate_l3(k);
    if (!l3)
      return;
    l3[k.word].fetch_and(~bit_mask(k.bit), cpp::MemoryOrder::RELEASE);
  }

  // Multi-cell remove — inverse of insert_range. Silently skips ranges
  // whose L2 or L3 never got populated; a mismatched remove is a no-op,
  // not an error. Matches SlabRegistry::remove_range's contract.
  LIBC_INLINE void remove_range(uintptr_t base, size_t size) {
    LIBC_ASSERT((base & (kCellBytes - 1)) == 0 &&
                "SubstrateRegistry::remove_range: base must be 64 KB-aligned");
    LIBC_ASSERT(size > 0 && (size & (kCellBytes - 1)) == 0 &&
                "SubstrateRegistry::remove_range: size must be a positive "
                "multiple of 64 KB");
    if (!init_latch_.is_ready())
      return;
    uintptr_t cursor = base;
    uintptr_t end = base + size;
    while (cursor < end) {
      Key k = decompose(cursor);
      BitmapWord *l3 = locate_l3(k);
      if (!l3) {
        cursor = skip_to_next_l3_region(cursor);
        continue;
      }
      uint64_t mask = compute_word_mask(k.bit, cursor, end);
      l3[k.word].fetch_and(~mask, cpp::MemoryOrder::RELEASE);
      cursor = advance_cursor(cursor, k.bit, end);
    }
  }

  // Hot-path membership test. Three ACQUIRE loads + one bit test. Returns
  // false for any address outside the currently-populated L2/L3 regions,
  // including addresses outside the VA range the L1 mask covers.
  [[nodiscard]] LIBC_INLINE bool contains(uintptr_t base) const {
    if (!init_latch_.is_ready())
      return false;
    Key k = decompose(base);
    L1Entry *l1 = l1_.load(cpp::MemoryOrder::RELAXED);
    L2Entry *l2 = l1[k.l1].load(cpp::MemoryOrder::ACQUIRE);
    if (!l2)
      return false;
    BitmapWord *l3 = l2[k.l2].load(cpp::MemoryOrder::ACQUIRE);
    if (!l3)
      return false;
    return (l3[k.word].load(cpp::MemoryOrder::ACQUIRE) >> k.bit) & 1ULL;
  }

private:
  // --- Geometry -----------------------------------------------------------

  // 64 KB cells — NT allocation granularity. Every substrate arena VA is
  // aligned to this and covers a whole number of cells.
  static constexpr size_t kCellBytes = 65536;

  // L3: 1024 atomic<uint64_t> words = 8 KB; covers 64 K bits = 4 GB VA.
  static constexpr unsigned kL3Words = 1024;
  static constexpr size_t kL3Bytes = kL3Words * sizeof(cpp::Atomic<uint64_t>);
  static constexpr unsigned kL3KeyBits = 16; // bit index within L3

  // L2: 1024 atomic pointers = 8 KB; covers 1024 L3s = 4 TB VA.
  static constexpr unsigned kL2Slots = 1024;
  static constexpr size_t kL2Bytes = kL2Slots * sizeof(void *);
  static constexpr unsigned kL2KeyBits = 10;

  // L1 is runtime-sized from PCB max_address.
  static constexpr unsigned kL2Mask = (1U << kL2KeyBits) - 1;
  static constexpr unsigned kL3Mask = (1U << kL3KeyBits) - 1;

  using BitmapWord = cpp::Atomic<uint64_t>;
  using L2Entry = cpp::Atomic<BitmapWord *>;
  using L1Entry = cpp::Atomic<L2Entry *>;

  // Compute the ceiling L1 size (in entries) required to cover every VA
  // up to g_pcb.zone0.max_address(). For 48-bit VA this is 512 entries
  // = 4 KB; for 57-bit VA it scales to ~16 K entries = 128 KB.
  LIBC_INLINE static size_t required_l1_size() {
    uintptr_t max_addr = reinterpret_cast<uintptr_t>(internal::pcb_max_address());
    uintptr_t max_key = max_addr >> 16;
    size_t max_l1 = static_cast<size_t>(max_key >> (kL2KeyBits + kL3KeyBits));
    return max_l1 + 1;
  }

  LIBC_INLINE static size_t round_up_pow2(size_t v) {
    if ((v & (v - 1)) == 0)
      return v;
    return static_cast<size_t>(1) << (64 - __builtin_clzll(v));
  }

  // Bit mask for a single cell.
  LIBC_INLINE static uint64_t bit_mask(unsigned bit) {
    return uint64_t{1} << bit;
  }

  struct Key {
    size_t l1;
    unsigned l2;
    unsigned word;
    unsigned bit;
  };

  LIBC_INLINE Key decompose(uintptr_t base) const {
    uintptr_t k = base >> 16;
    return Key{
        static_cast<size_t>(k >> (kL2KeyBits + kL3KeyBits)) & l1_mask_,
        static_cast<unsigned>((k >> kL3KeyBits) & kL2Mask),
        static_cast<unsigned>((k & kL3Mask) >> 6),
        static_cast<unsigned>(k & 63U),
    };
  }

  // Compute the fetch_or/fetch_and mask for [cursor, word_end) clipped to
  // [cursor, end). Handles up to 64 cells packed into one L3 word.
  LIBC_INLINE static uint64_t compute_word_mask(unsigned start_bit,
                                                uintptr_t cursor,
                                                uintptr_t end) {
    uintptr_t span_cells = 64U - start_bit;
    uintptr_t word_end = cursor + span_cells * kCellBytes;
    if (word_end > end)
      word_end = end;
    uintptr_t cells = (word_end - cursor) / kCellBytes;
    return (cells == 64U) ? ~uint64_t{0}
                          : (((uint64_t{1} << cells) - 1ULL) << start_bit);
  }

  // Advance the insert/remove cursor past the span covered by
  // `compute_word_mask`. Same arithmetic, isolated so the two callers
  // share one definition.
  LIBC_INLINE static uintptr_t advance_cursor(uintptr_t cursor,
                                              unsigned start_bit,
                                              uintptr_t end) {
    uintptr_t span_cells = 64U - start_bit;
    uintptr_t word_end = cursor + span_cells * kCellBytes;
    return word_end > end ? end : word_end;
  }

  // Used by remove_range on a missing L2/L3: skip to the next L3 region.
  // L3 region is 4 GB = 64 K cells × 64 KB — round up to the next 4 GB
  // boundary.
  LIBC_INLINE static uintptr_t skip_to_next_l3_region(uintptr_t cursor) {
    static constexpr uintptr_t kL3Bytes_va = uintptr_t{1} << 32;
    return (cursor & ~(kL3Bytes_va - 1)) + kL3Bytes_va;
  }

  // Look up an existing L3 bitmap page. Returns nullptr if L2 or L3 is
  // absent. Fast path for `remove` / `contains`.
  LIBC_INLINE BitmapWord *locate_l3(const Key &k) const {
    L1Entry *l1 = l1_.load(cpp::MemoryOrder::RELAXED);
    L2Entry *l2 = l1[k.l1].load(cpp::MemoryOrder::ACQUIRE);
    if (!l2)
      return nullptr;
    return l2[k.l2].load(cpp::MemoryOrder::ACQUIRE);
  }

  // Look up an L3 bitmap page, allocating L2/L3 on demand. Used by insert
  // paths. May return nullptr on allocation failure; caller traps.
  LIBC_INLINE BitmapWord *locate_or_create_l3(const Key &k) {
    L1Entry *l1 = l1_.load(cpp::MemoryOrder::RELAXED);
    L2Entry *l2 = l1[k.l1].load(cpp::MemoryOrder::ACQUIRE);
    if (!l2) {
      l2 = ensure_l2(k.l1);
      if (!l2)
        return nullptr;
    }
    BitmapWord *l3 = l2[k.l2].load(cpp::MemoryOrder::ACQUIRE);
    if (!l3) {
      l3 = ensure_l3(l2, k.l2);
      if (!l3)
        return nullptr;
    }
    return l3;
  }

  // CAS-install a freshly-allocated L2 page. Loser path frees its own
  // allocation and uses the winner's page. Lock-free first-populate.
  LIBC_INLINE L2Entry *ensure_l2(size_t l1_idx) {
    L1Entry *l1 = l1_.load(cpp::MemoryOrder::RELAXED);
    L2Entry *existing = l1[l1_idx].load(cpp::MemoryOrder::ACQUIRE);
    if (existing)
      return existing;

    void *mem = internal::page_alloc(kL2Bytes);
    if (!mem)
      return nullptr;

    L2Entry *fresh = static_cast<L2Entry *>(mem);
    L2Entry *expected = nullptr;
    if (l1[l1_idx].compare_exchange_strong(expected, fresh,
                                           cpp::MemoryOrder::ACQ_REL,
                                           cpp::MemoryOrder::ACQUIRE))
      return fresh;

    internal::page_free(fresh);
    return expected;
  }

  LIBC_INLINE BitmapWord *ensure_l3(L2Entry *l2, unsigned l2_idx) {
    BitmapWord *existing = l2[l2_idx].load(cpp::MemoryOrder::ACQUIRE);
    if (existing)
      return existing;

    void *mem = internal::page_alloc(kL3Bytes);
    if (!mem)
      return nullptr;

    BitmapWord *fresh = static_cast<BitmapWord *>(mem);
    BitmapWord *expected = nullptr;
    if (l2[l2_idx].compare_exchange_strong(expected, fresh,
                                           cpp::MemoryOrder::ACQ_REL,
                                           cpp::MemoryOrder::ACQUIRE))
      return fresh;

    internal::page_free(fresh);
    return expected;
  }

  // --- State --------------------------------------------------------------

  mutable cpp::Atomic<L1Entry *> l1_{nullptr};
  internal::alloc_primitives::InitLatch init_latch_;
  size_t l1_mask_{0};
};

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SUBSTRATE_REGISTRY_H
