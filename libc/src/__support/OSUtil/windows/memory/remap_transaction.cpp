//===-- RemapTransaction implementation --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "remap_transaction.h"

#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/memory/region_snapshot.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

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

bool RemapTransaction::prepare() {
  if (!guard_.prepare())
    return false;

  // Snapshot left fragment protections + COW (subset of guard's full range).
  if (has_left()) {
    if (!snapshot_fragment(view_base_, target_start_, left_records_,
                           &left_rec_count_, &left_cow_)) {
      // guard_ destructor will abort_remap.
      return false;
    }
  }

  // Snapshot right fragment.
  if (has_right()) {
    if (!snapshot_fragment(target_end_, view_end_, right_records_,
                           &right_rec_count_, &right_cow_)) {
      // CowContext RAII handles cleanup.
      return false;
    }
  }

  return true;
}

// =====================================================================
// Phases 1-3: Execute
// =====================================================================

RemapTransaction::Result RemapTransaction::execute() {
  Result result = {{}, false, false};

  if (!guard_.unmap()) {
    // CowContext RAII handles cleanup.
    return result;
  }

  // Split placeholder at target boundaries.
  if (has_left()) {
    if (!split_placeholder(reinterpret_cast<void *>(view_base_),
                           target_start_ - view_base_)) {
      // CowContext RAII handles cleanup.
      return result; // guard_ rollback re-remaps original view.
    }
  }

  if (has_right()) {
    void *remainder_base =
        has_left() ? reinterpret_cast<void *>(target_start_)
                   : reinterpret_cast<void *>(view_base_);
    SIZE_T offset_to_target_end =
        target_end_ - reinterpret_cast<uintptr_t>(remainder_base);
    if (!split_placeholder(remainder_base, offset_to_target_end)) {
      // CowContext RAII handles cleanup.
      return result;
    }
  }

  // Remap kept fragments.
  if (has_left()) {
    SIZE_T left_size = target_start_ - view_base_;
    left_remapped_ = remap_fragment_impl(view_base_, left_size,
                                         left_records_, left_rec_count_,
                                         left_cow_);
    result.left_ok = left_remapped_;
  }

  if (has_right()) {
    SIZE_T right_size = view_end_ - target_end_;
    right_remapped_ = remap_fragment_impl(target_end_, right_size,
                                          right_records_, right_rec_count_,
                                          right_cow_);
    result.right_ok = right_remapped_;
  }

  result.target = PlaceholderRange::from_raw(
      reinterpret_cast<void *>(target_start_), target_size_);
  return result;
}

// =====================================================================
// Phase 4: Commit
// =====================================================================

void RemapTransaction::commit() {
  const auto &e = guard_.entry();
  void *vb = reinterpret_cast<void *>(view_base_);
  bool first_committed = false;

  if (has_left() && left_remapped_) {
    SIZE_T left_size = target_start_ - view_base_;
    guard_.commit(vb, left_size, e.spec);
    first_committed = true;
  }

  if (has_right() && right_remapped_) {
    void *right_base = reinterpret_cast<void *>(target_end_);
    SIZE_T right_size = view_end_ - target_end_;
    ViewSpec right_spec = e.spec.at_offset(target_end_ - view_base_);

    if (!first_committed) {
      guard_.commit(right_base, right_size, right_spec);
      first_committed = true;
    } else {
      g_mapping_table.register_mapping(right_base, right_size, right_spec);
    }
  }

  if (!first_committed)
    guard_.discard();

  committed_ = true;
}

// =====================================================================
// Internal helpers
// =====================================================================

bool RemapTransaction::snapshot_fragment(uintptr_t start, uintptr_t end,
                                         RegionRecord *records, int *count,
                                         CowContext *cow) {
  int n = windows::snapshot_regions(start, end, records, MAX_RECORDS);
  if (n < 0)
    return false;
  *count = n;
  return cow->prepare(start, records, n);
}

bool RemapTransaction::remap_fragment_impl(uintptr_t frag_base,
                                           SIZE_T frag_size,
                                           const RegionRecord *records,
                                           int rec_count,
                                           const CowContext &cow) {
  ViewSpec frag_spec = guard_.entry().spec.at_offset(frag_base - view_base_);
  // Use owned section handle for rollback independence if available.
  if (guard_.owned_section())
    frag_spec.section = guard_.owned_section();

  if (cow && has_cow(records, rec_count)) {
    return remap_with_cow_splits(frag_base, frag_size, frag_spec, cow.buffer(),
                                 records, rec_count);
  }
  return remap_fragment(frag_base, frag_size, frag_spec, records, rec_count);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
