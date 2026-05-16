//===-- RAII guard for the remap protocol -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RemapGuard owns the remap "envelope": begin_remap (capture the slot's
// region reference, arm the VEH remap guard), snapshot protections + CoW,
// unmap to placeholder, and automatic RAII rollback. Callers do their
// custom post-unmap logic (split + remap fragments, NUMA rebind,
// remap_file_pages) then commit or let the destructor roll back.
//
// Two consumers:
//   - RemapTransaction: standard split-remap (adds fragment logic on top)
//   - Direct callers: NUMA rebind, remap_file_pages (custom remap logic)
//
// Ownership model (new region/shape design):
//
//   begin_remap transitions the slot LIVE -> REMAPPING and snapshots
//   (region_id, alloc_id, view_prot, flags) into the MappingEntry. Until
//   commit / abort / discard, the slot retains its +1 reference on the
//   region; no other thread can mutate the slot while REMAPPING. The
//   section / file handles can therefore be read directly from the
//   resolved RegionDesc without any caller-side duplication.
//
//   Terminal states:
//     commit  — publish the new mapping. Returns false and leaves the
//               guard in a rollback-on-destruction state if the
//               register/publish step fails.
//     abort   — put the original MappingEntry back (used on unmap or
//               fragment-remap failure); slot returns to LIVE.
//     discard — drop the slot's region reference; slot returns to FREE.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REMAP_GUARD_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REMAP_GUARD_H

#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/memory/legacy/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_desc.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_pool.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_snapshot.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

class RemapGuard {
public:
  RemapGuard(void *view_base, SIZE_T guarded_size);
  ~RemapGuard();

  RemapGuard(const RemapGuard &) = delete;
  RemapGuard &operator=(const RemapGuard &) = delete;

  RemapGuard(RemapGuard &&other);
  RemapGuard &operator=(RemapGuard &&other);

  /// Phase 0: Lock entry via begin_remap, snapshot per-page protections,
  /// save CoW pages when the region carries region_flag::COW.
  ///
  /// `out_stale` (optional) reports the begin_remap stale-snapshot signal
  /// — set to true iff the slot was rebound to a different extent between
  /// the caller's snapshot and our CAS. The caller should re-snapshot
  /// rather than treat the false return as a hard failure.
  [[nodiscard]] bool prepare(bool *out_stale = nullptr);

  /// Phase 1: Unmap the view to a placeholder. After this, the caller
  /// has a placeholder at [view_base, view_base + guarded_size) and can
  /// split/remap as needed.
  [[nodiscard]] bool unmap();

  /// Terminal: publish a new mapping at the guarded view_base. Returns
  /// false when the table publish step fails; on failure the guard stays
  /// non-terminal so the destructor rolls back from the UNMAPPED state.
  [[nodiscard]] bool commit(void *new_base, SIZE_T size, uint32_t region_id,
                            uint8_t alloc_id, DWORD prot, DWORD flags);

  /// Terminal: irreversible loss (mapping discarded). Releases the slot's
  /// region reference.
  void discard();

  /// Access snapshotted state.
  LIBC_INLINE const MappingEntry &entry() const { return entry_; }
  LIBC_INLINE const RegionRecord *records() const { return records_.data(); }
  LIBC_INLINE int record_count() const { return rec_count_; }
  LIBC_INLINE CowContext &cow() { return cow_; }

  /// Region descriptor captured at prepare() time. Live until commit /
  /// abort / discard. Section / file handles read directly from here.
  /// Null only when prepare() has not been called yet.
  LIBC_INLINE memory::RegionDesc *region() { return region_; }

  /// Convenience: section handle from the resolved region, or null when
  /// the region has no section (anonymous shapes).
  LIBC_INLINE HANDLE section_handle() const {
    return region_ != nullptr ? region_->section_handle : nullptr;
  }

  /// Whether the guard has been committed or discarded.
  LIBC_INLINE bool is_terminal() const {
    return phase_ == Phase::COMMITTED || phase_ == Phase::DISCARDED;
  }

private:
  uintptr_t view_base_;
  SIZE_T guarded_size_;

  MappingEntry entry_ = {};
  memory::RegionDesc *region_ = nullptr;

  // Dynamically allocated via thread-local scratch arena. No fixed cap —
  // overflow arenas extend transparently. Allocated in prepare(), freed
  // on destruction (or transferred via move).
  internal::ScratchAlloc<RegionRecord> records_{0};
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
};

/// Diagnostic: number of rollback_from_unmapped() calls where re-remapping
/// the original view failed — the VA range is permanently lost as a
/// placeholder. Useful for crash-dump analysis.
uint32_t get_remap_rollback_failures();

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REMAP_GUARD_H
