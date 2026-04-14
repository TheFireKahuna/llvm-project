//===-- RAII guard for the remap protocol -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RemapGuard owns the remap "envelope": begin_remap (lock entry, arm VEH
// guard), snapshot protections + COW, unmap to placeholder, and automatic
// RAII rollback. Callers do their custom post-unmap logic (split + remap
// fragments, NUMA rebind, remap_file_pages) then commit or let the
// destructor roll back.
//
// Two consumers:
//   - RemapTransaction: standard split-remap (adds fragment logic on top)
//   - Direct callers: NUMA rebind, remap_file_pages (custom remap logic)
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REMAP_GUARD_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REMAP_GUARD_H

#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/region_snapshot.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

class RemapGuard {
public:
  RemapGuard(void *view_base, SIZE_T guarded_size);
  ~RemapGuard();

  RemapGuard(const RemapGuard &) = delete;
  RemapGuard &operator=(const RemapGuard &) = delete;

  /// Phase 0: Lock entry via begin_remap, snapshot protections + COW,
  /// duplicate section handle for rollback independence.
  bool prepare();

  /// Phase 1: Unmap the view to a placeholder. After this, the caller
  /// has a placeholder at [view_base, view_base + guarded_size) and can
  /// split/remap as needed.
  bool unmap();

  /// Terminal: publish the new mapping via commit_remap.
  void commit(void *new_base, SIZE_T size, ViewSpec spec);

  /// Terminal: irreversible loss (mapping discarded).
  void discard();

  /// Access snapshotted state.
  const MappingEntry &entry() const { return entry_; }
  const RegionRecord *records() const { return records_; }
  int record_count() const { return rec_count_; }
  CowContext &cow() { return cow_; }
  HANDLE owned_section() const { return owned_section_; }

  /// Whether the guard has been committed or discarded.
  bool is_terminal() const {
    return phase_ == Phase::COMMITTED || phase_ == Phase::DISCARDED;
  }

private:
  uintptr_t view_base_;
  SIZE_T guarded_size_;

  MappingEntry entry_ = {};
  HANDLE owned_section_ = nullptr;

  static constexpr int MAX_RECORDS = 128;
  RegionRecord records_[MAX_RECORDS];
  int rec_count_ = 0;
  CowContext cow_;

  enum class Phase : uint8_t {
    INIT,
    PREPARED,
    UNMAPPED,
    COMMITTED,
    DISCARDED,
  };
  Phase phase_ = Phase::INIT;

  void rollback();
  void rollback_from_prepared();
  void rollback_from_unmapped();
  void release_resources();
};

/// Diagnostic: number of rollback_from_unmapped() calls where re-remapping
/// the original view failed — the VA range is permanently lost as a
/// placeholder. Useful for crash-dump analysis.
uint32_t get_remap_rollback_failures();

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REMAP_GUARD_H
