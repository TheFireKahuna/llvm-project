//===-- Guarded VA region primitive ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Unifies the "reserve N × 64 KB of VA, commit some regions, leave others
// uncommitted as guards" pattern shared by IndexedPool and thread_scratch.
// SlabPool uses a placeholder-based flow tied to its recycle path and is
// deliberately out of scope — adding placeholder methods here would broaden
// the API surface for a single caller whose VA lifecycle is lifecycle-
// coupled to recycling, not to reserve-and-forget.
//
// Real geometries this must express (every byte of the reservation must be
// covered by exactly one entry; reservation edges act as implicit guards):
//
//   IndexedPool    [guard 4K  | data var | guard 4K  | meta 4K ]
//                  (indexed_pool.h:98-108)
//   thread_scratch [control 4K| guard 4K | data 52K  | guard 4K]
//                  (thread_scratch.h:95-105)
//
// Note thread_scratch's control page has NO leading guard — it sits at the
// reservation base. That is pre-existing behaviour; a migration under the
// 0-regression contract must preserve it. The validator therefore treats
// reservation edges as implicit guards: a DATA/META bracketed by GUARD OR
// edge on each side is legal.
//
// Ownership: move-only RAII. Destructor releases the full reservation.
// Per-region commit/decommit operate by index; commit ordering is caller-
// driven (thread_scratch commits the control page eagerly and grows the
// data region lazily).
//
// Thread safety: not thread-safe. Kernel VA operations are atomic per
// range, but the wrapper's plain members aren't. Callers that need
// concurrent access wrap in their own lock or keep the object per-thread.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PRIMITIVES_GUARDED_REGION_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PRIMITIVES_GUARDED_REGION_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/common.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace alloc_primitives {

// x86-64 / AArch64 NT-POSIX fixed-page build. Matches the implicit
// assumptions throughout slab_pool/indexed_pool/thread_scratch.
inline constexpr size_t GUARDED_REGION_PAGE_SIZE = 4096;
inline constexpr size_t GUARDED_REGION_ALLOC_GRANULARITY = 65536;

// Role of a region inside a guarded reservation.
//   GUARD — uncommitted. Access faults via MMU. Never committed by the
//           primitive; callers must not commit_region/commit_subrange a
//           GUARD entry.
//   DATA  — caller-committable. May be committed at reserve time
//           (commit_eager=true) or deferred to commit_region/subrange.
//   META  — caller-committable. Same mechanics as DATA; the separate tag
//           exists so callers that have both (e.g. IndexedPool slot data
//           vs ChunkMeta) can document intent in the layout table.
enum class RegionKind : uint8_t {
  GUARD = 0,
  DATA = 1,
  META = 2,
};

// One entry in a Layout. offset + size must fit inside the reservation
// and must be page-aligned. Regions must be non-overlapping, listed in
// ascending order, and cover the entire reservation (entries[0].offset
// == 0, sum of sizes == total_size). reserve() validates at runtime;
// Layout::is_valid() provides the same check constexpr.
struct RegionSpec {
  size_t offset;
  size_t size;
  RegionKind kind;
  bool commit_eager; // If true, reserve() commits this region after mapping.
                     // Ignored for GUARD.
};

// A Layout is a small ordered array of RegionSpec. The primitive accepts
// a pointer + count so callers can define layouts at namespace scope as
// `inline constexpr RegionSpec kChunkSpecs[] = { ... };` and build the
// Layout likewise constexpr. total_size() is derived from the entries;
// the sum of entry sizes must be a multiple of 64 KB allocation
// granularity. Keeping the total computed rather than stored removes a
// drift axis — the old form accepted a hand-written total that had to
// equal sum(entries[i].size) and was only cross-checked in is_valid().
struct Layout {
  const RegionSpec *entries;
  unsigned count;

  // Sum of entry sizes. Equal to the VA footprint a reserve() call will
  // request. constexpr so callers can static_assert on it and reserve()
  // folds it at compile time for namespace-scope constexpr layouts.
  LIBC_INLINE constexpr size_t total_size() const {
    size_t sum = 0;
    for (unsigned i = 0; i < count; ++i)
      sum += entries[i].size;
    return sum;
  }

  // Compile-time checkable validator. Identical to reserve()'s runtime
  // check; exposed so callers can static_assert on their Layout.
  LIBC_INLINE constexpr bool is_valid() const {
    if (!entries || count == 0)
      return false;
    size_t expected_offset = 0;
    for (unsigned i = 0; i < count; ++i) {
      const RegionSpec &e = entries[i];
      if (e.offset != expected_offset)
        return false;
      if (e.size == 0)
        return false;
      if (e.offset & (GUARDED_REGION_PAGE_SIZE - 1))
        return false;
      if (e.size & (GUARDED_REGION_PAGE_SIZE - 1))
        return false;
      // Bracket rule: every committable region must have a GUARD or the
      // reservation edge on each side. No two committable regions may
      // touch — otherwise a buffer overrun in one silently enters the
      // other with no MMU fault.
      if (e.kind != RegionKind::GUARD) {
        bool left_guarded =
            (i == 0) || entries[i - 1].kind == RegionKind::GUARD;
        bool right_guarded =
            (i + 1 == count) || entries[i + 1].kind == RegionKind::GUARD;
        if (!left_guarded || !right_guarded)
          return false;
      }
      expected_offset += e.size;
    }
    // Accumulated size must be non-zero and a 64 KB multiple — matches
    // page_reserve()'s allocation-granularity requirement.
    return expected_offset != 0 &&
           (expected_offset & (GUARDED_REGION_ALLOC_GRANULARITY - 1)) == 0;
  }
};

// A reserved VA range composed of guard + committable regions per Layout.
// Constructed empty; reserve() does the VA work.
class GuardedRegion {
  void *base_ = nullptr;
  size_t total_size_ = 0;
  const RegionSpec *entries_ = nullptr;
  unsigned count_ = 0;

public:
  LIBC_INLINE GuardedRegion() = default;
  LIBC_INLINE ~GuardedRegion() { release(); }

  GuardedRegion(const GuardedRegion &) = delete;
  GuardedRegion &operator=(const GuardedRegion &) = delete;

  LIBC_INLINE GuardedRegion(GuardedRegion &&other) noexcept
      : base_(other.base_), total_size_(other.total_size_),
        entries_(other.entries_), count_(other.count_) {
    other.base_ = nullptr;
    other.total_size_ = 0;
    other.entries_ = nullptr;
    other.count_ = 0;
  }
  LIBC_INLINE GuardedRegion &operator=(GuardedRegion &&other) noexcept {
    if (this != &other) {
      release();
      base_ = other.base_;
      total_size_ = other.total_size_;
      entries_ = other.entries_;
      count_ = other.count_;
      other.base_ = nullptr;
      other.total_size_ = 0;
      other.entries_ = nullptr;
      other.count_ = 0;
    }
    return *this;
  }

  // Reserve VA covering the layout and commit any commit_eager=true
  // regions. Layout pointer must outlive the GuardedRegion. On any commit
  // failure, the reservation is released before return — the object stays
  // in the empty state.
  [[nodiscard]] LIBC_INLINE bool reserve(const Layout &layout) {
    LIBC_ASSERT(base_ == nullptr && "GuardedRegion already reserved");
    if (!layout.is_valid())
      return false;
    const size_t total = layout.total_size();
    void *mem = page_reserve(total);
    if (!mem)
      return false;
    auto *b = static_cast<char *>(mem);
    for (unsigned i = 0; i < layout.count; ++i) {
      const RegionSpec &e = layout.entries[i];
      if (e.kind == RegionKind::GUARD || !e.commit_eager)
        continue;
      if (!page_commit(b + e.offset, e.size)) {
        page_free(mem);
        return false;
      }
    }
    base_ = mem;
    total_size_ = total;
    entries_ = layout.entries;
    count_ = layout.count;
    return true;
  }

  // Commit a previously-reserved DATA/META region by its layout index.
  // page_commit is idempotent on already-committed pages, so this is safe
  // to call on a commit_eager=true region after reserve().
  [[nodiscard]] LIBC_INLINE bool commit_region(unsigned region_index) {
    return GuardedRegion::commit_region(base_, layout(), region_index);
  }

  // Commit a page-aligned subrange within a DATA/META region. offset is
  // relative to the region's start, not the reservation. Used for lazy
  // per-page commit (IndexedPool recommit, thread_scratch arena growth).
  // size == 0 is a no-op success.
  [[nodiscard]] LIBC_INLINE bool
  commit_subrange(unsigned region_index, size_t offset, size_t size) {
    return GuardedRegion::commit_subrange(base_, layout(), region_index,
                                          offset, size);
  }

  // Decommit a DATA/META subrange. See the static overload for semantics.
  LIBC_INLINE void decommit_subrange(unsigned region_index, size_t offset,
                                     size_t size, bool reset_only) {
    GuardedRegion::decommit_subrange(base_, layout(), region_index, offset,
                                     size, reset_only);
  }

  // Release the entire reservation (MEM_RELEASE). Safe on default-
  // constructed or moved-from instances — no-op if base_ is null.
  // Members are cleared BEFORE the static release() runs page_free so
  // that if this GuardedRegion instance happens to live inside the very
  // memory being released (e.g. a future migration embeds it in a
  // per-chunk metadata page), we do not write back to unmapped memory
  // after the free.
  LIBC_INLINE void release() {
    void *b = base_;
    base_ = nullptr;
    total_size_ = 0;
    entries_ = nullptr;
    count_ = 0;
    GuardedRegion::release(b);
  }

  // Relinquish ownership WITHOUT freeing. Returns the reservation base
  // pointer and resets this instance to the empty state. The caller
  // becomes responsible for eventually calling page_free(base) (directly
  // or via a new borrowed GuardedRegion).
  //
  // Used by callers (IndexedPool::alloc_chunk) that want the primitive's
  // validated reserve+commit+rollback sequence but manage the VA
  // lifetime by raw pointer afterwards (because the raw pointer is the
  // stable identity stored in a lock-free directory, and reconstructing
  // a full GuardedRegion per operation would add needless storage).
  [[nodiscard]] LIBC_INLINE void *detach() {
    void *b = base_;
    base_ = nullptr;
    total_size_ = 0;
    entries_ = nullptr;
    count_ = 0;
    return b;
  }

  // Accessors — cheap, inlineable, no syscalls. Callers that cache the
  // region base (IndexedPool stores slot_base in ChunkMeta) pay one
  // region_base() per reserve(), not per alloc.
  LIBC_INLINE void *region_base(unsigned region_index) const {
    LIBC_ASSERT(base_ != nullptr);
    LIBC_ASSERT(region_index < count_);
    return static_cast<char *>(base_) + entries_[region_index].offset;
  }
  LIBC_INLINE size_t region_size(unsigned region_index) const {
    LIBC_ASSERT(region_index < count_);
    return entries_[region_index].size;
  }
  LIBC_INLINE RegionKind region_kind(unsigned region_index) const {
    LIBC_ASSERT(region_index < count_);
    return entries_[region_index].kind;
  }
  LIBC_INLINE void *reservation_base() const { return base_; }
  LIBC_INLINE size_t reservation_size() const { return total_size_; }
  LIBC_INLINE bool valid() const { return base_ != nullptr; }

  // Reconstruct the Layout descriptor this instance was reserved with.
  // Used by instance methods to delegate to the static typed API below.
  LIBC_INLINE Layout layout() const { return Layout{entries_, count_}; }

  // -----------------------------------------------------------------------
  // Stateless typed API
  // -----------------------------------------------------------------------
  //
  // Callers that manage the reservation lifetime by raw pointer (the
  // reservation base is the stable identity stored elsewhere — e.g.
  // IndexedPool's lock-free directory uses T* slot pointers whose
  // reservation base is slot_base - DATA_OFFSET) use these static
  // helpers to perform the same bounds-checked typed ops as the instance
  // API without having to reconstruct a GuardedRegion object per call.
  //
  // Every op asserts at the API boundary: base != nullptr, region_index
  // within layout.count, region.kind != GUARD. Subrange ops additionally
  // assert page alignment and offset + size <= region.size. Under NDEBUG
  // the asserts compile out and the body folds to exactly the same
  // page_* syscall as a raw call — zero overhead.
  //
  // The invariant "no GUARD region can ever be committed" is enforced
  // by this API surface: no code path that routes through these helpers
  // can accidentally commit a guard page, regardless of how the offsets
  // are computed at the call site.

  // Whole-region commit — equivalent to instance commit_region(i).
  [[nodiscard]] LIBC_INLINE static bool
  commit_region(void *base, const Layout &layout, unsigned region_index) {
    LIBC_ASSERT(base != nullptr);
    LIBC_ASSERT(region_index < layout.count);
    const RegionSpec &e = layout.entries[region_index];
    LIBC_ASSERT(e.kind != RegionKind::GUARD);
    return page_commit(static_cast<char *>(base) + e.offset, e.size);
  }

  // Sub-range commit — equivalent to instance commit_subrange(i, off, sz).
  // size == 0 is a no-op success: page_commit(sz=0) has undocumented NT
  // behaviour and the asserts compile out under NDEBUG, so the degenerate
  // case is handled here rather than upstream.
  [[nodiscard]] LIBC_INLINE static bool
  commit_subrange(void *base, const Layout &layout, unsigned region_index,
                  size_t offset, size_t size) {
    LIBC_ASSERT(base != nullptr);
    LIBC_ASSERT(region_index < layout.count);
    const RegionSpec &e = layout.entries[region_index];
    LIBC_ASSERT(e.kind != RegionKind::GUARD);
    LIBC_ASSERT((offset & (GUARDED_REGION_PAGE_SIZE - 1)) == 0);
    LIBC_ASSERT((size & (GUARDED_REGION_PAGE_SIZE - 1)) == 0);
    LIBC_ASSERT(offset + size <= e.size);
    if (size == 0)
      return true;
    return page_commit(static_cast<char *>(base) + e.offset + offset, size);
  }

  // Sub-range decommit or reset.
  //   reset_only=true   — MEM_RESET (advisory reclaim, pages stay
  //                       committed, next access does not fault). Used
  //                       for fast recycling on the hot path.
  //   reset_only=false  — MEM_DECOMMIT (hard, backing returned, VA
  //                       stays reserved). Used for fork_reinit.
  // GUARD regions are rejected via LIBC_ASSERT — their uncommitted
  // state is a layout invariant and must not be touched. size == 0 is
  // a no-op (same rationale as commit_subrange).
  LIBC_INLINE static void
  decommit_subrange(void *base, const Layout &layout, unsigned region_index,
                    size_t offset, size_t size, bool reset_only) {
    LIBC_ASSERT(base != nullptr);
    LIBC_ASSERT(region_index < layout.count);
    const RegionSpec &e = layout.entries[region_index];
    LIBC_ASSERT(e.kind != RegionKind::GUARD);
    LIBC_ASSERT((offset & (GUARDED_REGION_PAGE_SIZE - 1)) == 0);
    LIBC_ASSERT((size & (GUARDED_REGION_PAGE_SIZE - 1)) == 0);
    LIBC_ASSERT(offset + size <= e.size);
    if (size == 0)
      return;
    void *p = static_cast<char *>(base) + e.offset + offset;
    if (reset_only)
      page_reset(p, size);
    else
      page_decommit(p, size);
  }

  // Attempt to undo a prior MEM_RESET on a sub-range. Returns true if
  // the kernel preserved the pages (fast path: content is either intact
  // or already zero-filled — both acceptable for callers that treat
  // "all zero" as "empty"). Returns false if the kernel reclaimed them,
  // in which case the caller must commit_subrange to re-obtain backing.
  // GUARD regions are rejected via LIBC_ASSERT (same as decommit).
  [[nodiscard]] LIBC_INLINE static bool
  reset_undo_subrange(void *base, const Layout &layout, unsigned region_index,
                      size_t offset, size_t size) {
    LIBC_ASSERT(base != nullptr);
    LIBC_ASSERT(region_index < layout.count);
    const RegionSpec &e = layout.entries[region_index];
    LIBC_ASSERT(e.kind != RegionKind::GUARD);
    LIBC_ASSERT((offset & (GUARDED_REGION_PAGE_SIZE - 1)) == 0);
    LIBC_ASSERT((size & (GUARDED_REGION_PAGE_SIZE - 1)) == 0);
    LIBC_ASSERT(offset + size <= e.size);
    if (size == 0)
      return true;
    return page_reset_undo(static_cast<char *>(base) + e.offset + offset,
                           size);
  }

  // Release a reservation. Symmetric with reserve() on the instance API.
  // No layout is needed — page_free covers the entire reservation by
  // base pointer, as with the owning instance's release().
  LIBC_INLINE static void release(void *base) {
    if (base)
      page_free(base);
  }
};

} // namespace alloc_primitives
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PRIMITIVES_GUARDED_REGION_H
