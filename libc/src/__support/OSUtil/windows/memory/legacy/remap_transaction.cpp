//===-- RemapTransaction implementation --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "remap_transaction.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/legacy/mapping_table.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_desc.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_pool.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_snapshot.h"
#include "src/__support/OSUtil/windows/memory/legacy/view_spec.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// =====================================================================
// ViewSpec derivation from a resolved region
// =====================================================================
//
// ViewSpec is the transient value used by region_snapshot.h's CoW-aware
// remap helpers (remap_fragment / remap_with_cow_splits). It is never
// stored; we build one per fragment from the resolved RegionDesc and the
// captured MappingEntry view_prot/flags.

static ViewSpec spec_for_region(const memory::RegionDesc *region,
                                const MappingEntry &entry) {
  ViewSpec s{};
  if (region != nullptr) {
    s.section = region->section_handle;
    s.file = region->file_handle;
    s.offset = region->section_offset;
  }
  s.prot = entry.view_prot;
  s.flags = entry.flags;
  return s;
}

// =====================================================================
// Construction
// =====================================================================

RemapTransaction::RemapTransaction(void *view_base, void *view_end,
                                   uintptr_t target_start, SIZE_T target_size)
    : guard_(view_base,
             static_cast<SIZE_T>(reinterpret_cast<uintptr_t>(view_end) -
                                 reinterpret_cast<uintptr_t>(view_base))),
      view_base_(reinterpret_cast<uintptr_t>(view_base)),
      view_end_(reinterpret_cast<uintptr_t>(view_end)),
      target_start_(target_start),
      target_end_(target_start + target_size), target_size_(target_size) {}

// =====================================================================
// Phase 0: Prepare
// =====================================================================

bool RemapTransaction::prepare(bool *out_stale) {
  if (out_stale)
    *out_stale = false;
  if (!guard_.prepare(out_stale))
    return false;

  // CoW save per fragment is gated by the region flag (source of truth,
  // set at acquire time from the section's allocation protect) with a
  // fall-back to the per-record has_cow() MBI scan so an unported caller
  // that forgets to set the flag cannot silently drop dirty CoW pages.
  // snapshot_fragment passes nullptr cow when both signals say "no save
  // needed" for this fragment.
  const bool region_says_cow =
      guard_.region() != nullptr &&
      guard_.region()->has_flag(memory::region_flag::COW);

  if (has_left()) {
    left_records_ = internal::ScratchAlloc<RegionRecord>(MAX_REGION_RECORDS);
    if (!left_records_)
      return false;
    if (!snapshot_fragment(view_base_, target_start_, left_records_.data(),
                           &left_rec_count_, &left_cow_))
      return false;
    if (!region_says_cow &&
        !has_cow(left_records_.data(), left_rec_count_)) {
      // Confirmed no CoW work for this fragment — release the context so
      // downstream remap helpers short-circuit.
      left_cow_.release();
    }
  }

  if (has_right()) {
    right_records_ = internal::ScratchAlloc<RegionRecord>(MAX_REGION_RECORDS);
    if (!right_records_)
      return false;
    if (!snapshot_fragment(target_end_, view_end_, right_records_.data(),
                           &right_rec_count_, &right_cow_))
      return false;
    if (!region_says_cow &&
        !has_cow(right_records_.data(), right_rec_count_)) {
      right_cow_.release();
    }
  }

  return true;
}

// =====================================================================
// Phases 1-3: Execute
// =====================================================================

RemapTransaction::Result RemapTransaction::execute() {
  Result result = {{}, false, false};

  if (!guard_.unmap())
    return result;

  // Split placeholder at target boundaries.
  if (has_left()) {
    if (!nt_pal::split_placeholder(reinterpret_cast<void *>(view_base_),
                           target_start_ - view_base_))
      return result;
  }

  if (has_right()) {
    void *remainder_base =
        has_left() ? reinterpret_cast<void *>(target_start_)
                   : reinterpret_cast<void *>(view_base_);
    SIZE_T offset_to_target_end =
        target_end_ - reinterpret_cast<uintptr_t>(remainder_base);
    if (!nt_pal::split_placeholder(remainder_base, offset_to_target_end))
      return result;
  }

  // Remap kept fragments from the section.
  if (has_left()) {
    SIZE_T left_size = target_start_ - view_base_;
    left_remapped_ =
        remap_fragment_impl(view_base_, left_size, left_records_.data(),
                            left_rec_count_, left_cow_);
    result.left_ok = left_remapped_;
  }

  if (has_right()) {
    SIZE_T right_size = view_end_ - target_end_;
    right_remapped_ =
        remap_fragment_impl(target_end_, right_size, right_records_.data(),
                            right_rec_count_, right_cow_);
    result.right_ok = right_remapped_;
  }

  result.target = PlaceholderRange::from_raw(
      reinterpret_cast<void *>(target_start_), target_size_);
  return result;
}

// =====================================================================
// Phase 4: Commit
// =====================================================================
//
// Dispatch matrix:
//
//   has_left &&  has_right  → two surviving fragments: commit_remap(left)
//                             + publish_second_fragment(right) + shape
//                             mutation.
//   has_left && !has_right  → single left fragment: commit_remap(left)
//                             with same base; shape unchanged if MONO-
//                             with-new-size is valid, or chunk-list punch
//                             for CHUNKED.
//  !has_left &&  has_right  → single right fragment: commit_remap with
//                             different base (view_base → target_end).
//  !has_left && !has_right  → full unmap (caller should have routed to
//                             full_unmap_view). Discard as a safety net.

bool RemapTransaction::commit() {
  MappingEntry e = guard_.entry();
  const uint32_t region_id = e.region_id;
  const uint8_t alloc_id = e.alloc_id;
  const DWORD prot = e.view_prot;
  const DWORD flags = e.flags;
  void *vb = reinterpret_cast<void *>(view_base_);

  // Pre-install the chunk list before any irreversible publish. The
  // post-commit shape mutation only ever appends to inline storage for
  // the 1- or 2-chunk cases (INLINE_CAPACITY = 4), so once the list is
  // installed every later step is infallible. install_chunk_list is
  // idempotent: a CHUNKED region with a list already attached returns
  // the existing pointer.
  if (memory::RegionDesc *rd = guard_.region(); rd != nullptr) {
    if (rd->is_section_backed() && memory::install_chunk_list(rd) == nullptr)
      return false; // ~guard rolls back from UNMAPPED.
  }

  const bool both = has_left() && has_right() && left_remapped_ &&
                    right_remapped_;
  const bool left_only = has_left() && left_remapped_ &&
                         (!has_right() || !right_remapped_);
  const bool right_only = has_right() && right_remapped_ &&
                          (!has_left() || !left_remapped_);

  // After execute(), the placeholder graph for the surviving address range
  // is exactly:
  //   has_left  + !left_remapped_  → bare placeholder at view_base_
  //   has_right + !right_remapped_ → bare placeholder at target_end_
  // (Successfully remapped fragments are LIVE views, not placeholders, so
  // they're not part of this set.) Any branch below that does NOT consume
  // a given placeholder via guard_.commit / publish_second_fragment must
  // release it explicitly, otherwise the VA leaks into NT-tracked-but-
  // libc-untracked space — the same untracked-VA class the redesign was
  // written to eliminate.
  auto release_unmapped_left = [&] {
    if (has_left() && !left_remapped_)
      nt_pal::free_placeholder(reinterpret_cast<void *>(view_base_));
  };
  auto release_unmapped_right = [&] {
    if (has_right() && !right_remapped_)
      nt_pal::free_placeholder(reinterpret_cast<void *>(target_end_));
  };

  if (!both && !left_only && !right_only) {
    // Nothing survived. Drop the slot's region ref and release every
    // placeholder execute() left behind.
    release_unmapped_left();
    release_unmapped_right();
    guard_.discard();
    committed_ = true;
    return true;
  }

  if (both) {
    SIZE_T left_size = target_start_ - view_base_;
    void *right_base = reinterpret_cast<void *>(target_end_);
    SIZE_T right_size = view_end_ - target_end_;

    // Step 1: commit the left fragment in place (same base). This drains
    // the REMAPPING state and re-binds the slot to region_id with the new
    // size. No refcount change.
    if (!guard_.commit(vb, left_size, region_id, alloc_id, prot, flags))
      return false;

    // Step 2: publish the right fragment as a new LIVE slot. Consumes
    // +1 ref on region_id (acquired internally via add_ref).
    if (!publish_second_fragment(right_base, right_size))
      return false;

    // Step 3: shape mutation. MONO -> CHUNKED with two chunks; CHUNKED
    // already-split punch-hole.
    if (!apply_shape_mutation(vb, left_size, right_base, right_size))
      return false;

    committed_ = true;
    return true;
  }

  if (left_only) {
    SIZE_T left_size = target_start_ - view_base_;
    if (!guard_.commit(vb, left_size, region_id, alloc_id, prot, flags))
      return false;

    // Right side either had no fragment or its remap failed. In the
    // failed-remap case execute() left a bare placeholder at target_end_;
    // release it now that this branch has bound the surviving slot.
    release_unmapped_right();

    // Single-fragment survival of a CHUNKED region needs chunk-list
    // maintenance: drop the target range and any already-dead right
    // chunk out of the list.
    if (!apply_shape_mutation(vb, left_size, /*second_base=*/nullptr,
                              /*second_size=*/0))
      return false;

    committed_ = true;
    return true;
  }

  // right_only
  SIZE_T right_size = view_end_ - target_end_;
  void *right_base = reinterpret_cast<void *>(target_end_);
  if (!guard_.commit(right_base, right_size, region_id, alloc_id, prot,
                     flags))
    return false;

  // Symmetric to left_only above: release the orphaned left placeholder
  // before completing the publish.
  release_unmapped_left();

  if (!apply_shape_mutation(right_base, right_size, /*second_base=*/nullptr,
                            /*second_size=*/0))
    return false;

  committed_ = true;
  return true;
}

// =====================================================================
// Publish second fragment (new LIVE slot sharing the region)
// =====================================================================

bool RemapTransaction::publish_second_fragment(void *base, SIZE_T size) {
  const MappingEntry &e = guard_.entry();
  const uint32_t region_id = e.region_id;

  if (region_id == memory::RegionPool::NONE) {
    // Region-less entry (should not occur for file views, but guard
    // anyway). register_mapping with NONE would be a contract break.
    return false;
  }

  // Reserve the additional reference this slot will own.
  memory::g_region_pool.add_ref(region_id);

  if (g_mapping_table.register_mapping(base, size, region_id, e.alloc_id,
                                       e.view_prot, e.flags)) {
    return true;
  }

  // Publish failed. The view at `base` is already mapped — unmap it
  // back to a placeholder, release the placeholder to MEM_FREE, and
  // release the ref we acquired.
  nt_pal::unmap_view_preserve(base);
  nt_pal::free_placeholder(base);
  memory::g_region_pool.release(region_id);
  return false;
}

// =====================================================================
// Shape mutation (MONO -> CHUNKED promotion, CHUNKED punch-hole)
// =====================================================================

namespace {

// Per-region parking mutex covering chunk_list mutations. Acquire is a
// CAS 0->1; on contention the loser parks via futex_addr::wait, the
// releaser stores 0 and broadcasts via futex_addr::wake. Multi-waiter by
// design (N concurrent partial-unmaps of the same CHUNKED region all
// queue here) — that's why this isn't a ThreadLocalWord, which is
// single-owner-single-waiter. Contention is rare in practice.
class ChunkListLockGuard {
public:
  LIBC_INLINE explicit ChunkListLockGuard(memory::RegionDesc *rd) : rd_(rd) {
    if (rd_ == nullptr)
      return;
    for (;;) {
      uint8_t expected = 0;
      if (rd_->chunk_list_lock.compare_exchange_strong(
              expected, 1, cpp::MemoryOrder::ACQUIRE,
              cpp::MemoryOrder::RELAXED))
        return;
      // Busy: park on the byte. `wait` returns immediately if the
      // value already changed.
      futex_addr::wait<uint8_t>(&rd_->chunk_list_lock,
                                static_cast<uint8_t>(1), nullptr);
    }
  }

  LIBC_INLINE ~ChunkListLockGuard() {
    if (rd_ == nullptr)
      return;
    rd_->chunk_list_lock.store(0, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&rd_->chunk_list_lock, UINT32_MAX);
  }

  ChunkListLockGuard(const ChunkListLockGuard &) = delete;
  ChunkListLockGuard &operator=(const ChunkListLockGuard &) = delete;

private:
  memory::RegionDesc *rd_;
};

// Convert a VA within a region to 64 KB-relative base_units.
LIBC_INLINE uint32_t base_units_of(memory::RegionDesc *rd, void *addr) {
  const uintptr_t gran = get_alloc_granularity();
  const uintptr_t va = reinterpret_cast<uintptr_t>(addr);
  const uintptr_t region_start = rd->first_slot_key << 16;
  return static_cast<uint32_t>((va - region_start) / gran);
}

LIBC_INLINE uint32_t size_units_of(SIZE_T size) {
  const uintptr_t gran = get_alloc_granularity();
  return static_cast<uint32_t>(size / gran);
}

} // namespace

bool RemapTransaction::apply_shape_mutation(void *first_base, SIZE_T first_size,
                                            void *second_base,
                                            SIZE_T second_size) {
  memory::RegionDesc *rd = guard_.region();
  if (rd == nullptr)
    return true; // No region = nothing to maintain (anonymous path).

  // Shape mutation is scoped to section-backed regions (file-backed
  // shapes plus the pagefile-backed ANON_RESERVE_SECTION extension).
  // Pure anonymous shapes (ANON_PLACEHOLDER) carry all
  // state in their per-slot extent and do not need a chunk list.
  if (!rd->is_section_backed())
    return true;
  const memory::RegionShape shape = rd->current_shape();

  ChunkListLockGuard lk(rd);

  // Carved-out range (in 64 KB units, relative to the region).
  const uint32_t hole_base = base_units_of(rd, reinterpret_cast<void *>(
                                                   target_start_));
  const uint32_t hole_size = size_units_of(target_size_);

  if (shape == memory::RegionShape::FILE_VIEW_CHUNKED) {
    // Punch the hole out of whichever chunk currently covers the target
    // range. Mutation happens in place.
    memory::ChunkList *list =
        rd->chunk_list.load(cpp::MemoryOrder::ACQUIRE);
    if (list == nullptr)
      return false; // CHUNKED without a list is a structural invariant
                    // break.

    uint32_t idx = memory::chunk_list_find(list, hole_base);
    if (idx == memory::ChunkList::NPOS) {
      // Target was already absent — nothing to punch. Success.
      return true;
    }
    return memory::chunk_list_punch(list, idx, hole_base, hole_size);
  }

  // MONO (or RESERVE): promote to CHUNKED when we have two survivors, or
  // retain a single-chunk representation for a one-survivor partial
  // unmap that changed the bounds. In either case we install a chunk
  // list and flip shape to CHUNKED — this keeps the model uniform and
  // matches the plan's "every partial unmap promotes" intent.
  memory::ChunkList *list = memory::install_chunk_list(rd);
  if (list == nullptr)
    return false;

  // Re-seed the list with the surviving fragments. After promotion the
  // list is the source of truth for which 64 KB slots belong to the
  // region.
  list->count = 0;
  const uint32_t first_bu =
      base_units_of(rd, first_base);
  const uint32_t first_su = size_units_of(first_size);
  if (!memory::chunk_list_append(
          list, memory::ChunkEntry{first_bu, first_su}))
    return false;

  if (second_base != nullptr && second_size > 0) {
    const uint32_t second_bu = base_units_of(rd, second_base);
    const uint32_t second_su = size_units_of(second_size);
    if (!memory::chunk_list_append(
            list, memory::ChunkEntry{second_bu, second_su}))
      return false;
  }

  // Publish the shape transition. RELEASE so concurrent readers that
  // observe CHUNKED also observe a populated chunk_list.
  rd->shape.store(static_cast<uint16_t>(memory::RegionShape::FILE_VIEW_CHUNKED),
                  cpp::MemoryOrder::RELEASE);
  return true;
}

// =====================================================================
// Internal helpers
// =====================================================================

bool RemapTransaction::snapshot_fragment(uintptr_t start, uintptr_t end,
                                         RegionRecord *records, int *count,
                                         CowContext *cow) {
  int n = windows::snapshot_regions(start, end, records, MAX_REGION_RECORDS);
  if (n < 0)
    return false;
  *count = n;
  if (cow == nullptr)
    return true;
  return cow->prepare(start, records, n);
}

bool RemapTransaction::remap_fragment_impl(uintptr_t frag_base,
                                           SIZE_T frag_size,
                                           const RegionRecord *records,
                                           int rec_count,
                                           const CowContext &cow) {
  ViewSpec frag_spec = spec_for_region(guard_.region(), guard_.entry());
  // Section offset is anchored to the region's first slot, not to this
  // transaction's view_base_ — `view_base_` is whichever slot the caller
  // entered through, which after CHUNKED promotion may be any chunk in
  // the region. region_desc.h:266: "chunk view at section_offset +
  // chunk.base_units * 65536". Mirroring that here keeps every concurrent
  // partial-unmap path consistent.
  if (memory::RegionDesc *rd = guard_.region(); rd != nullptr) {
    const uintptr_t region_start = rd->first_slot_key << 16;
    frag_spec.offset.QuadPart +=
        static_cast<LONGLONG>(frag_base - region_start);
  } else {
    frag_spec.offset.QuadPart +=
        static_cast<LONGLONG>(frag_base - view_base_);
  }

  if (cow && has_cow(records, rec_count)) {
    return remap_with_cow_splits(frag_base, frag_size, frag_spec,
                                 cow.buffer(), records, rec_count);
  }
  return remap_fragment(frag_base, frag_size, frag_spec, records, rec_count);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
