//===-- RAII guard for MAP_FIXED preparation ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// FixedRangeGuard encapsulates the MAP_FIXED preparation protocol:
//
//   1. validate_map_fixed_target  — reject critical foreign regions
//   2. begin_remap_guard          — arm VEH stall sentinel (REMAPPING)
//   3. g_mmap_lock.acquire_exclusive — serialize against concurrent MAP_FIXED
//   4. prepare_for_fixed          — tear down existing VA in the range
//
// After prepare() succeeds, the VA state depends on alignment:
//   - 64KB-aligned addr: contiguous placeholder — use from_raw()
//   - Non-64KB addr: MEM_FREE — use reserve/reserve_with_retry()
//
// Destruction releases the lock (if still held) and aborts the remap guard
// (if not committed). All error paths are handled automatically — callers
// never need explicit cleanup.
//
// Safety properties:
//   - Fail-safe on forgotten mark_committed(): abort_remap_guard is a no-op
//     if commit_remap already transitioned the slot out of REMAPPING.
//   - Lock and guard are independent: abort works whether lock is held or not.
//   - prepare_for_fixed partial VA mutation is a pre-existing property that
//     this RAII inherits but does not worsen; placeholder intermediate state
//     is strictly better than MEM_FREE on partial failure.
//
// Three consumers in mmap_engine.cpp:
//   - alloc_large_pages (MAP_FIXED + MAP_HUGETLB)
//   - alloc_file_large_pages (MAP_FIXED + MAP_HUGETLB + file)
//   - alloc_file_fixed (MAP_FIXED + file)
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_FIXED_RANGE_GUARD_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_FIXED_RANGE_GUARD_H

#include "src/__support/OSUtil/windows/alloc/pagemap_classifier.h"
#include "src/__support/OSUtil/windows/memory/legacy/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_region.h"
#include "src/__support/OSUtil/windows/memory/legacy/mmap_lock.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include "hdr/errno_macros.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

class FixedRangeGuard {
  void *addr_ = nullptr;
  SIZE_T size_ = 0;
  bool guard_armed_ = false;
  bool committed_ = false;
  // Lock state is owned by an embedded MmapLockWriterGuard so the lock is
  // self-managing. Constructed deferred so the member exists even before
  // prepare() runs; prepare() acquires, release_lock() releases, and the
  // ~MmapLockWriterGuard releases on any error path before our own ~dtor.
  // Non-movable, which forces FixedRangeGuard itself to be non-movable —
  // intentional: there are no callers that move FixedRangeGuard, and a
  // movable lock guard would silently double-release on the source.
  MmapLockWriterGuard lock_guard_{MmapLockWriterGuard::defer_acquire};

public:
  LIBC_INLINE FixedRangeGuard() = default;

  LIBC_INLINE ~FixedRangeGuard() {
    // ~lock_guard_ releases the mmap lock if still held.
    if (guard_armed_ && !committed_)
      g_mapping_table.abort_remap_guard(addr_);
  }

  FixedRangeGuard(const FixedRangeGuard &) = delete;
  FixedRangeGuard &operator=(const FixedRangeGuard &) = delete;
  FixedRangeGuard(FixedRangeGuard &&) = delete;
  FixedRangeGuard &operator=(FixedRangeGuard &&) = delete;

  /// Phase 0: validate → arm remap guard → acquire lock → prepare_for_fixed.
  ///
  /// Returns 0 on success, errno on failure. On failure the destructor
  /// handles all cleanup — callers can simply return the negated errno.
  ///
  /// After success: the mmap lock is held exclusively, the remap guard
  /// sentinel is armed, and the VA range [addr, addr+size) is ready.
  /// For 64KB-aligned addr: contiguous placeholder (use from_raw).
  /// For non-64KB addr: MEM_FREE (use reserve_with_retry).
  [[nodiscard]] LIBC_INLINE int prepare(void *addr, SIZE_T size) {
    addr_ = addr;
    size_ = size;

    if (int err = alloc::pagemap::validate_map_fixed_target(addr, size))
      return err;

    if (!g_mapping_table.begin_remap_guard(addr, size))
      return ENOMEM;
    guard_armed_ = true;

    lock_guard_.acquire();

    if (!prepare_for_fixed(addr, size))
      return EINVAL; // ~FixedRangeGuard releases lock + aborts guard.

    return 0;
  }

  /// Release the exclusive mmap lock. Call after the placeholder is created
  /// but before the mapping operation (which does not need the lock).
  /// Idempotent: MmapLockWriterGuard::release() is a no-op if not held.
  LIBC_INLINE void release_lock() { lock_guard_.release(); }

  /// Mark as committed. Call after the caller has successfully called
  /// commit_remap on the mapping table. Prevents the destructor from
  /// calling abort_remap_guard.
  ///
  /// Forgetting this call is safe (defense-in-depth): abort_remap_guard
  /// is a no-op if commit_remap already transitioned the slot to LIVE.
  LIBC_INLINE void mark_committed() { committed_ = true; }

  LIBC_INLINE void *addr() const { return addr_; }
  LIBC_INLINE SIZE_T size() const { return size_; }
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_FIXED_RANGE_GUARD_H
