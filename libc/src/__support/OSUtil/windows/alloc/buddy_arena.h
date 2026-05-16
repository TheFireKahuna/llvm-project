//===-- Lock-free buddy chunk broker (Layer 2) ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Brokers raw NT VA (sourced via the nt_pal PAL) into power-of-two chunks in
// [64 KiB, 4 MiB] for segment / slab_page / huge-bypass. One 4 GiB partition
// per arena, carved by a 1-indexed flat binary tree of single-byte NBALLOC
// status words.
//
// Algorithms: NBALLOC for lock-free split/coalesce (Marotta et al.,
// arXiv:1804.03436, 2018); vmem-style boundary-tag-free instant-fit (Bonwick
// & Adams, USENIX ATC 2001) — metadata out-of-band in a sealed descriptor
// pool plus the pagemap; Crystalline-W (Nikolaev & Ravindran, PLDI 2024)
// gates chunk-granularity reclamation so a wait-free reader holding a tagged
// pointer never observes freed metadata.
//
// In-arena chunks live inside a single plain `MEM_RESERVE | MEM_WRITE_WATCH`
// partition VAD (not a placeholder): per-chunk MEM_COMMIT inherits the
// dirty bitmap from the VAD-level arming, feeding quarantine sweep / fork
// CoW preservation / slab telemetry without placeholder lifecycle overhead.
// The huge-direct path uses one MEM_REPLACE_PLACEHOLDER per allocation
// without WW — see `buddy_alloc_huge` for the sub-range-release rationale.
//
// Position in the stack:
//   malloc(small)    -> ThreadHeap -> slab_page -> segment -> buddy -> nt_pal
//   malloc(<= 4 MiB) -> buddy directly
//   malloc(>  4 MiB) -> huge-direct, one placeholder/commit per alloc.
//
// Caller invariants: sized free mandatory (rounded-up class size — wrong
// class trips the chunk_base / chunk_bytes / size_class cross-check in
// `buddy_free_sized` before the FreeFn runs); sizes above 4 MiB route to
// `buddy_alloc_huge` / `buddy_free_huge`; higher layers overlaying their
// own pagemap tag must restore `BuddyDirect` / `HugeDirect` before calling
// buddy's free; every commit goes through nt_pal.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_BUDDY_ARENA_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_BUDDY_ARENA_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives_bootstrap.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

// Geometry. Leaf shift equals pagemap chunk shift so every leaf maps to
// exactly one pagemap entry; 64 KiB matches NT's allocation granule so
// per-chunk MEM_COMMIT calls don't straddle VAD boundaries. The 4 GiB
// partition keeps every byte addressable by a 32-bit RVA — the segment
// layer above uses this for compact intra-partition pointers. The tree is
// 1-indexed (children at 2n / 2n+1) so the backing array needs `2^(d+1)`
// slots with index 0 unused; one byte per node holds the 5-bit NBALLOC
// status word.
inline constexpr uint8_t kBuddyMinShift = kPagemapShift;
inline constexpr size_t kBuddyMinChunkBytes = static_cast<size_t>(1)
                                              << kBuddyMinShift;
inline constexpr uint8_t kBuddyMaxShift = 22;
inline constexpr size_t kBuddyMaxChunkBytes = static_cast<size_t>(1)
                                              << kBuddyMaxShift;
inline constexpr uint8_t kBuddyClassCount =
    kBuddyMaxShift - kBuddyMinShift + 1;
// Sentinel `BuddyChunkDescriptor::size_class` for huge-direct chunks.
inline constexpr uint8_t kClassHuge = 0xFF;
inline constexpr size_t kBuddyArenaBytes = static_cast<size_t>(1) << 32;
inline constexpr uint8_t kBuddyTreeDepth = 32 - kBuddyMinShift;
inline constexpr size_t kBuddyTreeNodeCount =
    static_cast<size_t>(1) << (kBuddyTreeDepth + 1);
inline constexpr size_t kBuddyTreeBytes = kBuddyTreeNodeCount;

// Returns `kBuddyMaxShift + 1` (huge-path sentinel) for sizes above 4 MiB.
[[nodiscard]] LIBC_INLINE uint8_t buddy_class_shift_for(size_t size) {
  if (size <= kBuddyMinChunkBytes)
    return kBuddyMinShift;
  // ceil(log2(x)) = 64 - lzcnt(x - 1). Single defined-on-zero `lzcnt`.
  unsigned ceil_log2 =
      64u - static_cast<unsigned>(__builtin_clzll(size - 1ULL));
  if (ceil_log2 > kBuddyMaxShift)
    return kBuddyMaxShift + 1;
  return static_cast<uint8_t>(ceil_log2);
}

[[nodiscard]] LIBC_INLINE constexpr size_t
buddy_class_bytes(uint8_t shift) {
  return static_cast<size_t>(1) << shift;
}

// 0 = root, kBuddyTreeDepth = leaf.
[[nodiscard]] LIBC_INLINE constexpr uint8_t
buddy_class_tree_level(uint8_t shift) {
  return kBuddyTreeDepth - (shift - kBuddyMinShift);
}

// Per-slot freelist-link cookie for the slab layer (XORed against `next`
// pointers so a leaked link reveals only that one slot's cookie). The
// golden-ratio multiplier (Knuth) + 16-bit word rotate spreads bits so an
// attacker who leaks one link cannot extrapolate to neighbouring slots.
[[nodiscard]] LIBC_INLINE uint32_t derive_slot_cookie(uint32_t page_cookie,
                                                     uint32_t slot_idx) {
  uint32_t mixed = slot_idx * 0x9E3779B9u;
  return page_cookie ^ ((mixed >> 16) | (mixed << 16));
}

// Crystalline-W managed per-chunk metadata. One descriptor per live chunk,
// pool base + capacity sealed in PCB Zone 0. BuddyDirect and HugeDirect
// share the pool; the FreeFn dispatches on `consumer_tag`.
//
// Invariants: the pagemap entry covering `chunk_base` resolves to this
// descriptor's `slot_idx` while live (`pagemap_load_descriptor<Tag>` is the
// one-shot wait-free decode); `canary` authenticates over (Zone-0 secret,
// chunk_base, chunk_bytes) and the FreeFn rejects any mismatch before any
// NT call; storage returns to the pool only through the Crystalline FreeFn
// so a wait-free reader holding a stale tagged pointer cannot observe a
// freed slot until the era advances.
struct alignas(64) BuddyChunkDescriptor
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
  // Macro-emitted intrusive Crystalline-W fields so the descriptor stays
  // standard-layout (which inheritance from CrystallineNode alone would not
  // guarantee under -Werror=-Winvalid-offsetof). `chunk_base` lands at
  // offset 24 with a 4-byte natural gap at [20..23].
  LIBC_CRYSTALLINE_NODE_FIELDS(BuddyChunkDescriptor);

  // Value-init so file-scope instances clear -Werror=-Wglobal-constructors;
  // `cpp::Atomic`'s defaulted ctor leaves the value indeterminate.
  void *chunk_base{};
  size_t chunk_bytes{};
  uintptr_t canary{};
  uint8_t size_class{};     // Buddy shift, or `kClassHuge`.
  uint8_t generation{};     // Counter bumped per alloc; reserved for cross-check.
  uint16_t consumer_tag{};  // `VaChunkConsumer` selecting FreeFn path.
  uint32_t page_cookie{};   // Per-descriptor seed for `derive_slot_cookie`.
  uint32_t slot_idx{};      // Back-index into the descriptor pool.
};

static_assert(sizeof(BuddyChunkDescriptor) <= 64,
              "BuddyChunkDescriptor must fit in one cache line");

} // namespace alloc
} // namespace windows

namespace concurrent {
// Pool capacity is 2^17 so `slot_idx` fits in 17 bits; the `+1` shift keeps
// the encoded code clear of the BatchLinkCodec zero sentinel and well below
// `kCrystallineBatchLinkRnodeBit` (bit 31). Declared at this scope so every
// TU instantiating `CrystallineDomain<BuddyChunkDescriptor>` — including
// one that only calls `retire()` on `g_arena_domain` — sees the
// specialization without ODR drift.
template <>
struct BatchLinkCodec<::LIBC_NAMESPACE::windows::alloc::BuddyChunkDescriptor> {
  using Node = ::LIBC_NAMESPACE::windows::alloc::BuddyChunkDescriptor;
  LIBC_INLINE static uint32_t encode(CrystallineNode *n) noexcept {
    return 1u + static_cast<Node *>(n)->slot_idx;
  }
  LIBC_INLINE static CrystallineNode *decode(uint32_t code) noexcept {
    auto *base = static_cast<Node *>(
        ::LIBC_NAMESPACE::g_pcb.zone0.buddy_desc_pool_base());
    return &base[code - 1u];
  }
};
} // namespace concurrent

namespace windows {
namespace alloc {

// PagemapTraits — pool base + capacity come from sealed Zone 0 so an
// attacker with arbitrary write cannot redirect descriptor resolution by
// clobbering a BSS pointer. BuddyDirect and HugeDirect share the pool;
// the FreeFn dispatches on `desc->consumer_tag` to pick the decommit path.
template <>
struct PagemapTraits<VaChunkConsumer::BuddyDirect> {
  using Descriptor = BuddyChunkDescriptor;
  [[nodiscard]] LIBC_INLINE static size_t pool_capacity() {
    return ::LIBC_NAMESPACE::g_pcb.zone0.buddy_desc_pool_capacity();
  }
  [[nodiscard]] LIBC_INLINE static Descriptor *pool_base() {
    return static_cast<Descriptor *>(
        ::LIBC_NAMESPACE::g_pcb.zone0.buddy_desc_pool_base());
  }
};

template <>
struct PagemapTraits<VaChunkConsumer::HugeDirect> {
  using Descriptor = BuddyChunkDescriptor;
  [[nodiscard]] LIBC_INLINE static size_t pool_capacity() {
    return ::LIBC_NAMESPACE::g_pcb.zone0.buddy_desc_pool_capacity();
  }
  [[nodiscard]] LIBC_INLINE static Descriptor *pool_base() {
    return static_cast<Descriptor *>(
        ::LIBC_NAMESPACE::g_pcb.zone0.buddy_desc_pool_base());
  }
};

// Allocate a chunk. Rounded up to a class in [64 KiB, 4 MiB]; sizes above
// 4 MiB auto-route to `buddy_alloc_huge`. Hot path is lock-free: one NBALLOC
// CAS chain, one `commit_in_reservation_no_writewatch`, the pagemap RELEASE
// stores. Caller must pass the rounded-up size to `buddy_free_sized`.
[[nodiscard]] void *buddy_alloc(size_t size);

// Pagemap retire (RELEASE-stores of zero) precedes Crystalline retire — a
// wait-free reader past the pagemap retire decodes `(0, Empty)` and rejects
// the address before reaching the descriptor; physical decommit runs in the
// FreeFn after quiescence.
void buddy_free_sized(void *addr, size_t size);

// Direct-to-PAL path for chunks above `kBuddyMaxChunkBytes` — one
// placeholder + commit per allocation, no NBALLOC tree. Every 64 KiB
// sub-chunk is stamped `HugeDirect` so the SIGSEGV classifier and
// `is_libc_pointer` resolve any interior address back to the descriptor.
[[nodiscard]] void *buddy_alloc_huge(size_t size);

// `size` is rounded up to NT's 64 KiB granule internally; mismatch against
// the descriptor's recorded `chunk_bytes` / `chunk_base` / `kClassHuge`
// traps directly in this path before any NT call.
void buddy_free_huge(void *addr, size_t size);

// Tier-A bootstrap entry. Reserves partition / tree / descriptor-pool VA,
// initializes the Crystalline domain, emits substrate Receipts so the
// bootstrap pass stamps the reservations as libc-internal. Returns the
// number of Receipts written into `out` (capacity `cap`).
uint32_t buddy_arena_init_fn(::LIBC_NAMESPACE::internal::Receipt *out,
                              uint32_t cap);

// Post-fork-child cleanup. Walks the descriptor-pool occupancy bitmap and
// classifies each occupied slot via `pagemap_load_descriptor`. Live
// descriptors preserved verbatim; zombies (slot occupied but pagemap entry
// zeroed because the parent's Crystalline retire batch never reached the
// FreeFn) have CoW-shared pages dropped and tree paths repaired before the
// slot is reclaimed. O(occupied descriptors). Single-threaded.
void buddy_arena_fork_reinit();

// Diagnostics / test only — reads are unsynchronised.
struct BuddyStats {
  size_t total_arenas;
  size_t total_chunks_live;
  size_t total_committed_bytes;
  size_t descriptor_pool_used;
};

[[nodiscard]] BuddyStats buddy_stats_snapshot();

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
