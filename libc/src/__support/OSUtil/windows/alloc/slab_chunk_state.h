//===- slab_chunk_state.h - Sub-slab commit-state mask --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-slab-page sub-page commit-state bitmask used by sub-slab decommit.
//
// A 64 KiB slab page is divided into kSubPagesPerSlab = 16 OS-page sub-pages
// of kSubPageBytes = 4 KiB each. The mask is a 16-bit atomic; bit i set
// means sub-page i is currently backed by physical RAM (PAGE_READWRITE,
// observably writable), bit i clear means it has been returned to the OS
// via MEM_DECOMMIT | MEM_PRESERVE_PLACEHOLDER while the partition VA stays
// reserved.
//
// Decommitting at 4 KiB granularity rather than the documented 64 KiB
// allocation-granularity hint is supported by the kernel's
// MEM_DECOMMIT | MEM_PRESERVE_PLACEHOLDER path on Windows 11 build 26200,
// validated empirically. Matches the sub-slab reclamation strategy in
// mimalloc (Leijen, Zorn, de Moura, APLAS 2019) on Linux.
//
// Sibling header to other chunk-state types rather than folded in: the
// coarse pool-chunk lifecycle (Live -> Draining -> Dead) packs tri-state +
// 24-bit count + 32-bit gen into one 64-bit atomic with a CAS-loop
// multi-writer protocol -- structurally unsuitable for the per-OS-page,
// single-writer bitmap shape here.
//
// Initial value at slab-page minting is kSubPageMaskAllCommitted (0xFFFF):
// the underlying buddy chunk was committed wholesale at allocation, so the
// slab page inherits a fully-committed sub-page set. Decommit is strictly
// a reclamation primitive; never preempts the initial commit.
//
// Ordering invariants (the entire correctness story):
//
//   1. Bit-clear (RELEASE fetch_and) happens-before the MEM_DECOMMIT
//      syscall. A reader observing a cleared bit must not assume the
//      sub-page is mapped. Sequencing clear-before-syscall means a
//      concurrent allocator that observes the cleared bit recommits before
//      touching even if the syscall has not yet run; worst case is one
//      idempotent extra commit.
//   2. Bit-set (RELEASE fetch_or) happens-after the commit_in_reservation
//      syscall returns. A reader observing a set bit can trust the page
//      exists.
//   3. Owner-thread serialisation. The slab page's owner thread is the
//      sole writer of the mask. Cross-thread frees route through a
//      separate thread_free_bitmap and never touch commit state.
//      Decommit (deadline drain) and recommit (alloc slow path) are both
//      owner work, so two bit-and-syscall pairs on the same sub-page
//      cannot race.
//
// Edge cases callers can rely on:
//
//   * Cross-thread free into a decommitted sub-page is safe -- the freer
//     writes the descriptor's separate free bitmap (committed metadata
//     page), not the slot payload.
//   * Canary verification on the free path derives per-slot cookies from
//     descriptor metadata, never from slot payload; safe across decommit.
//   * Zero-on-free is owner-side. Sub-pages with any live slot stay
//     committed (compute_idle_subpages excludes them), so decommit only
//     retires sub-pages whose slots are already free and zeroed.
//   * NT semantics: MEM_DECOMMIT | MEM_PRESERVE_PLACEHOLDER operates at
//     4 KiB inside a placeholder-armed reservation, auto-clears the
//     write-watch bitmap for the affected range, and is idempotent on
//     already-decommitted sub-pages.
//   * Fork re-runs the idle-set / decommit pass against the inherited
//     mask state in case a deadline was pending at fork time.
//
// What this header is NOT:
//
//   * Not Crystalline-W managed (Nikolaev, Ravindran, PLDI 2024) -- the
//     mask is a plain field embedded in the page descriptor; Crystalline-W
//     reclaims the descriptor, not the mask.
//   * Not a substitute for whole-chunk decommit -- chunk-level release
//     routes through the layered-reclamation rule; sub-slab decommit
//     operates within an already-Live chunk's lifetime.
//   * Not for medium-class single-slot pages above 64 KiB -- those retire
//     wholesale when their single slot is freed. The mask is valid only
//     for multi-slot 64 KiB small-class pages; the descriptor gates this
//     by size class (caller responsibility).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SLAB_CHUNK_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SLAB_CHUNK_STATE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

//===----------------------------------------------------------------------===//
// Geometry.
//===----------------------------------------------------------------------===//

// OS page size on x86-64 / ARM64 Windows. Sub-page decommit operates here.
inline constexpr size_t kSubPageBytes = 4 * 1024;

// Slab-page granularity. Matches the buddy arena's leaf-chunk size and the
// medium-grain slab size that mimalloc (Leijen, Zorn, de Moura, APLAS 2019)
// and snmalloc (Liétar et al., ISMM 2019) converge on.
inline constexpr size_t kSlabPageBytes = 64 * 1024;

// 16 fits in a uint16_t bitmap with one bit per sub-page.
inline constexpr uint8_t kSubPagesPerSlab =
    static_cast<uint8_t>(kSlabPageBytes / kSubPageBytes);

static_assert(kSubPagesPerSlab == 16,
              "SubPageCommitMask is uint16_t; sub-page count must be 16");
static_assert(static_cast<size_t>(kSubPagesPerSlab) * kSubPageBytes ==
                  kSlabPageBytes,
              "kSlabPageBytes must equal kSubPagesPerSlab * kSubPageBytes");

// Initial value at slab-page minting (every sub-page committed).
inline constexpr uint16_t kSubPageMaskAllCommitted = 0xFFFFu;

// Fully-reclaimed value (no sub-page backed by RAM).
inline constexpr uint16_t kSubPageMaskAllDecommitted = 0u;

//===----------------------------------------------------------------------===//
// SubPageCommitMask -- the type embedded in PageDescriptor.
//
//   bit i == 1 -> sub-page i currently committed and writable.
//   bit i == 0 -> sub-page i decommitted; partition VA still reserved.
//
// File banner carries the full ordering story. Free functions below operate
// on a reference rather than wrapping in a class -- the descriptor is the
// natural owner.
//===----------------------------------------------------------------------===//

using SubPageCommitMask = cpp::Atomic<uint16_t>;

//===----------------------------------------------------------------------===//
// Mask accessor primitives.
//
// Writers must be the slab page's owner thread (banner invariant 3).
// Readers may run from any thread; the cross-thread free's canary
// verification does not touch this mask but accessors are exposed for
// tools and telemetry.
//
// Non-const SubPageCommitMask& parameters reflect cpp::Atomic::load being
// non-const in this codebase, not mutation.
//===----------------------------------------------------------------------===//

// RELAXED is safe: nothing observes the mask until the slab page is
// published into the heap's class list, and that publication carries its
// own RELEASE.
LIBC_INLINE void
init_subpage_mask_all_committed(SubPageCommitMask &m) {
  m.store(kSubPageMaskAllCommitted, cpp::MemoryOrder::RELAXED);
}

// ACQUIRE pairs with the RELEASE store in mark_subpage_committed so the
// caller's observation of "committed" happens-after the underlying commit
// syscall returned.
[[nodiscard]] LIBC_INLINE bool
is_subpage_committed(SubPageCommitMask &m, uint8_t idx) {
  LIBC_ASSERT(idx < kSubPagesPerSlab);
  uint16_t bits = m.load(cpp::MemoryOrder::ACQUIRE);
  return ((bits >> idx) & 1u) != 0u;
}

// Caller MUST issue the commit syscall first (banner invariant 2).
// Returns true on a real 0->1 transition, false on idempotent re-arm.
// Owner-thread serialisation means fetch_or never contends.
LIBC_INLINE bool
mark_subpage_committed(SubPageCommitMask &m, uint8_t idx) {
  LIBC_ASSERT(idx < kSubPagesPerSlab);
  uint16_t bit = static_cast<uint16_t>(static_cast<uint16_t>(1u) << idx);
  uint16_t prev = m.fetch_or(bit, cpp::MemoryOrder::RELEASE);
  return (prev & bit) == 0u;
}

// Caller MUST issue the decommit syscall after this returns true (banner
// invariant 1). RELEASE sequences the bit-clear before the syscall so a
// concurrent allocator reading the cleared bit recommits defensively even
// if the kernel has not yet observably reclaimed the page.
LIBC_INLINE bool
mark_subpage_decommitted(SubPageCommitMask &m, uint8_t idx) {
  LIBC_ASSERT(idx < kSubPagesPerSlab);
  uint16_t bit = static_cast<uint16_t>(static_cast<uint16_t>(1u) << idx);
  uint16_t mask = static_cast<uint16_t>(~bit);
  uint16_t prev = m.fetch_and(mask, cpp::MemoryOrder::RELEASE);
  return (prev & bit) != 0u;
}

// ACQUIRE pairs with any RELEASE writer for happens-before on the published
// mask. Feeds compute_idle_subpages on the deadline-drain path.
[[nodiscard]] LIBC_INLINE uint16_t
snapshot_subpage_mask(SubPageCommitMask &m) {
  return m.load(cpp::MemoryOrder::ACQUIRE);
}

//===----------------------------------------------------------------------===//
// Slot <-> sub-page arithmetic. Co-located with the mask so every site that
// converts uses the same convention. Offsets are bytes from the slab page's
// payload base.
//===----------------------------------------------------------------------===//

// Inclusive sub-page index range [first, last] spanned by one slot. A slot
// wider than kSubPageBytes spans multiple sub-pages (e.g. 8 KiB -> 2 pages).
struct SubPageRange {
  uint8_t first;
  uint8_t last;
};

[[nodiscard]] LIBC_INLINE constexpr uint8_t
subpage_index_for_offset(size_t offset_bytes) {
  return static_cast<uint8_t>(offset_bytes / kSubPageBytes);
}

// Precondition: slot_idx * slot_size + slot_size <= kSlabPageBytes (holds
// for any well-formed small-class slab geometry).
[[nodiscard]] LIBC_INLINE constexpr SubPageRange
subpage_range_for_slot(size_t slot_size, uint32_t slot_idx) {
  size_t lo = static_cast<size_t>(slot_idx) * slot_size;
  size_t hi = lo + slot_size - 1;
  return SubPageRange{subpage_index_for_offset(lo),
                      subpage_index_for_offset(hi)};
}

// OR-fold helper for live-slot ranges into a live_subpage_mask on the
// deadline-drain path.
[[nodiscard]] LIBC_INLINE constexpr uint16_t
subpage_range_to_mask(SubPageRange r) {
  uint32_t count = static_cast<uint32_t>(r.last - r.first + 1);
  // count maxes at 16 by the slab-page geometry (kSubPagesPerSlab). The
  // >= 16 short-circuit returns the saturated mask directly so the result
  // is correct even if a caller widens the geometry past uint32_t shift
  // limits in the future; for count == 16 it also avoids the redundant
  // `(1u << 16) - 1` arithmetic.
  uint32_t span = (count >= 16u) ? 0xFFFFu
                                 : ((1u << count) - 1u);
  return static_cast<uint16_t>(span << r.first);
}

// Returns the argument that the commit / decommit syscalls expect for the
// named sub-page.
[[nodiscard]] LIBC_INLINE void *
subpage_address_for(void *slab_payload_base, uint8_t idx) {
  LIBC_ASSERT(idx < kSubPagesPerSlab);
  return static_cast<void *>(static_cast<unsigned char *>(slab_payload_base) +
                             static_cast<size_t>(idx) * kSubPageBytes);
}

//===----------------------------------------------------------------------===//
// Idle-set computation.
//===----------------------------------------------------------------------===//

// Drives the deadline-driven decommit decision: each set bit in the result
// names a sub-page the drain step will clear from the commit mask and then
// release via the decommit syscall.
//
// committed_mask: snapshot of the commit mask (typically from
//                 snapshot_subpage_mask).
// live_subpage_mask: bit i set iff sub-page i contains at least one live
//                    slot. Computed by walking the descriptor's free /
//                    local-free / thread-free bitmaps, inverting to
//                    liveness, and OR-folding
//                    subpage_range_to_mask(subpage_range_for_slot(...))
//                    for each live slot.
[[nodiscard]] LIBC_INLINE constexpr uint16_t
compute_idle_subpages(uint16_t committed_mask, uint16_t live_subpage_mask) {
  return static_cast<uint16_t>(committed_mask &
                                static_cast<uint16_t>(~live_subpage_mask));
}

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
