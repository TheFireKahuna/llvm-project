//===- mem_fault_handler.cpp - Memory-subsystem VEH filter ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the memory-subsystem VEH classifier. The filter record is
// emitted into the `.libcveh` section via `LIBC_REGISTER_VEH_FILTER`;
// the master VEH handler picks it up from the registry and calls into
// `try_demand_commit` for every `EXCEPTION_ACCESS_VIOLATION`.
//
// VEH path discipline. State read on the VEH path must be lock-free or
// wait-free. The handler fires on the faulting thread, so any lock the
// thread might already hold deadlocks against itself - including the
// mmap-engine write side. The pattern across the subsystem is an
// atomic projection (the pagemap entry, the desc's `RegionShape` /
// `flags` words) readable from any context, with the full mutable
// table sitting behind a separate writer-only lock that the VEH path
// never touches.
//
// Probe priority:
//
//   1. `pagemap::classify`           wait-free, non-faulting; rejects
//                                    every libc-internal and cordon
//                                    fault to `CONTINUE_SEARCH`.
//   2. `nt_pal::query_region` MBI    cheap fast-reject - most ACCESS
//                                    violations land on committed
//                                    pages with `State != MEM_RESERVE`.
//   3. `va_tracker::resolve`         authoritative for POSIX VA;
//                                    returns a per-pointer Crystalline-W
//                                    pinned `RegionRef`.
//   4. NT protection compatibility   reads `AllocationProtect` and
//                                    rejects mismatched access classes.
//   5. Demand-commit dispatch        NUMA-interleave per-page, else a
//                                    256 KiB cluster commit.
//
// Pin discipline. `va_tracker::resolve` returns a `RegionRef` pinned on
// the skiplist domain's `kPinSlotCur`. The pin persists until the next
// va_tracker call on this thread rotates the slot - no scope-exit drop
// step. Reading kernel-state fields off the resolved desc requires an
// additional pin on the backing domain plus a triple-validate via
// `deref_backing_raw(desc->backing_ref)`. The current implementation only
// reads `current_shape()`, `flags_load()`, and `numa_interleave_mask`,
// all of which live on the desc proper.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/mem_fault_handler.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/pagemap_classifier.h"
#include "src/__support/OSUtil/windows/memory/desc_backing.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/nt_pal/lock.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/veh/veh_core.h"
#include "src/__support/OSUtil/windows/veh/veh_filter_registry.h"
#include "src/__support/OSUtil/windows/veh/veh_state.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

//===----------------------------------------------------------------------===//
// Demand-commit for MAP_NORESERVE / SEC_RESERVE regions
//===----------------------------------------------------------------------===//

LONG try_demand_commit(EXCEPTION_POINTERS *ep) {
  uintptr_t fault_addr = reinterpret_cast<uintptr_t>(
      ep->ExceptionRecord->ExceptionInformation[1]);
  ULONG access_type = static_cast<ULONG>(
      ep->ExceptionRecord->ExceptionInformation[0]);

  // Cached PCB read populated by pcb_startup_init (Tier A Phase 0). Demand
  // commits are guarded by `mbi.State == MEM_RESERVE`, which cannot hold
  // for libc-owned VA before the memory primitives bootstrap publishes
  // reservations — and that bootstrap runs strictly after the PCB is
  // populated. A pre-bootstrap AV reaches this handler only via the
  // pagemap-Empty + non-MEM_RESERVE early exits, never the page-size
  // arithmetic below.
  const SIZE_T page_size = get_page_size();
  void *fault_page =
      reinterpret_cast<void *>(align_down_to_page(fault_addr));

  // First-line dispatch reads only the atomic pagemap projection - any
  // non-`Empty` tag is libc-internal or a cordon, both of which the
  // memory filter declines on. POSIX-tracked mappings deliberately
  // leave the pagemap entry `Empty` so the tracker is the single
  // source of truth for them.
  auto cls = alloc::pagemap::classify(reinterpret_cast<const void *>(fault_addr));
  if (cls.tag != alloc::VaChunkConsumer::Empty)
    return EXCEPTION_CONTINUE_SEARCH;

  MEMORY_BASIC_INFORMATION mbi;
  if (LIBC_UNLIKELY(!nt_pal::query_region(fault_page, mbi)))
    return EXCEPTION_CONTINUE_SEARCH;

  // Fast reject: most ACCESS_VIOLATIONs land on already-committed
  // pages, where the NT-level fault is a genuine permission violation
  // rather than a demand-commit request.
  if (LIBC_LIKELY(mbi.State != MEM_RESERVE))
    return EXCEPTION_CONTINUE_SEARCH;

  // Authoritative ownership check. `resolve` pins the desc on this
  // thread's per-pointer Crystalline-W slot; subsequent field reads
  // remain safe even if the writer side reclaims the desc body.
  auto ref_or =
      va_tracker::resolve(reinterpret_cast<void *>(fault_addr));
  if (!ref_or.has_value())
    return EXCEPTION_CONTINUE_SEARCH;

  const va_tracker::RegionRef ref = ref_or.value();
  if (ref.desc == nullptr)
    return EXCEPTION_CONTINUE_SEARCH;

  // Both shape and flags are atomic projections - reading them on the
  // VEH path takes no lock.
  const va_tracker::RegionShape shape = ref.desc->current_shape();
  const uint16_t flags = ref.desc->flags_load();
  const bool is_demand_commit_shape =
      (shape == va_tracker::RegionShape::ANON_PLACEHOLDER &&
       (flags & va_tracker::region_flag::NORESERVE) != 0) ||
      shape == va_tracker::RegionShape::FILE_VIEW_RESERVE ||
      shape == va_tracker::RegionShape::ANON_RESERVE_SECTION;
  if (!is_demand_commit_shape)
    return EXCEPTION_CONTINUE_SEARCH;

  // Access-vs-protection compatibility. A write to a `PAGE_READONLY`
  // section view or an execute on a non-executable mapping is a
  // genuine protection violation; fall through to signal delivery.
  DWORD prot = mbi.AllocationProtect;
  bool incompatible =
      (access_type == AV_WRITE &&
       (prot == PAGE_READONLY || prot == PAGE_EXECUTE_READ)) ||
      (access_type == AV_DEP &&
       (prot == PAGE_READWRITE || prot == PAGE_READONLY ||
        prot == PAGE_WRITECOPY));
  if (incompatible)
    return EXCEPTION_CONTINUE_SEARCH;

  // NUMA-interleave path. The interleave mask is read from the desc
  // under the same pin established by `resolve`.
  const bool is_interleave =
      (flags & va_tracker::region_flag::NUMA_INTERLEAVE) != 0;

  if (is_interleave) {
    uint32_t mask = ref.desc->numa_interleave_mask;
    if (mask) {
      // Deterministic per-page rotation:
      //   node = nodes[page_index % popcount(mask)]
      // matching Linux `do_numa_page`'s per-page interleave. One
      // syscall per fault.
      uintptr_t view_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
      SIZE_T page_index = (fault_addr - view_base) / page_size;
      ULONG node_count = static_cast<ULONG>(__builtin_popcount(mask));
      ULONG slot_idx = static_cast<ULONG>(page_index % node_count);

      // Walk to the n-th set bit by clearing the lowest set bit
      // `slot_idx` times.
      uint32_t m = mask;
      for (ULONG i = 0; i < slot_idx; ++i)
        m &= m - 1;
      ULONG node = static_cast<ULONG>(__builtin_ctz(m));

      NTSTATUS numa_st =
          nt_pal::commit_in_reservation_numa(fault_page, page_size, prot,
                                             node);
      if (NT_SUCCESS(numa_st)) {
        // MLOCK_ONFAULT trip on the freshly-committed page. The bit is
        // a property of the desc this fault already resolved against,
        // so the check is one mask off `flags` — already loaded above
        // for the demand-commit shape gate. No global table, no
        // reader lock, no extra resolve.
        if (flags & va_tracker::region_flag::LOCK_ONFAULT)
          (void)nt_pal::lock_range(fault_page, page_size);
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }
    // Fall through to the non-NUMA commit path on failure.
  }

  // Cluster the commit to 256 KiB around the fault, clamped to the
  // contiguous uncommitted region. Amortises VEH dispatch over 64
  // pages; `MEM_COMMIT` on already-committed pages in the same view is
  // a no-op.
  constexpr SIZE_T DEMAND_COMMIT_CLUSTER = 256 * 1024;
  uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  uintptr_t region_end = region_start + mbi.RegionSize;
  uintptr_t cluster_start = fault_addr & ~(DEMAND_COMMIT_CLUSTER - 1);
  uintptr_t cluster_end = cluster_start + DEMAND_COMMIT_CLUSTER;
  if (cluster_start < region_start)
    cluster_start = region_start;
  if (cluster_end > region_end)
    cluster_end = region_end;

  // File MAP_PRIVATE is served by section views with `PAGE_WRITECOPY`
  // and the kernel commits CoW pages itself, so the only paths that
  // reach this commit call are anonymous `MAP_NORESERVE` and
  // `SEC_RESERVE` section views.
  void *base = reinterpret_cast<void *>(cluster_start);
  SIZE_T commit_size = static_cast<SIZE_T>(cluster_end - cluster_start);
  NTSTATUS st = nt_pal::commit_in_reservation_no_writewatch(base, commit_size, prot);
  if (NT_SUCCESS(st)) {
    // MLOCK_ONFAULT trip on the freshly-committed page. Per-desc flag
    // already loaded above; one mask, no global table.
    if (flags & va_tracker::region_flag::LOCK_ONFAULT)
      (void)nt_pal::lock_range(fault_page, page_size);
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

//===----------------------------------------------------------------------===//
// Guard-page filter — the second half of MLOCK_ONFAULT
//===----------------------------------------------------------------------===//
//
// Demand-commit faults are EXCEPTION_ACCESS_VIOLATION. MLOCK_ONFAULT
// also has to fire on already-committed pages — `mlock2(MLOCK_ONFAULT)`
// can be called on a fully-touched mapping, in which case there is no
// commit fault to ride. The `mlock2` entry walks the range and ORs
// `PAGE_GUARD` into each committed chunk's protection; the kernel then
// raises `STATUS_GUARD_PAGE_VIOLATION` on first touch (and clears
// PAGE_GUARD as a one-shot). This filter catches that violation,
// resolves the desc, and locks if the bit is still set. The flag is
// the source of truth for "should we react"; PAGE_GUARD is just the
// mechanism that gets us a fault.
//
// Lives here rather than in a separate TU so that the two halves of
// MLOCK_ONFAULT (commit-time and guard-page) sit next to each other
// and share the same desc-resolution pattern.

LONG try_mlock_onfault(EXCEPTION_POINTERS *ep) {
  if (ep->ExceptionRecord->ExceptionCode !=
      static_cast<DWORD>(STATUS_GUARD_PAGE_VIOLATION))
    return EXCEPTION_CONTINUE_SEARCH;

  uintptr_t fault_addr = reinterpret_cast<uintptr_t>(
      ep->ExceptionRecord->ExceptionInformation[1]);

  // Resolve against the va_tracker. A foreign / image / libc-internal
  // PAGE_GUARD trip is not ours to handle; the master VEH chain falls
  // through to whoever owns those VAs (the loader's stack-grow handler,
  // a debugger, etc.).
  auto ref_or =
      va_tracker::resolve(reinterpret_cast<void *>(fault_addr));
  if (!ref_or.has_value())
    return EXCEPTION_CONTINUE_SEARCH;
  va_tracker::RegionDesc *desc = ref_or.value().desc;
  if (desc == nullptr)
    return EXCEPTION_CONTINUE_SEARCH;

  if (!(desc->flags_load() & va_tracker::region_flag::LOCK_ONFAULT))
    return EXCEPTION_CONTINUE_SEARCH;

  // Guard bit already cleared by the CPU; the page is accessible.
  // Lock it into the working set.
  const SIZE_T page_size = get_page_size();
  uintptr_t page_base = fault_addr & ~(page_size - 1);
  (void)nt_pal::lock_range(reinterpret_cast<void *>(page_base), page_size);

  return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

//===----------------------------------------------------------------------===//
// VEH filter callback - registered into the master dispatch table
//===----------------------------------------------------------------------===//

static LONG mem_fault_filter(EXCEPTION_POINTERS *ep) {
  // The master VEH handler already validated `ep` and checked the
  // reentry guard; no further checks needed here.
  return LIBC_NAMESPACE::windows::try_demand_commit(ep);
}

// Static filter record picked up by `register_all_static_veh_filters()`
// during VEH bring-up. The `VEH_PRIORITY_MEMORY` slot fires the memory
// filter before signal-delivery filters get a look at the exception.
LIBC_REGISTER_VEH_FILTER(mem_fault,
                         ::LIBC_NAMESPACE::windows::VEH_ACCESS_VIOLATION,
                         &mem_fault_filter,
                         ::LIBC_NAMESPACE::windows::VEH_PRIORITY_MEMORY)

//===----------------------------------------------------------------------===//
// MLOCK_ONFAULT guard-page filter
//===----------------------------------------------------------------------===//

static LONG mlock_onfault_filter(EXCEPTION_POINTERS *ep) {
  return LIBC_NAMESPACE::windows::try_mlock_onfault(ep);
}

LIBC_REGISTER_VEH_FILTER(mlock_onfault,
                         ::LIBC_NAMESPACE::windows::VEH_GUARD_PAGE,
                         &mlock_onfault_filter,
                         ::LIBC_NAMESPACE::windows::VEH_PRIORITY_MLOCK)
