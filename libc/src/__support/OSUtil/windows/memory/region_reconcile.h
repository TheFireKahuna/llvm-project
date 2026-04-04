//===--- Region reconciliation — bulk VA scan + table sync -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Reconciliation runs at event boundaries where the mapping table may
// disagree with NT's actual VAD tree:
//
//   * post-fork  — child inherits parent VA + section views; needs to
//                  confirm the inherited set still matches NT reality and
//                  pick up any kernel-side adjustments.
//   * post-exec  — self-hollow tears most VA down; surviving regions must
//                  be re-validated, foreign placeholders re-stamped.
//   * post-dlopen — the loader can install IMAGE / placeholder VA inside
//                  ranges we hint over; cordon them as FOREIGN before the
//                  next mmap hits the gap.
//
// Each entry point:
//   1. Acquires MmapLock writer (excludes new mmap/munmap/mprotect).
//   2. Drains in-flight remaps (waits for active_remap_count_ == 0).
//   3. Walks the full VA via NtPssCaptureVaSpaceBulk (RegionWalker).
//   4. For each NT region not represented in our table, stamps a FOREIGN
//      slot at 64 KB granularity so subsequent hint/MAP_FIXED requests
//      avoid it.
//   5. Releases MmapLock.
//
// Reconciliation is rare — running it at each event is acceptable cost
// for the table-correctness guarantee. Hot mmap/munmap paths never call
// into here.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_RECONCILE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_RECONCILE_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace memory {

// Result counters returned by a reconcile pass — telemetry only, callers
// may discard. Useful for tests and logging the size of drift events.
struct ReconcileStats {
  unsigned foreign_stamped;     // FREE slots stamped FOREIGN this pass
  unsigned foreign_revalidated; // FOREIGN slots confirmed still foreign
  unsigned foreign_cleared;     // FOREIGN slots cleared (NT freed the VA)
};

// Post-fork reconciliation. Child invokes this from MmapLock::fork_reinit
// after the RegionPool::fork_reinit call has restored pool-side state.
// Returns drift stats.
ReconcileStats post_fork_scan();

// Post-exec reconciliation. Called after self-hollow has cleared the
// non-PCB VA. The mapping-table state for surviving regions is left in
// place; this pass picks up the post-hollow holes as new FREE space and
// stamps anything the new image header / loader installed.
ReconcileStats post_exec_scan();

// Post-dlopen / pre-dlclose reconciliation. Hooked into the dynamic
// loader's notification callback. Bounds-the-window tighter via
// RegionWalker over [base, base + module size] when known; falls back to
// full scan if the loader hasn't reported a range.
ReconcileStats post_dlopen_reconcile(void *bound_start, SIZE_T bound_size);

// Scoped FOREIGN cordon for a specific range. Bulk-walks NT VA over
// [base, base + size) via NtPssCaptureVaSpaceBulk; for every non-FREE NT
// region not already represented by an owned slot, stamps a FOREIGN slot
// at 64 KB granularity.
//
// CALLER MUST HOLD MmapLock writer. (Unlike the reconcile-event entry
// points above, this helper does not acquire the lock — it is meant to be
// used inside an existing exclusive section, e.g. prepare_for_fixed.)
//
// No reverse "clear stale" pass: the typical caller is about to mutate
// the range, so cordon recall would be wasted work.
//
// Returns the number of slots newly stamped this call.
unsigned cordon_foreigners_in_range(void *base, SIZE_T size);

} // namespace memory
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_RECONCILE_H
