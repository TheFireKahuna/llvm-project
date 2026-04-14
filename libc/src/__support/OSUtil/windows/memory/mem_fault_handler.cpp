//===-- Memory fault VEH filter implementation ---------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the memory infrastructure VEH filter: remap guard stalling and
// MAP_NORESERVE demand-commit. Registered into the unified VEH dispatch table
// (veh/veh_core.h) at VEH_PRIORITY_MEMORY during mem_fault_startup_init() (Phase 5).
//
// Demand-commit is self-identifying via MBI state:
//   MEM_RESERVE + MEM_MAPPED: SEC_RESERVE section view (file-backed noreserve)
//   MEM_RESERVE + MEM_PRIVATE + AllocationProtect != PAGE_NOACCESS:
//     reserve-replaced placeholder (anonymous MAP_NORESERVE)
// No mapping table dependency on the fault path.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/mem_fault_handler.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/memory_lock_policy.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/veh/veh_core.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// ---------------------------------------------------------------------------
// Per-thread cached event for synchronous NtReadFile in VEH demand-read path.
// Avoids NtCreateEvent+NtClose on every file-backed page fault.
// ---------------------------------------------------------------------------
static DWORD g_read_event_tls_index = internal::TLS_OUT_OF_INDEXES;

static void NTAPI read_event_cleanup(void *val) {
  if (val)
    ::NtClose(static_cast<HANDLE>(val));
}

static HANDLE get_demand_read_event() {
  if (g_read_event_tls_index == internal::TLS_OUT_OF_INDEXES)
    return nullptr;
  auto *evt = static_cast<HANDLE>(
      internal::teb_tls_get(g_read_event_tls_index));
  if (!evt) {
    auto oa = internal_oa();
    ::NtCreateEvent(&evt, EVENT_MODIFY_STATE | SYNCHRONIZE, &oa,
                    SynchronizationEvent,
                    FALSE);
    if (evt)
      internal::teb_tls_set(g_read_event_tls_index, evt);
  }
  return evt;
}

// ---------------------------------------------------------------------------
// Responsibility 1: Remap guard
// ---------------------------------------------------------------------------
//
// During MAP_FIXED, mremap, or mbind NUMA migration, mapping table entries
// transition to REMAPPING state. If the fault address falls in a REMAPPING
// entry's guarded range, stall until the remap completes. Dead-owner
// recovery prevents permanent hangs if the remapping thread crashes.
LONG try_remap_guard(EXCEPTION_POINTERS *ep) {
  // Fast path: no active remaps in progress.
  if (g_mapping_table.active_remap_count() == 0)
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
// Two region types require demand-commit:
//
//   MEM_RESERVE + MEM_MAPPED: SEC_RESERVE section view (legacy path, also
//     used by file-backed MAP_NORESERVE). Self-identifying --- no other mmap
//     path produces this combination.
//
//   MEM_RESERVE + MEM_PRIVATE + AllocationProtect != PAGE_NOACCESS:
//     Reserve-replaced placeholder (new path for anonymous MAP_NORESERVE).
//     AllocationProtect distinguishes from bare PROT_NONE placeholders
//     (which have PAGE_NOACCESS and must NOT be committed).
//
// AllocationProtect is set at allocation time and not changed by mprotect
// (mprotect on uncommitted pages commits them first in mprotect.cpp).
LONG try_demand_commit(EXCEPTION_POINTERS *ep) {
  uintptr_t fault_addr = reinterpret_cast<uintptr_t>(
      ep->ExceptionRecord->ExceptionInformation[1]);
  ULONG access_type = static_cast<ULONG>(
      ep->ExceptionRecord->ExceptionInformation[0]);

  void *fault_page = reinterpret_cast<void *>(
      fault_addr & ~static_cast<uintptr_t>(4095));

  // Query the faulting page's virtual memory state. This is the single
  // source of truth --- no mapping table lookup, no flag checks, no races.
  MEMORY_BASIC_INFORMATION mbi;
  SIZE_T ret_len;
  NTSTATUS st = ::NtQueryVirtualMemory(NtCurrentProcess(), fault_page,
                                       MemoryBasicInformation, &mbi,
                                       sizeof(mbi), &ret_len);
  if (!NT_SUCCESS(st))
    return EXCEPTION_CONTINUE_SEARCH;

  if (mbi.State != MEM_RESERVE)
    return EXCEPTION_CONTINUE_SEARCH;

  // MEM_MAPPED: SEC_RESERVE section view (file or pagefile).
  // MEM_PRIVATE + accessible AllocationProtect: reserve-replaced noreserve.
  // MEM_PRIVATE + PAGE_NOACCESS: bare placeholder (PROT_NONE) --- don't touch.
  if (mbi.Type != MEM_MAPPED &&
      !(mbi.Type == MEM_PRIVATE && mbi.AllocationProtect != PAGE_NOACCESS))
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

  // Check if this view has NUMA interleave --- commit per-page with rotation.
  // AllocationBase is the view_base (key into the mapping table).
  windows::SlotSnapshot snap = {};
  bool have_snap =
      windows::g_mapping_table.snapshot(mbi.AllocationBase, &snap);
  bool is_interleave =
      have_snap && (snap.flags & windows::VM_FLAG_NUMA_INTERLEAVE);

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
  uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  uintptr_t region_end = region_start + mbi.RegionSize;
  uintptr_t cluster_start =
      fault_addr & ~(FILE_PRIVATE_DEMAND_READ_CLUSTER - 1);
  uintptr_t cluster_end = cluster_start + FILE_PRIVATE_DEMAND_READ_CLUSTER;
  if (cluster_start < region_start)
    cluster_start = region_start;
  if (cluster_end > region_end)
    cluster_end = region_end;

  // File MAP_PRIVATE demand-read: check mapping table before commit.
  // MEM_PRIVATE regions may be backed by a file (VM_FLAG_FILE_PRIVATE).
  // If so, commit as READWRITE for NtReadFile, then narrow to target prot.
  // Anonymous NORESERVE has no table entry --- commit with AllocationProtect.
  bool is_file_private = false;
  windows::MappingEntry file_entry = {};
  if (mbi.Type == MEM_PRIVATE) {
    windows::SlotSnapshot priv_snap = {};
    if (windows::g_mapping_table.snapshot(mbi.AllocationBase, &priv_snap) &&
        (priv_snap.flags & windows::VM_FLAG_FILE_PRIVATE) &&
        priv_snap.file_handle) {
      is_file_private = true;
      file_entry = priv_snap.to_entry();
    }
  }

  // Widen commit prot for NtReadFile if needed.
  DWORD commit_prot = prot;
  if (is_file_private && prot != PAGE_READWRITE &&
      prot != PAGE_EXECUTE_READWRITE)
    commit_prot = PAGE_READWRITE;

  PVOID base = reinterpret_cast<void *>(cluster_start);
  SIZE_T commit_size = static_cast<SIZE_T>(cluster_end - cluster_start);
  st = ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &base, &commit_size,
                                   MEM_COMMIT, commit_prot, nullptr, 0);
  if (NT_SUCCESS(st)) {
    if (is_file_private) {
      // Read file content into committed pages.
      uintptr_t alloc_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
      windows::ViewSpec read_spec =
          file_entry.spec.at_offset(cluster_start - alloc_base);
      // The file handle is overlapped (no FILE_SYNCHRONOUS_IO_*), so
      // NtReadFile may return STATUS_PENDING. Use a per-thread cached
      // event to wait synchronously without creating/closing an event
      // on every fault. Falls back to a local event if TLS is unavailable.
      HANDLE read_event = get_demand_read_event();
      bool local_event = false;
      if (!read_event) {
        auto fallback_oa = internal_oa();
        ::NtCreateEvent(&read_event, EVENT_MODIFY_STATE | SYNCHRONIZE,
                        &fallback_oa, SynchronizationEvent, FALSE);
        local_event = true;
      }
      // If even local event creation failed, we cannot safely issue async
      // I/O — STATUS_PENDING with no event leaves the IOSB dangling on
      // the stack. Decommit and signal SIGBUS rather than risk corruption.
      if (!read_event) {
        PVOID dc = base;
        SIZE_T ds = commit_size;
        ::NtFreeVirtualMemory(NtCurrentProcess(), &dc, &ds, MEM_DECOMMIT);
        g_pending_sigbus.set(fault_addr);
        return EXCEPTION_CONTINUE_SEARCH;
      }
      IO_STATUS_BLOCK iosb = {};
      NTSTATUS read_st = ::NtReadFile(file_entry.spec.file, read_event,
                                       nullptr, nullptr, &iosb, base,
                                       static_cast<ULONG>(commit_size),
                                       &read_spec.offset, nullptr);
      if (read_st == STATUS_PENDING) {
        ::NtWaitForSingleObject(read_event, FALSE, nullptr);
        read_st = iosb.Status;
      }
      if (local_event && read_event)
        ::NtClose(read_event);
      // STATUS_END_OF_FILE is expected past file end — zero-fill is correct.
      // Other I/O errors: decommit and signal SIGBUS (matching Linux behavior).
      if (NT_ERROR(read_st) && read_st != STATUS_END_OF_FILE) {
        // I/O error. Decommit pages we just committed (restore MEM_RESERVE).
        PVOID decommit_base = base;
        SIZE_T decommit_size = commit_size;
        ::NtFreeVirtualMemory(NtCurrentProcess(), &decommit_base,
                              &decommit_size, MEM_DECOMMIT);
        // Set per-thread generation stamp for signal VEH filter to deliver SIGBUS.
        g_pending_sigbus.set(fault_addr);
        return EXCEPTION_CONTINUE_SEARCH;
      }

      // Narrow protection if we widened for NtReadFile.
      if (commit_prot != prot) {
        PVOID pp = base;
        SIZE_T pps = commit_size;
        ULONG old_prot;
        ::NtProtectVirtualMemory(NtCurrentProcess(), &pp, &pps, prot,
                                 &old_prot);
      }

      // Reset write-watch for these pages. NtReadFile dirtied the write-watch
      // bitmap when it wrote file data into the committed pages. Resetting
      // establishes a clean baseline so only genuine user writes appear dirty
      // — perfect CoW tracking for fork(), msync, and split/remap (RA22.3).
      windows::write_watch_reset(base, commit_size);
    }

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

static LONG NTAPI mem_fault_filter(EXCEPTION_POINTERS *ep) {
  // No reentry guard needed --- the master handler already checked it.
  // No null checks --- the master handler already validated ep.
  LONG result = LIBC_NAMESPACE::windows::try_remap_guard(ep);
  if (result != EXCEPTION_CONTINUE_SEARCH)
    return result;
  return LIBC_NAMESPACE::windows::try_demand_commit(ep);
}

// ---------------------------------------------------------------------------
// SEH safety net --- frame-level handler for thread entry and do_start
// ---------------------------------------------------------------------------
//
// Defense-in-depth for demand-commit and remap guard if VEH is displaced.
// Registered via .seh_handler on thread entry and main() wrapper frames.
// Uses the standard EXCEPTION_ROUTINE signature so RtlDispatchException
// calls it identically to __gxx_personality_seh0.
//
// Only handles ACCESS_VIOLATION during the dispatch phase (not unwind).
extern "C" __declspec(dllexport) LONG NTAPI __llvm_libc_thread_fault_handler(
    EXCEPTION_RECORD *record, void * /*frame*/, CONTEXT *context,
    DISPATCHER_CONTEXT * /*dispatch*/) {
  if (record->ExceptionFlags & EXCEPTION_UNWINDING)
    return ExceptionContinueSearch;

  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return ExceptionContinueSearch;

  EXCEPTION_POINTERS ep = {record, context};

  LONG result = LIBC_NAMESPACE::windows::try_remap_guard(&ep);
  if (result == EXCEPTION_CONTINUE_EXECUTION)
    return ExceptionContinueExecution;

  result = LIBC_NAMESPACE::windows::try_demand_commit(&ep);
  if (result == EXCEPTION_CONTINUE_EXECUTION)
    return ExceptionContinueExecution;

  return ExceptionContinueSearch;
}

// ---------------------------------------------------------------------------
// Subsystem init --- register memory fault filter into dispatch table
// ---------------------------------------------------------------------------
int LIBC_NAMESPACE::internal::mem_fault_startup_init() {
  // Allocate a TEB TLS slot for per-thread demand-read event caching.
  LIBC_NAMESPACE::windows::g_read_event_tls_index =
      LIBC_NAMESPACE::internal::tls_alloc();
  if (LIBC_NAMESPACE::windows::g_read_event_tls_index !=
      LIBC_NAMESPACE::internal::TLS_OUT_OF_INDEXES)
    LIBC_NAMESPACE::internal::tls_cleanup_register(
        LIBC_NAMESPACE::windows::g_read_event_tls_index,
        LIBC_NAMESPACE::windows::read_event_cleanup);

  LIBC_NAMESPACE::windows::VehFilter filter;
  filter.exception_mask = LIBC_NAMESPACE::windows::VEH_ACCESS_VIOLATION;
  filter.handler = mem_fault_filter;
  filter.priority = LIBC_NAMESPACE::windows::VEH_PRIORITY_MEMORY;
  LIBC_NAMESPACE::windows::register_veh_filter(filter);
  return 0;
}
