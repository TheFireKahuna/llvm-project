//===- slab_chunk_state.h - Sub-slab commit-state mask --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-slab-page sub-page commit-state bitmask used by sub-slab decommit.
///
/// A 64 KiB slab page is divided into \c kSubPagesPerSlab = 16 OS-page
/// sub-pages of \c kSubPageBytes = 4 KiB each. The mask is a 16-bit
/// atomic where bit <tt>i</tt> records whether sub-page <tt>i</tt> is
/// currently backed by physical RAM (\c PAGE_READWRITE, observably
/// writable) or has been returned to the OS via
/// <tt>MEM_DECOMMIT | MEM_PRESERVE_PLACEHOLDER</tt> while keeping the
/// partition VA reserved.
///
/// Decommitting at sub-page granularity (4 KiB) rather than the
/// documented 64 KiB allocation-granularity hint is supported by the
/// kernel's <tt>MEM_DECOMMIT | MEM_PRESERVE_PLACEHOLDER</tt> path on
/// Windows 11 build 26200, validated empirically; this matches the
/// sub-slab reclamation strategy used by mimalloc (Leijen, Zorn, de
/// Moura, APLAS 2019) on Linux.
///
/// Why this is a sibling header to other chunk-state types rather than
/// folded in:
///
///   * A separate chunk-state descriptor exists for the coarse pool-chunk
///     lifecycle (Live -> Draining -> Dead). That descriptor packs a
///     tri-state byte + 24-bit count + 32-bit generation into one 64-bit
///     atomic. Its CAS-loop multi-writer protocol and per-chunk
///     granularity are structurally unsuitable for the per-OS-page,
///     single-writer bitmap shape needed here.
///
/// Initial value at slab-page minting is \c kSubPageMaskAllCommitted
/// (0xFFFF). The underlying buddy chunk was committed wholesale at
/// allocation, so the slab page inherits a fully-committed sub-page set.
/// Decommit is strictly a reclamation primitive; it never preempts the
/// initial commit.
///
/// Ordering invariants (the entire correctness story):
///
///   1. Bit-clear (RELEASE \c fetch_and) happens-before the
///      \c MEM_DECOMMIT syscall. A reader observing a cleared bit
///      concludes "do not assume the sub-page is mapped". Sequencing the
///      clear before the syscall means a concurrent allocator that
///      observes the bit clear will recommit before touching, even if
///      the syscall has not yet run; worst case is one idempotent extra
///      commit.
///   2. Bit-set (RELEASE \c fetch_or) happens-after the
///      <tt>commit_in_reservation</tt> syscall returns. A reader
///      observing a set bit can trust the page exists.
///   3. Owner-thread serialisation. The slab page's owner thread is the
///      sole writer of the mask. Cross-thread frees route through a
///      separate \c thread_free_bitmap and never touch commit state.
///      Decommit (deadline drain) and recommit (alloc slow path) are
///      both owner work, so the bit-and-syscall pair on any one sub-page
///      cannot race against another bit-and-syscall pair on the same
///      sub-page.
///
/// Edge cases callers can rely on:
///
///   * Cross-thread free into a decommitted sub-page is safe — the freer
///     writes the descriptor's separate free bitmap (committed metadata
///     page), not the slot payload.
///   * Canary verification on the free path derives per-slot cookies
///     from descriptor metadata, never from slot payload; safe across
///     decommit.
///   * Zero-on-free is owner-side. Sub-pages with any live slot stay
///     committed (\c compute_idle_subpages excludes them), so decommit
///     only retires sub-pages whose slots are already free and zeroed.
///   * NT semantics: <tt>MEM_DECOMMIT | MEM_PRESERVE_PLACEHOLDER</tt>
///     operates at 4 KiB granularity inside a placeholder-armed
///     reservation, auto-clears the write-watch bitmap for the affected
///     range, and is idempotent on already-decommitted sub-pages.
///   * Fork re-runs the idle-set / decommit pass against the inherited
///     mask state in case the parent had a deadline pending at fork
///     time.
///
/// What this header is NOT:
///
///   * Not Crystalline-W managed (Nikolaev, Ravindran, PLDI 2024). The
///     mask is a plain field embedded in the page descriptor; the
///     descriptor itself is what Crystalline-W reclaims.
///   * Not a substitute for whole-chunk decommit. Chunk-level release
///     still routes through the layered-reclamation rule; sub-slab
///     decommit operates within an already-Live chunk's lifetime.
///   * Not for medium-class single-slot pages above 64 KiB; those
///     retire wholesale when their single slot is freed. The mask is
///     valid only for multi-slot 64 KiB small-class pages, gated at the
///     descriptor level by size class (caller responsibility).
///
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

/// OS page size on x86-64 / ARM64 Windows. Sub-page decommit operates at
/// this granularity.
inline constexpr size_t kSubPageBytes = 4 * 1024;

/// Slab-page granularity. Matches the buddy arena's leaf-chunk size and
/// the size that mimalloc (Leijen, Zorn, de Moura, APLAS 2019) and
/// snmalloc (Liétar et al., ISMM 2019) converge on for medium-grain
/// slab pages.
inline constexpr size_t kSlabPageBytes = 64 * 1024;

/// Number of OS sub-pages within one slab page. 16 fits in a uint16_t
/// bitmap with one bit per sub-page.
inline constexpr uint8_t kSubPagesPerSlab =
    static_cast<uint8_t>(kSlabPageBytes / kSubPageBytes);

static_assert(kSubPagesPerSlab == 16,
              "SubPageCommitMask is uint16_t; sub-page count must be 16");
static_assert(static_cast<size_t>(kSubPagesPerSlab) * kSubPageBytes ==
                  kSlabPageBytes,
              "kSlabPageBytes must equal kSubPagesPerSlab * kSubPageBytes");

/// Mask value with every sub-page bit set (freshly-minted slab page).
inline constexpr uint16_t kSubPageMaskAllCommitted = 0xFFFFu;

/// Mask value with every sub-page bit clear (fully reclaimed).
inline constexpr uint16_t kSubPageMaskAllDecommitted = 0u;

//===----------------------------------------------------------------------===//
// SubPageCommitMask — the type embedded in PageDescriptor.
//===----------------------------------------------------------------------===//

/// Atomic 16-bit bitmap of sub-page commit state, embedded in the page
/// descriptor.
///
/// Bit semantics (file-level brief carries the full ordering story):
///
///   bit i == 1 -> sub-page i is currently committed and writable.
///   bit i == 0 -> sub-page i has been decommitted; the partition VA
///                 stays reserved but no physical RAM backs the range.
///
/// Free functions below operate on a reference to this atomic rather
/// than wrapping it in a class — the descriptor is the natural owner.
using SubPageCommitMask = cpp::Atomic<uint16_t>;

//===----------------------------------------------------------------------===//
// Mask accessor primitives.
//
// All writers must be the slab page's owner thread (file-level
// invariant 3). Readers may run from any thread; cross-thread free's
// canary verification does not touch this mask but the accessors are
// exposed for tools and telemetry.
//===----------------------------------------------------------------------===//

/// Initialise \p m for a freshly-minted slab page (all sub-pages
/// committed).
///
/// RELAXED is safe here: nothing observes the mask until the slab page
/// is published into the heap's class list, and that publication carries
/// its own RELEASE.
LIBC_INLINE void
init_subpage_mask_all_committed(SubPageCommitMask &m) {
  m.store(kSubPageMaskAllCommitted, cpp::MemoryOrder::RELAXED);
}

/// Test whether sub-page \p idx is currently committed.
///
/// ACQUIRE pairs with the RELEASE store in \c mark_subpage_committed so
/// the caller's observation of "committed" happens-after the underlying
/// commit syscall returned.
///
/// The reference is non-const because \c cpp::Atomic::load is non-const
/// in this codebase.
[[nodiscard]] LIBC_INLINE bool
is_subpage_committed(SubPageCommitMask &m, uint8_t idx) {
  LIBC_ASSERT(idx < kSubPagesPerSlab);
  uint16_t bits = m.load(cpp::MemoryOrder::ACQUIRE);
  return ((bits >> idx) & 1u) != 0u;
}

/// Mark sub-page \p idx as committed; the caller MUST issue the commit
/// syscall first.
///
/// RELEASE publishes the bit set after the syscall has made the page
/// observable. The owner-thread serialisation invariant means \c fetch_or
/// never spins on contention.
///
/// \returns \c true if the bit was previously clear (a real
///          0-to-1 transition), \c false if it was already set
///          (idempotent re-arm).
LIBC_INLINE bool
mark_subpage_committed(SubPageCommitMask &m, uint8_t idx) {
  LIBC_ASSERT(idx < kSubPagesPerSlab);
  uint16_t bit = static_cast<uint16_t>(static_cast<uint16_t>(1u) << idx);
  uint16_t prev = m.fetch_or(bit, cpp::MemoryOrder::RELEASE);
  return (prev & bit) == 0u;
}

/// Mark sub-page \p idx as decommitted; the caller MUST issue the
/// decommit syscall after this returns true.
///
/// RELEASE sequences the bit clear before the syscall so concurrent
/// allocators that read the cleared bit will recommit defensively even
/// if the kernel has not yet observably reclaimed the page.
///
/// \returns \c true if the bit was previously set (a real 1-to-0
///          transition; the caller should issue the decommit syscall),
///          \c false if it was already clear (no syscall needed).
LIBC_INLINE bool
mark_subpage_decommitted(SubPageCommitMask &m, uint8_t idx) {
  LIBC_ASSERT(idx < kSubPagesPerSlab);
  uint16_t bit = static_cast<uint16_t>(static_cast<uint16_t>(1u) << idx);
  uint16_t mask = static_cast<uint16_t>(~bit);
  uint16_t prev = m.fetch_and(mask, cpp::MemoryOrder::RELEASE);
  return (prev & bit) != 0u;
}

/// Snapshot the entire mask.
///
/// ACQUIRE pairs with any RELEASE writer for happens-before on the
/// published mask state. The deadline-drain path uses this to feed
/// \c compute_idle_subpages.
///
/// The reference is non-const because \c cpp::Atomic::load is non-const
/// in this codebase.
[[nodiscard]] LIBC_INLINE uint16_t
snapshot_subpage_mask(SubPageCommitMask &m) {
  return m.load(cpp::MemoryOrder::ACQUIRE);
}

//===----------------------------------------------------------------------===//
// Slot <-> sub-page arithmetic.
//
// Co-located with the mask so every site that converts between slot
// indices and sub-page indices uses the same convention. All offsets are
// in bytes from the slab page's payload base.
//===----------------------------------------------------------------------===//

/// Inclusive sub-page index range <tt>[first, last]</tt> spanned by one
/// slot. A slot wider than \c kSubPageBytes spans multiple sub-pages
/// (e.g. an 8 KiB slot covers two sub-pages).
struct SubPageRange {
  uint8_t first; ///< Index of the first sub-page touched by the slot.
  uint8_t last;  ///< Index of the last sub-page touched by the slot.
};

/// Sub-page index for a byte \p offset_bytes from the payload base.
[[nodiscard]] LIBC_INLINE constexpr uint8_t
subpage_index_for_offset(size_t offset_bytes) {
  return static_cast<uint8_t>(offset_bytes / kSubPageBytes);
}

/// Compute the inclusive sub-page range that slot \p slot_idx of size
/// \p slot_size spans within a slab page's payload.
///
/// \pre The caller must guarantee
///      <tt>slot_idx * slot_size + slot_size <= kSlabPageBytes</tt>;
///      this holds for any well-formed small-class slab geometry.
[[nodiscard]] LIBC_INLINE constexpr SubPageRange
subpage_range_for_slot(size_t slot_size, uint32_t slot_idx) {
  size_t lo = static_cast<size_t>(slot_idx) * slot_size;
  size_t hi = lo + slot_size - 1;
  return SubPageRange{subpage_index_for_offset(lo),
                      subpage_index_for_offset(hi)};
}

/// Convert an inclusive range into a 16-bit mask with bits
/// <tt>[first, last]</tt> set.
///
/// Useful for OR-folding live-slot ranges into a \c live_subpage_mask on
/// the deadline-drain path.
[[nodiscard]] LIBC_INLINE constexpr uint16_t
subpage_range_to_mask(SubPageRange r) {
  // Build the contiguous run from constants so the compiler folds at the
  // call site. The width >= 16 branch sidesteps the undefined-shift
  // corner when a full-width range arrives.
  uint32_t count = static_cast<uint32_t>(r.last - r.first + 1);
  uint32_t span = (count >= 16u) ? 0xFFFFu
                                 : ((1u << count) - 1u);
  return static_cast<uint16_t>(span << r.first);
}

/// Address of sub-page \p idx within a slab page whose payload starts at
/// \p slab_payload_base. The returned pointer is the argument that the
/// commit and decommit syscalls expect for that sub-page.
[[nodiscard]] LIBC_INLINE void *
subpage_address_for(void *slab_payload_base, uint8_t idx) {
  LIBC_ASSERT(idx < kSubPagesPerSlab);
  return static_cast<void *>(static_cast<unsigned char *>(slab_payload_base) +
                             static_cast<size_t>(idx) * kSubPageBytes);
}

//===----------------------------------------------------------------------===//
// Idle-set computation.
//===----------------------------------------------------------------------===//

/// Compute the bitmap of currently-committed sub-pages that hold no live
/// slot, given the snapshot of the commit mask and a separately-computed
/// live-slot bitmap.
///
/// Drives the deadline-driven decommit decision: each set bit names a
/// sub-page the drain step will clear from the commit mask and then
/// release via the decommit syscall.
///
/// \param committed_mask    Snapshot of the commit mask (typically from
///                          \c snapshot_subpage_mask).
/// \param live_subpage_mask Bit <tt>i</tt> set iff sub-page <tt>i</tt>
///                          contains at least one live slot. Computed by
///                          walking the descriptor's free / local-free /
///                          thread-free bitmaps, inverting to liveness,
///                          and OR-folding
///                          <tt>subpage_range_to_mask(subpage_range_for_slot(...))</tt>
///                          for each live slot.
/// \returns a bitmap of sub-pages eligible for decommit (currently
///          committed AND no live slot).
[[nodiscard]] LIBC_INLINE constexpr uint16_t
compute_idle_subpages(uint16_t committed_mask, uint16_t live_subpage_mask) {
  return static_cast<uint16_t>(committed_mask &
                                static_cast<uint16_t>(~live_subpage_mask));
}

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
