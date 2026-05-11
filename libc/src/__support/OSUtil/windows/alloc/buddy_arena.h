//===-- Lock-free buddy chunk broker (Layer 2) ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Layer-2 chunk broker for the NT-POSIX allocator. Brokers raw NT placeholder
/// VA carved by the `nt_pal::` PAL into power-of-two chunks (64 KiB through
/// 4 MiB) consumed by the segment / slab_page layers and the huge-bypass path.
///
/// The lock-free split/coalesce protocol is NBALLOC (Marotta et al.,
/// arXiv:1804.03436, 2018) adapted for placeholder-VA semantics. Boundary-tag-
/// free instant-fit lookup over chunk descriptors follows the vmem lineage
/// (Bonwick & Adams, USENIX ATC 2001) — metadata is out-of-band in a sealed
/// descriptor pool plus the pagemap, never embedded in user-released memory.
///
/// The arena owns a power-of-two VA range (one 4 GiB partition per arena)
/// carved up by a 1-indexed flat binary tree of single-byte status words. NT
/// placeholder operations (`MEM_REPLACE_PLACEHOLDER`, `MEM_PRESERVE_PLACEHOLDER`,
/// `MEM_COALESCE_PLACEHOLDERS`) split/coalesce VA regions at page granularity
/// in lock-step with NBALLOC's logical split/coalesce on the tree. Every private
/// commit pairs `MEM_COMMIT | PAGE_READWRITE` with `MEM_WRITE_WATCH` (armed once
/// at partition reserve time) so the hardware dirty bitmap is available to the
/// quarantine sweep, fork CoW preservation, and slab telemetry. Chunk-granularity
/// bookkeeping is gated by Crystalline-W (Nikolaev & Ravindran, PLDI 2024) so a
/// wait-free reader holding a tagged pointer can never observe freed metadata.
///
/// Position in the allocation stack:
/// \code
///   malloc(small)   -> ThreadHeap -> slab_page -> segment -> buddy -> nt_pal
///   malloc(<= 4 MiB)-> buddy directly
///   malloc(>  4 MiB)-> huge-direct path bypasses the tree, one
///                      placeholder/commit per allocation
/// \endcode
///
/// Caller invariants:
///   * Sized free is mandatory. `buddy_free_sized(addr, size)` must receive
///     the same (rounded-up) size the chunk was allocated for. A wrong-class
///     free traps via canary mismatch in the Crystalline FreeFn.
///   * `size` rounds up to a power of 2 in `[64 KiB, 4 MiB]`. Anything outside
///     the range routes to `buddy_alloc_huge` / `buddy_free_huge`.
///   * Higher layers may overlay their own pagemap tag, but must restore the
///     `BuddyDirect` / `HugeDirect` tag before calling the buddy's free.
///   * The buddy never bypasses `nt_pal` for commits; every commit goes
///     through `commit_in_reservation_no_writewatch` (in-arena) or
///     `commit_replace` (huge-direct) so write-watch tracking stays armed.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_BUDDY_ARENA_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_BUDDY_ARENA_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_primitives_bootstrap.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

//===----------------------------------------------------------------------===//
// Geometry
//===----------------------------------------------------------------------===//

/// Tree leaf shift, matching the pagemap chunk shift so every leaf maps to
/// exactly one pagemap entry. 64 KiB is the snmalloc/mimalloc consensus for
/// slab-page granularity (Liétar et al., ISMM 2019) and pairs cleanly with
/// NT's 64 KiB allocation granularity.
inline constexpr uint8_t kBuddyMinShift = kPagemapShift;
inline constexpr size_t kBuddyMinChunkBytes = static_cast<size_t>(1)
                                              << kBuddyMinShift;

/// Top class within the buddy. Sizes above 4 MiB route to the huge-direct
/// path and bypass the NBALLOC tree.
inline constexpr uint8_t kBuddyMaxShift = 22;
inline constexpr size_t kBuddyMaxChunkBytes = static_cast<size_t>(1)
                                              << kBuddyMaxShift;

/// Number of in-arena power-of-two classes: 64 KiB, 128 KiB, ..., 4 MiB.
inline constexpr uint8_t kBuddyClassCount =
    kBuddyMaxShift - kBuddyMinShift + 1;

/// Sentinel `BuddyChunkDescriptor::size_class` value identifying a chunk
/// allocated through the huge-direct path. In-arena chunks always use a
/// shift in `[kBuddyMinShift, kBuddyMaxShift]`.
inline constexpr uint8_t kClassHuge = 0xFF;

/// Per-arena VA partition size. 4 GiB lets a 32-bit RVA address every byte,
/// which the segment layer above exploits for compact intra-partition pointers.
inline constexpr size_t kBuddyArenaBytes = static_cast<size_t>(1) << 32;

/// Tree depth `d = log2(partition / leaf) = log2(2^32 / 2^16) = 16`.
inline constexpr uint8_t kBuddyTreeDepth = 32 - kBuddyMinShift;

/// NBALLOC node count. The tree is 1-indexed (node `n`'s children sit at `2n`
/// and `2n+1`), so the backing array needs `2^(d+1)` slots; index 0 is unused
/// and the deepest valid node index is `2^(d+1) - 1`.
inline constexpr size_t kBuddyTreeNodeCount =
    static_cast<size_t>(1) << (kBuddyTreeDepth + 1);

/// One byte per node (the 5-bit NBALLOC status word fits in a `uint8_t`).
/// 131 072 bytes total, naturally aligned to NT's 64 KiB allocation granule.
inline constexpr size_t kBuddyTreeBytes = kBuddyTreeNodeCount;

//===----------------------------------------------------------------------===//
// Class arithmetic
//===----------------------------------------------------------------------===//

/// Round \p size up to the next buddy class shift. Returns `kBuddyMinShift`
/// for any size at or below 64 KiB; returns `kBuddyMaxShift + 1` (huge-path
/// sentinel) for sizes above 4 MiB.
[[nodiscard]] LIBC_INLINE uint8_t buddy_class_shift_for(size_t size) {
  if (size <= kBuddyMinChunkBytes)
    return kBuddyMinShift;
  // ceil(log2(x)) for x > 1 is `64 - lzcnt(x - 1)`. The pinned ISA baseline
  // makes this a single `lzcnt` instruction with defined-on-zero semantics.
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

/// Map a class shift to its NBALLOC tree level (0 = root, `kBuddyTreeDepth` =
/// leaf). The 64 KiB leaf class sits at the deepest level; the 4 MiB top
/// class sits 6 levels above leaves.
[[nodiscard]] LIBC_INLINE constexpr uint8_t
buddy_class_tree_level(uint8_t shift) {
  return kBuddyTreeDepth - (shift - kBuddyMinShift);
}

//===----------------------------------------------------------------------===//
// Per-slot cookie derivation
//===----------------------------------------------------------------------===//

/// Derive a per-slot freelist-link cookie from the slab page's seed and a
/// slot index.
///
/// The slab layer XORs `next` pointers against this cookie so a leaked link
/// reveals only the cookie of that one slot. The golden-ratio multiplier
/// (Knuth, 32-bit constant) spreads index bits uniformly across the output,
/// and the 16-bit byteswap mixes high and low halves so neighbouring slots
/// produce maximally-distinct cookies — an attacker who leaks one link
/// cannot extrapolate to other slots in the same page.
[[nodiscard]] LIBC_INLINE uint32_t derive_slot_cookie(uint32_t page_cookie,
                                                     uint32_t slot_idx) {
  uint32_t mixed = slot_idx * 0x9E3779B9u;
  return page_cookie ^ ((mixed >> 16) | (mixed << 16));
}

//===----------------------------------------------------------------------===//
// BuddyChunkDescriptor
//===----------------------------------------------------------------------===//

/// Crystalline-W managed per-chunk metadata. One descriptor per live buddy
/// allocation, allocated from a process-lifetime descriptor pool sealed in
/// PCB Zone 0.
///
/// Invariants:
///   * The pagemap entry covering \c chunk_base resolves to this descriptor's
///     `slot_idx` while the chunk is live (`pagemap_load_descriptor<Tag>` is
///     the one-shot wait-free decode).
///   * \c canary is the keyed authenticator over (Zone-0 secret, \c chunk_base,
///     \c chunk_bytes). The FreeFn rejects any mismatch — a wrong-class free or
///     a forged descriptor traps before any NT call.
///   * \c size_class holds the buddy shift in `[kBuddyMinShift, kBuddyMaxShift]`
///     for in-arena chunks and `kClassHuge` for huge-direct chunks; the FreeFn
///     dispatches on \c consumer_tag to pick decommit vs decommit-and-release.
///   * Storage is returned to the descriptor pool only through the Crystalline
///     FreeFn — a wait-free reader holding a stale tagged pointer cannot
///     observe a freed slot until the Crystalline era advances.
///
/// Layout is one cache line. Both `BuddyDirect` and `HugeDirect` consumers
/// share the pool; the FreeFn dispatches on \c consumer_tag.
struct alignas(64) BuddyChunkDescriptor
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
  // Intrusive Crystalline-W runtime fields emitted in-line via the macro so
  // BuddyChunkDescriptor stays standard-layout (which inheritance from
  // CrystallineNode alone would not guarantee under -Werror=-Winvalid-offsetof).
  // `chunk_base` lands at offset 24 with a 4-byte natural gap at [20..23].
  LIBC_CRYSTALLINE_NODE_FIELDS(BuddyChunkDescriptor);

  // Members are value-initialized so the implicit default constructor stays
  // constexpr-eligible. `cpp::Atomic`'s defaulted constructor leaves its value
  // indeterminate; an indeterminate-init field would block any constant-
  // initialized global instance under -Werror=-Wglobal-constructors.
  void *chunk_base{};   ///< Base VA of the chunk this descriptor owns.
  size_t chunk_bytes{}; ///< Rounded-up class size, in bytes.
  uintptr_t canary{};   ///< Keyed authenticator; rejected on wrong-class free.
  uint8_t size_class{}; ///< Buddy shift, or `kClassHuge` for huge-direct.
  uint8_t generation{}; ///< ABA-defense counter bumped per allocation.
  uint16_t consumer_tag{}; ///< `VaChunkConsumer` value selecting the FreeFn path.
  uint32_t page_cookie{};  ///< Per-descriptor seed for `derive_slot_cookie`.
  uint32_t slot_idx{};     ///< Back-index into the descriptor pool.
};

static_assert(sizeof(BuddyChunkDescriptor) <= 64,
              "BuddyChunkDescriptor must fit in one cache line");

} // namespace alloc
} // namespace windows

namespace concurrent {
/// Batch-link codec specialization for BuddyChunkDescriptor.
///
/// Encodes the descriptor's `slot_idx` directly. Pool capacity is
/// `1 << 17 = 131072`, so the index fits in 17 bits; the `+1` shift keeps the
/// encoded code clear of the zero sentinel and well below bit 31 (the
/// Crystalline batch-link reserved sign bit).
///
/// Declared here so any TU instantiating `CrystallineDomain<BuddyChunkDescriptor>`
/// — or just calling `retire()` on `g_arena_domain` — sees the specialization
/// without ODR drift.
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

//===----------------------------------------------------------------------===//
// PagemapTraits specializations
//===----------------------------------------------------------------------===//

/// Wires the typed pagemap load to the buddy's descriptor pool for
/// `BuddyDirect`-tagged entries. The pool base and capacity come from sealed
/// PCB Zone 0 — an attacker with arbitrary write cannot redirect descriptor
/// resolution by clobbering a BSS pointer.
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

/// Wires the typed pagemap load for `HugeDirect`-tagged entries. Shares the
/// same descriptor pool as `BuddyDirect`; callers differentiate by inspecting
/// `desc->size_class == kClassHuge`.
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

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

/// Allocate a chunk of \p size bytes from the buddy arena.
///
/// \p size is rounded up to the next power-of-two class in
/// `[64 KiB, 4 MiB]`; sizes above 4 MiB auto-route to `buddy_alloc_huge` and
/// are stamped with the `HugeDirect` pagemap tag. The returned VA is
/// committed `PAGE_READWRITE` against the arena's pre-armed write-watch
/// reservation and registered in the pagemap under the `BuddyDirect` tag.
///
/// Hot path is lock-free: one NBALLOC TRYALLOC CAS chain at the target level,
/// one `commit_in_reservation_no_writewatch` syscall, and the pagemap RELEASE
/// stores. Every CAS retry is caused by another thread's CAS success — there
/// are no spin loops.
///
/// \returns the chunk base VA, or nullptr on OOM (commit-charge exhaustion or
///          partition saturation). The caller must pass the rounded-up size,
///          not the original request size, to `buddy_free_sized`.
[[nodiscard]] void *buddy_alloc(size_t size);

/// Release a chunk previously returned by `buddy_alloc`.
///
/// \param addr the chunk base returned by `buddy_alloc`.
/// \param size the same rounded-up class size that was allocated. Passing the
///             user's original (unrounded) request is a wrong-class free and
///             traps via canary mismatch in the Crystalline FreeFn.
///
/// Releases the chunk's pagemap entries, frees the NBALLOC tree node, and
/// retires the descriptor through `g_arena_domain`; physical-page decommit
/// runs in the Crystalline FreeFn after quiescence.
void buddy_free_sized(void *addr, size_t size);

/// Allocate a chunk larger than `kBuddyMaxChunkBytes` directly from the PAL.
///
/// Each huge allocation gets its own placeholder VA + commit (no NBALLOC tree
/// involvement). The pagemap entry for every 64 KiB sub-chunk is stamped with
/// `HugeDirect` so the SIGSEGV classifier and `is_libc_pointer` resolve any
/// interior address back to the descriptor.
///
/// \returns the chunk base VA, or nullptr on OOM.
[[nodiscard]] void *buddy_alloc_huge(size_t size);

/// Release a huge allocation.
///
/// \param addr the base VA returned by `buddy_alloc_huge`.
/// \param size the requested size (rounded up to page granularity inside).
///             A mismatch traps via canary check in the Crystalline FreeFn.
void buddy_free_huge(void *addr, size_t size);

//===----------------------------------------------------------------------===//
// Tier-A bootstrap
//===----------------------------------------------------------------------===//

/// Tier-A bootstrap entry. Reserves the partition VA, the NBALLOC tree backing
/// VA, and the descriptor pool VA; initializes the Crystalline domain; and
/// emits substrate Receipts so the bootstrap pass stamps the reservations as
/// libc-internal.
///
/// \param out  output Receipt buffer.
/// \param cap  capacity of \p out, in Receipt entries.
/// \returns the number of Receipts written.
uint32_t buddy_arena_init_fn(::LIBC_NAMESPACE::internal::Receipt *out,
                              uint32_t cap);

/// Post-fork-child cleanup. Walks the descriptor pool's occupancy bitmap and
/// classifies each occupied slot via `pagemap_load_descriptor`. Live
/// descriptors are preserved verbatim; zombie descriptors (slot occupied but
/// the pagemap entry zeroed because the parent's Crystalline retire batch
/// never reached its FreeFn) have their CoW-shared physical pages dropped and
/// their tree path repaired before the slot is reclaimed.
///
/// Cost is O(occupied descriptors), bounded by the retire-frequency cap of
/// `g_arena_domain` rather than by live-chunk count. Runs single-threaded.
void buddy_arena_fork_reinit();

//===----------------------------------------------------------------------===//
// Diagnostics
//===----------------------------------------------------------------------===//

/// Snapshot of buddy-wide statistics for diagnostics and test use only. Reads
/// are not synchronized — counter values may be stale relative to one another.
struct BuddyStats {
  size_t total_arenas;          ///< Number of live arena instances.
  size_t total_chunks_live;     ///< Sum of in-use leaves across arenas.
  size_t total_committed_bytes; ///< Sum of committed chunk VA across arenas.
  size_t descriptor_pool_used;  ///< Descriptor slots currently in use.
};

[[nodiscard]] BuddyStats buddy_stats_snapshot();

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
