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
// placeholder, remap the kept left/right fragments, and return the target
// placeholder to the caller.
//
// Built on RemapGuard (which owns the remap envelope: begin_remap, snapshot,
// COW, unmap, and RAII rollback). RemapTransaction adds the split + fragment
// remap logic.
//
// For operations that need custom post-unmap logic (NUMA rebind,
// remap_file_pages), use RemapGuard directly instead.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REMAP_TRANSACTION_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REMAP_TRANSACTION_H

#include "src/__support/OSUtil/windows/alloc/placeholder_range.h"
#include "src/__support/OSUtil/windows/memory/remap_guard.h"
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

  ~RemapTransaction() = default; // guard_ destructor handles rollback.

  RemapTransaction(const RemapTransaction &) = delete;
  RemapTransaction &operator=(const RemapTransaction &) = delete;

  /// Phase 0: Lock entry, snapshot, COW save. Returns false on failure.
  bool prepare();

  /// Phases 1-3: Unmap, split, remap kept fragments.
  Result execute();

  /// Phase 4: Publish mapping table entries for kept fragments.
  void commit();

  const MappingEntry &entry() const { return guard_.entry(); }
  bool has_left() const { return target_start_ > view_base_; }
  bool has_right() const { return target_end_ < view_end_; }

private:
  RemapGuard guard_;

  uintptr_t view_base_;
  uintptr_t view_end_;
  uintptr_t target_start_;
  uintptr_t target_end_;
  SIZE_T target_size_;

  // Fragment-specific snapshot data (subsets of guard_.records()).
  static constexpr int MAX_RECORDS = 128;
  RegionRecord left_records_[MAX_RECORDS];
  RegionRecord right_records_[MAX_RECORDS];
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
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REMAP_TRANSACTION_H
