//===- mem_fault_handler.cpp - Memory-subsystem VEH filter ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// VEH-path discipline (load-bearing across both filters): the handler
// fires on the faulting thread, so any lock that thread already holds
// would deadlock against itself. Every read here goes through an atomic
// projection (pagemap entry, desc RegionShape/flags words); the writer
// side sits behind a lock the VEH path never touches.
//
// Probe order in try_demand_commit is cheapest-and-most-selective first
// so foreign / cordoned faults bail in one wait-free load and already-
// committed protection faults bail in one MBI syscall before either
// touches the tracker:
//   1. pagemap::classify        wait-free tag read; rejects every
//                               libc-internal and cordon fault.
//   2. nt_pal::query_region MBI single NtQueryVirtualMemory; MEM_RESERVE
//                               gate filters out the dominant "real
//                               permission fault" case before the
//                               tracker lookup.
//   3. va_tracker::resolve      authoritative for POSIX VA, but the
//                               heaviest of the four — runs last among
//                               the classifiers.
//   4. NT-protection compat     read-vs-PAGE_READONLY etc. before any
//                               commit syscall.
// Demand-commit dispatch (NUMA-interleave per-page or 256 KiB cluster)
// only runs once all four have passed.
//
// Pin discipline: va_tracker::resolve returns a RegionRef pinned on the
// skiplist domain's kPinSlotCur and the pin persists until the next
// va_tracker call on this thread rotates the slot — no scope-exit drop.
// The fields read off the desc here (current_shape, flags_load,
// numa_interleave_mask) all live on the desc proper, so no backing-pin
// dance is needed.
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

  // get_page_size() reads g_pcb (populated in Tier A Phase 0). Pre-Tier-A
  // AVs are guaranteed to exit through one of the early CONTINUE_SEARCH
  // branches below (Empty pagemap + non-MEM_RESERVE state) and never
  // reach this arithmetic — the memory bootstrap that publishes
  // reservations runs strictly after PCB init.
  const SIZE_T page_size = get_page_size();
  void *fault_page =
      reinterpret_cast<void *>(align_down_to_page(fault_addr));

  // POSIX-tracked mappings deliberately leave the pagemap entry Empty
  // so va_tracker is the single source of truth for them; any non-Empty
  // tag is libc-internal or a cordon and not ours to handle.
  auto cls = alloc::pagemap::classify(reinterpret_cast<const void *>(fault_addr));
  if (cls.tag != alloc::VaChunkConsumer::Empty)
    return EXCEPTION_CONTINUE_SEARCH;

  MEMORY_BASIC_INFORMATION mbi;
  if (LIBC_UNLIKELY(!nt_pal::query_region(fault_page, mbi)))
    return EXCEPTION_CONTINUE_SEARCH;

  // Most ACCESS_VIOLATIONs land on already-committed pages — a genuine
  // permission fault, not a demand-commit request.
  if (LIBC_LIKELY(mbi.State != MEM_RESERVE))
    return EXCEPTION_CONTINUE_SEARCH;

  // resolve() pins on the per-thread Crystalline-W slot; the desc body
  // stays alive for subsequent field reads even if the writer reclaims.
  auto ref_or =
      va_tracker::resolve(reinterpret_cast<void *>(fault_addr));
  if (!ref_or.has_value())
    return EXCEPTION_CONTINUE_SEARCH;

  const va_tracker::RegionRef ref = ref_or.value();
  if (ref.desc == nullptr)
    return EXCEPTION_CONTINUE_SEARCH;

  const va_tracker::RegionShape shape = ref.desc->current_shape();
  const uint16_t flags = ref.desc->flags_load();
  const bool is_demand_commit_shape =
      (shape == va_tracker::RegionShape::ANON_PLACEHOLDER &&
       (flags & va_tracker::region_flag::NORESERVE) != 0) ||
      shape == va_tracker::RegionShape::FILE_VIEW_RESERVE ||
      shape == va_tracker::RegionShape::ANON_RESERVE_SECTION;
  if (!is_demand_commit_shape)
    return EXCEPTION_CONTINUE_SEARCH;

  // Write to PAGE_READONLY / execute on non-X is a genuine protection
  // violation; let signal delivery have it.
  DWORD prot = mbi.AllocationProtect;
  bool incompatible =
      (access_type == AV_WRITE &&
       (prot == PAGE_READONLY || prot == PAGE_EXECUTE_READ)) ||
      (access_type == AV_DEP &&
       (prot == PAGE_READWRITE || prot == PAGE_READONLY ||
        prot == PAGE_WRITECOPY));
  if (incompatible)
    return EXCEPTION_CONTINUE_SEARCH;

  const bool is_interleave =
      (flags & va_tracker::region_flag::NUMA_INTERLEAVE) != 0;

  if (is_interleave) {
    uint32_t mask = ref.desc->numa_interleave_mask;
    if (mask) {
      // Deterministic per-page rotation matching Linux do_numa_page:
      //   node = nodes[page_index % popcount(mask)]
      uintptr_t view_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
      SIZE_T page_index = (fault_addr - view_base) / page_size;
      ULONG node_count = static_cast<ULONG>(__builtin_popcount(mask));
      ULONG slot_idx = static_cast<ULONG>(page_index % node_count);

      // Walk to the n-th set bit by clearing the lowest slot_idx bits.
      uint32_t m = mask;
      for (ULONG i = 0; i < slot_idx; ++i)
        m &= m - 1;
      ULONG node = static_cast<ULONG>(__builtin_ctz(m));

      NTSTATUS numa_st =
          nt_pal::commit_in_reservation_numa(fault_page, page_size, prot,
                                             node);
      if (NT_SUCCESS(numa_st)) {
        // MLOCK_ONFAULT post-commit lock — `flags` was loaded once above
        // for the demand-commit shape gate, so no extra resolve and no
        // global lookup table on the hot path.
        if (flags & va_tracker::region_flag::LOCK_ONFAULT)
          (void)nt_pal::lock_range(fault_page, page_size);
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }
    // Fall through to the non-NUMA commit path on failure.
  }

  // 256 KiB cluster around the fault amortises VEH dispatch over 64
  // pages; MEM_COMMIT on already-committed pages in the same view is a
  // no-op, so over-cluster is harmless.
  constexpr SIZE_T DEMAND_COMMIT_CLUSTER = 256 * 1024;
  uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  uintptr_t region_end = region_start + mbi.RegionSize;
  uintptr_t cluster_start = fault_addr & ~(DEMAND_COMMIT_CLUSTER - 1);
  uintptr_t cluster_end = cluster_start + DEMAND_COMMIT_CLUSTER;
  if (cluster_start < region_start)
    cluster_start = region_start;
  if (cluster_end > region_end)
    cluster_end = region_end;

  // File MAP_PRIVATE arrives via PAGE_WRITECOPY section views where the
  // kernel handles CoW commit itself, so the only shapes that reach
  // this call are anonymous MAP_NORESERVE and SEC_RESERVE.
  void *base = reinterpret_cast<void *>(cluster_start);
  SIZE_T commit_size = static_cast<SIZE_T>(cluster_end - cluster_start);
  NTSTATUS st = nt_pal::commit_in_reservation_no_writewatch(base, commit_size, prot);
  if (NT_SUCCESS(st)) {
    if (flags & va_tracker::region_flag::LOCK_ONFAULT)
      (void)nt_pal::lock_range(fault_page, page_size);
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

//===----------------------------------------------------------------------===//
// Guard-page filter — second half of MLOCK_ONFAULT
//===----------------------------------------------------------------------===//
//
// mlock2(MLOCK_ONFAULT) on a fully-touched mapping has no commit fault
// to ride, so the entry path ORs PAGE_GUARD into each committed chunk's
// protection. The kernel then raises STATUS_GUARD_PAGE_VIOLATION on
// first touch (one-shot — PAGE_GUARD self-clears). The region_flag bit
// is the source of truth for "should we react"; PAGE_GUARD is just the
// mechanism that delivers us the fault.

LONG try_mlock_onfault(EXCEPTION_POINTERS *ep) {
  // Defence-in-depth: the registration mask is exactly VEH_GUARD_PAGE so
  // master dispatch can only route GUARD_PAGE faults here, but a future
  // mask change or direct test invocation should still no-op cleanly
  // rather than reach into ExceptionInformation[1] for the wrong code.
  if (ep->ExceptionRecord->ExceptionCode !=
      static_cast<DWORD>(STATUS_GUARD_PAGE_VIOLATION))
    return EXCEPTION_CONTINUE_SEARCH;

  uintptr_t fault_addr = reinterpret_cast<uintptr_t>(
      ep->ExceptionRecord->ExceptionInformation[1]);

  // No MBI fast-reject here: STATUS_GUARD_PAGE_VIOLATION only fires on
  // committed pages, so an MEM_RESERVE check would always be false.
  // Foreign / image / libc-internal PAGE_GUARD trips (loader stack-grow,
  // debugger, ...) are not ours to handle.
  auto ref_or =
      va_tracker::resolve(reinterpret_cast<void *>(fault_addr));
  if (!ref_or.has_value())
    return EXCEPTION_CONTINUE_SEARCH;
  va_tracker::RegionDesc *desc = ref_or.value().desc;
  if (desc == nullptr)
    return EXCEPTION_CONTINUE_SEARCH;

  if (!(desc->flags_load() & va_tracker::region_flag::LOCK_ONFAULT))
    return EXCEPTION_CONTINUE_SEARCH;

  // CPU already cleared the guard bit; the page is accessible.
  const SIZE_T page_size = get_page_size();
  uintptr_t page_base = fault_addr & ~(page_size - 1);
  (void)nt_pal::lock_range(reinterpret_cast<void *>(page_base), page_size);

  return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

//===----------------------------------------------------------------------===//
// VEH filter registrations
//===----------------------------------------------------------------------===//
//
// Master VEH validates `ep` and the reentry guard before dispatch; the
// trampolines below do nothing but forward. The split into two records
// (rather than one handler keyed on ExceptionCode) is what lets master
// dispatch route by `exception_mask & bit` and skip whichever filter
// the current fault can't reach.
//
// The two priorities serve different ordering invariants:
//   * VEH_PRIORITY_MEMORY (10) must beat VEH_PRIORITY_SIGNAL (20)
//     because ACCESS_VIOLATION sits in both `VEH_ACCESS_VIOLATION`
//     here and the signal filter's `VEH_ALL_SIGNAL` mask — without
//     memory winning, every demand-commit fault would surface as
//     SIGSEGV.
//   * VEH_PRIORITY_MLOCK (15) has no signal contender — GUARD_PAGE is
//     deliberately absent from `VEH_ALL_SIGNAL` (see veh_core.h) — so
//     the value is just a stable slot below SIGNAL for future
//     additions.

static LONG mem_fault_filter(EXCEPTION_POINTERS *ep) {
  return LIBC_NAMESPACE::windows::try_demand_commit(ep);
}

LIBC_REGISTER_VEH_FILTER(mem_fault,
                         ::LIBC_NAMESPACE::windows::VEH_ACCESS_VIOLATION,
                         &mem_fault_filter,
                         ::LIBC_NAMESPACE::windows::VEH_PRIORITY_MEMORY)

static LONG mlock_onfault_filter(EXCEPTION_POINTERS *ep) {
  return LIBC_NAMESPACE::windows::try_mlock_onfault(ep);
}

LIBC_REGISTER_VEH_FILTER(mlock_onfault,
                         ::LIBC_NAMESPACE::windows::VEH_GUARD_PAGE,
                         &mlock_onfault_filter,
                         ::LIBC_NAMESPACE::windows::VEH_PRIORITY_MLOCK)
