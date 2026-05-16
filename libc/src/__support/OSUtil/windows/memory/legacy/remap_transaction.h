//===-- Transactional split-remap for memory subsystem ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RemapTransaction encapsulates the split-remap protocol: given an existing
// section view and a target range within it, carve out the target range as a
// placeholder, remap the kept left/right fragments, publish them as mapping
// table slots backed by the same region, and return the target placeholder
// to the caller.
//
// Built on RemapGuard (which owns the remap envelope: begin_remap, snapshot,
// CoW, unmap, and RAII rollback). RemapTransaction adds the split + fragment
// remap logic and the shape mutation for file-view partial unmaps
// (FILE_VIEW_MONO -> FILE_VIEW_CHUNKED, or chunk-list punch on already
// chunked regions).
//
// Ownership model:
//   begin_remap captures the slot's +1 reference on the region. The
//   transaction inherits that reference and uses it to commit one of the
//   two kept fragments (left if present, else right). When both fragments
//   survive, a second +1 is added to the region via g_region_pool.add_ref
//   and consumed by register_mapping for the additional slot. On any
//   publish failure the add_ref is released.
//
// For operations that need custom post-unmap logic (NUMA rebind,
// remap_file_pages), use RemapGuard directly instead.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REMAP_TRANSACTION_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REMAP_TRANSACTION_H

#include "src/__support/OSUtil/windows/alloc/legacy/placeholder_range.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_desc.h"
#include "src/__support/OSUtil/windows/memory/legacy/remap_guard.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

class RemapTransaction {
public:
  struct Result {
    PlaceholderRange target; // Owned placeholder for the carved-out range.
    bool left_ok;
    bool right_ok;
  };

  /// Construct for carving [target_start, target_start+target_size) out of
  /// the view at [view_base, view_end).
  RemapTransaction(void *view_base, void *view_end,
                   uintptr_t target_start, SIZE_T target_size);

  LIBC_INLINE ~RemapTransaction() = default;

  RemapTransaction(const RemapTransaction &) = delete;
  RemapTransaction &operator=(const RemapTransaction &) = delete;
  LIBC_INLINE RemapTransaction(RemapTransaction &&) = default;
  LIBC_INLINE RemapTransaction &operator=(RemapTransaction &&) = default;

  /// Phase 0: Lock entry, snapshot, CoW save.
  ///
  /// `out_stale` (optional) propagates the begin_remap stale-snapshot
  /// signal — see RemapGuard::prepare.
  [[nodiscard]] bool prepare(bool *out_stale = nullptr);

  /// Phases 1-3: Unmap, split, remap kept fragments.
  Result execute();

  /// Phase 4: Publish mapping-table entries for the kept fragments and
  /// perform the shape mutation (MONO -> CHUNKED promotion, or chunk-list
  /// punch when already CHUNKED). Returns false on publish failure; on
  /// failure the guarded range is left in a best-effort consistent state
  /// and the caller must propagate -ENOMEM.
  [[nodiscard]] bool commit();

  LIBC_INLINE const MappingEntry &entry() const { return guard_.entry(); }
  LIBC_INLINE memory::RegionDesc *region() { return guard_.region(); }
  LIBC_INLINE bool has_left() const { return target_start_ > view_base_; }
  LIBC_INLINE bool has_right() const { return target_end_ < view_end_; }

private:
  RemapGuard guard_;

  uintptr_t view_base_;
  uintptr_t view_end_;
  uintptr_t target_start_;
  uintptr_t target_end_;
  SIZE_T target_size_;

  // Fragment-specific snapshot data (subsets of guard_.records()).
  // Allocated from thread-local scratch arena — no fixed cap.
  internal::ScratchAlloc<RegionRecord> left_records_{0};
  internal::ScratchAlloc<RegionRecord> right_records_{0};
  int left_rec_count_ = 0;
  int right_rec_count_ = 0;
  CowContext left_cow_;
  CowContext right_cow_;

  bool left_remapped_ = false;
  bool right_remapped_ = false;
  bool committed_ = false;

  bool snapshot_fragment(uintptr_t start, uintptr_t end,
                         RegionRecord *records, int *count,
                         CowContext *cow);
  bool remap_fragment_impl(uintptr_t frag_base, SIZE_T frag_size,
                           const RegionRecord *records, int rec_count,
                           const CowContext &cow);

  // Publish the second LIVE slot when both fragments survive. Handles
  // refcount bookkeeping and, on register_mapping failure, undoes the
  // view so the region's invariants hold. Returns false on failure.
  bool publish_second_fragment(void *base, SIZE_T size);

  // Mutate the region's shape for the post-commit state. Called with the
  // first fragment already published via commit_remap (so the slot's
  // view_base/size are the first fragment's). Returns false on OOM during
  // chunk-list allocation/growth — caller must treat as publish failure.
  bool apply_shape_mutation(void *first_base, SIZE_T first_size,
                            void *second_base, SIZE_T second_size);
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REMAP_TRANSACTION_H
