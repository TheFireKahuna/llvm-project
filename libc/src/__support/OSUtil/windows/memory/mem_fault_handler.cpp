//===-- Memory fault VEH filter implementation ---------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the memory infrastructure VEH filter: remap guard stalling and
// MAP_NORESERVE demand-commit. Declaratively registered into the unified VEH
// dispatch table via `.libcveh` at VEH_PRIORITY_MEMORY; the Tier B Phase 3
// sweep (register_all_static_veh_filters(), after mmap_subsystem_init()) makes
// the filter live before any pool or DLL-load-driven file-backed fault can
// arrive.
//
// Demand-commit is self-identifying via MBI state:
//   MEM_RESERVE + MEM_MAPPED: SEC_RESERVE section view (file-backed noreserve)
//   MEM_RESERVE + MEM_PRIVATE + AllocationProtect != PAGE_NOACCESS:
//     reserve-replaced placeholder (anonymous MAP_NORESERVE)
// No mapping table dependency on the fault path.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/mem_fault_handler.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/memory_lock_policy.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/veh/veh_core.h"
#include "src/__support/OSUtil/windows/veh/veh_filter_registry.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// ---------------------------------------------------------------------------
// Responsibility 1: Remap guard
// ---------------------------------------------------------------------------
//
// During MAP_FIXED, mremap, or mbind NUMA migration, mapping table entries
// transition to REMAPPING state. If the fault address falls in a REMAPPING
// entry's guarded range, stall until the remap completes. Dead-owner
// recovery prevents permanent hangs if the remapping thread crashes.
LONG try_remap_guard(EXCEPTION_POINTERS *ep) {
  // Fast path: no active remaps in progress (>99.99% of dispatches).
  if (LIBC_LIKELY(g_mapping_table.active_remap_count() == 0))
    return EXCEPTION_CONTINUE_SEARCH;

  uintptr_t fault_addr = reinterpret_cast<uintptr_t>(
      ep->ExceptionRecord->ExceptionInformation[1]);

  int slot = g_mapping_table.check_remap_guard(fault_addr);
  if (slot < 0)
    return EXCEPTION_CONTINUE_SEARCH;

  g_mapping_table.wait_for_remap(slot);
  return EXCEPTION_CONTINUE_EXECUTION;
}

// ---------------------------------------------------------------------------
// Responsibility 2: Demand-commit for MAP_NORESERVE regions
// ---------------------------------------------------------------------------
//
// The mapping table is the source of truth. The slot at the faulting view's
// AllocationBase tells us whether the VA is ours and which shape's
// demand-commit recipe applies:
//
//   ANON_PLACEHOLDER + region_flag::NORESERVE   → reserve-replaced anon
//   FILE_VIEW_RESERVE / ANON_RESERVE_SECTION    → SEC_RESERVE section view
//   anything else (LIVE non-NORESERVE, FOREIGN, miss) → propagate as
//                                                       SIGSEGV
//
// The MBI probe survives only as a confirmation pass: NT exposes the
// AllocationProtect we need for the access-vs-protection compatibility
// check, and the State==MEM_RESERVE filter is the cheap fast-reject for
// the ~99% of access violations that hit already-committed pages. The
// table consult that follows is what gates the actual NtAllocateVM commit.
LONG try_demand_commit(EXCEPTION_POINTERS *ep) {
  uintptr_t fault_addr = reinterpret_cast<uintptr_t>(
      ep->ExceptionRecord->ExceptionInformation[1]);
  ULONG access_type = static_cast<ULONG>(
      ep->ExceptionRecord->ExceptionInformation[0]);

  void *fault_page = reinterpret_cast<void *>(
      fault_addr & ~static_cast<uintptr_t>(4095));

  MEMORY_BASIC_INFORMATION mbi;
  SIZE_T ret_len;
  NTSTATUS st = ::NtQueryVirtualMemory(NtCurrentProcess(), fault_page,
                                       MemoryBasicInformation, &mbi,
                                       sizeof(mbi), &ret_len);
  if (LIBC_UNLIKELY(!NT_SUCCESS(st)))
    return EXCEPTION_CONTINUE_SEARCH;

  // Fast reject: most ACCESS_VIOLATIONs are on committed pages.
  if (LIBC_LIKELY(mbi.State != MEM_RESERVE))
    return EXCEPTION_CONTINUE_SEARCH;

  // Authoritative ownership check: the slot at AllocationBase must be
  // LIVE in a shape whose recipe is "VEH commits on first touch". A
  // miss, FOREIGN stamp, or wrong shape means the faulted VA is not
  // ours to commit — propagate so SIGSEGV reaches the program.
  windows::SlotSnapshot snap = {};
  if (!windows::g_mapping_table.snapshot(mbi.AllocationBase, &snap))
    return EXCEPTION_CONTINUE_SEARCH;
  if (snap.region == nullptr)
    return EXCEPTION_CONTINUE_SEARCH;

  const auto shape = snap.region->current_shape();
  const bool is_demand_commit_shape =
      (shape == windows::memory::RegionShape::ANON_PLACEHOLDER &&
       snap.region->has_flag(windows::memory::region_flag::NORESERVE)) ||
      shape == windows::memory::RegionShape::FILE_VIEW_RESERVE ||
      shape == windows::memory::RegionShape::ANON_RESERVE_SECTION;
  if (!is_demand_commit_shape)
    return EXCEPTION_CONTINUE_SEARCH;

  // Validate access against the section's original protection. A write to
  // a PAGE_READONLY section view is a genuine protection violation --- let
  // it fall through to signal delivery as SIGSEGV.
  DWORD prot = mbi.AllocationProtect;
  bool incompatible =
      (access_type == AV_WRITE &&
       (prot == PAGE_READONLY || prot == PAGE_EXECUTE_READ)) ||
      (access_type == AV_DEP &&
       (prot == PAGE_READWRITE || prot == PAGE_READONLY ||
        prot == PAGE_WRITECOPY));
  if (incompatible)
    return EXCEPTION_CONTINUE_SEARCH;

  // NUMA interleave is a slot-flag attribute; the snap we already hold
  // surfaces it without a second seqlock pass.
  bool is_interleave = (snap.flags & windows::VM_FLAG_NUMA_INTERLEAVE) != 0;

  if (is_interleave) {
    // Per-page commit with rotating NUMA node. Deterministic rotation:
    // node = nodes[(page_index) % node_count], matching Linux per-page
    // NUMA interleave. One syscall per fault (same as Linux do_numa_page).
    DWORD64 mask = windows::g_mapping_table.get_numa_interleave_mask(
        mbi.AllocationBase);
    if (mask) {
      // Derive the node from the page's position in the view.
      uintptr_t view_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
      SIZE_T page_index = (fault_addr - view_base) / 4096;
      ULONG node_count = static_cast<ULONG>(__builtin_popcountll(mask));
      ULONG slot_idx = static_cast<ULONG>(page_index % node_count);

      // Walk the mask to the nth set bit.
      DWORD64 m = mask;
      for (ULONG i = 0; i < slot_idx; ++i)
        m &= m - 1; // Clear lowest set bit.
      ULONG node = static_cast<ULONG>(__builtin_ctzll(m));

      MEM_EXTENDED_PARAMETER param = {};
      param.Type = MemExtendedParameterNumaNode;
      param.ULong = node;

      PVOID page = fault_page;
      SIZE_T page_size = 4096;
      st = ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &page, &page_size,
                                        MEM_COMMIT, prot, &param, 1);
      if (NT_SUCCESS(st)) {
        if (onfault_contains(fault_addr))
          lock_range(fault_page, 4096);
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }
    // Fall through to non-NUMA commit on failure.
  }

  // Commit a 256KB cluster around the fault, clamped to the contiguous
  // uncommitted region. Amortizes VEH dispatch: 1 fault per 64 pages.
  // MEM_COMMIT on already-committed pages within the same view is a no-op.
  constexpr SIZE_T DEMAND_COMMIT_CLUSTER = 256 * 1024;
  uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  uintptr_t region_end = region_start + mbi.RegionSize;
  uintptr_t cluster_start = fault_addr & ~(DEMAND_COMMIT_CLUSTER - 1);
  uintptr_t cluster_end = cluster_start + DEMAND_COMMIT_CLUSTER;
  if (cluster_start < region_start)
    cluster_start = region_start;
  if (cluster_end > region_end)
    cluster_end = region_end;

  // File MAP_PRIVATE is now handled by section views with PAGE_WRITECOPY —
  // the kernel services the fault atomically, so this VEH path is only
  // reached for anonymous MAP_NORESERVE (private reserve) or SEC_RESERVE
  // section views (MEM_MAPPED).
  PVOID base = reinterpret_cast<void *>(cluster_start);
  SIZE_T commit_size = static_cast<SIZE_T>(cluster_end - cluster_start);
  st = ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &base, &commit_size,
                                   MEM_COMMIT, prot, nullptr, 0);
  if (NT_SUCCESS(st)) {
    // MLOCK_ONFAULT: if this address is in a lock-on-fault range, lock
    // the faulting page immediately.
    if (onfault_contains(fault_addr))
      lock_range(fault_page, 4096);
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

// ---------------------------------------------------------------------------
// VEH filter callback --- registered into the master dispatch table
// ---------------------------------------------------------------------------

static LONG mem_fault_filter(EXCEPTION_POINTERS *ep) {
  // No reentry guard needed --- the master handler already checked it.
  // No null checks --- the master handler already validated ep.
  LONG result = LIBC_NAMESPACE::windows::try_remap_guard(ep);
  if (result != EXCEPTION_CONTINUE_SEARCH)
    return result;
  return LIBC_NAMESPACE::windows::try_demand_commit(ep);
}

// ---------------------------------------------------------------------------
// Static VEH filter record --- picked up by register_all_static_veh_filters()
// ---------------------------------------------------------------------------
LIBC_REGISTER_VEH_FILTER(mem_fault,
                         ::LIBC_NAMESPACE::windows::VEH_ACCESS_VIOLATION,
                         &mem_fault_filter,
                         ::LIBC_NAMESPACE::windows::VEH_PRIORITY_MEMORY)
