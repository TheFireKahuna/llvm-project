//===-- Memory fault VEH filter (remap guard + demand-commit) ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// VEH filter for memory infrastructure faults. Registered into the unified
// VEH dispatch table (veh/veh_core.h) at VEH_PRIORITY_MEMORY.
//
// Two responsibilities:
//
//   1. Remap guard: stalls threads that fault during mmap/munmap/mremap VA
//      mutations. Mirrors Linux mmap_lock page fault serialization.
//
//   2. Demand-commit: commits pages on first access for SEC_RESERVE section
//      views (MAP_NORESERVE). Self-identifying via NtQueryVirtualMemory ---
//      MEM_RESERVE + MEM_MAPPED uniquely identifies uncommitted section pages.
//      No mapping table dependency on the fault path.
//
// This filter only processes EXCEPTION_ACCESS_VIOLATION. The exception_mask
// in the VehFilter registration ensures the master handler only calls this
// filter for access violations.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEM_FAULT_HANDLER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEM_FAULT_HANDLER_H

#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {

/// File-private demand-read faults commit/read pages in 256KB clusters.
/// Shared with write-side prefaulting so kernel-probed source buffers
/// materialize the same granularity the VEH path would.
inline constexpr SIZE_T FILE_PRIVATE_DEMAND_READ_CLUSTER = 256 * 1024;

/// Try to handle an ACCESS_VIOLATION as a memory infrastructure fault.
/// Returns EXCEPTION_CONTINUE_EXECUTION if handled (remap guard wait
/// or demand-commit), EXCEPTION_CONTINUE_SEARCH otherwise. Also called
/// by the SEH safety net (__llvm_libc_thread_fault_handler).
LONG try_remap_guard(EXCEPTION_POINTERS *ep);
LONG try_demand_commit(EXCEPTION_POINTERS *ep);

/// Materialize lazy MAP_PRIVATE file-backed pages in [addr, addr + len).
/// This is used before kernel write I/O probes user buffers, since the
/// kernel cannot trigger our user-mode VEH demand-read path itself.
inline void materialize_file_private_range(const void *addr, SIZE_T len) {
  if (!addr || len == 0)
    return;

  uintptr_t current = reinterpret_cast<uintptr_t>(addr);
  uintptr_t end = current + len;
  if (end < current)
    end = static_cast<uintptr_t>(-1);

  while (current < end) {
    MEMORY_BASIC_INFORMATION mbi;
    NTSTATUS st =
        windows::query_region_status(reinterpret_cast<const void *>(current),
                                     mbi);
    if (!NT_SUCCESS(st) || mbi.RegionSize == 0)
      return;

    uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) +
                           static_cast<uintptr_t>(mbi.RegionSize);
    if (region_end <= current)
      return;
    if (region_end > end)
      region_end = end;

    if (mbi.State == MEM_RESERVE && mbi.Type == MEM_PRIVATE) {
      windows::SlotSnapshot snap = {};
      if (windows::g_mapping_table.snapshot(mbi.AllocationBase, &snap) &&
          (snap.flags & windows::VM_FLAG_FILE_PRIVATE) && snap.file_handle) {
        (void)*reinterpret_cast<volatile const unsigned char *>(current);
        uintptr_t next =
            (current & ~(FILE_PRIVATE_DEMAND_READ_CLUSTER - 1)) +
            FILE_PRIVATE_DEMAND_READ_CLUSTER;
        while (next < region_end) {
          (void)*reinterpret_cast<volatile const unsigned char *>(next);
          next += FILE_PRIVATE_DEMAND_READ_CLUSTER;
        }
      }
    }

    current = region_end;
  }
}

/// Pre-fault source buffer pages before kernel I/O (lightweight path).
///
/// Touches every page in [addr, addr+len) via volatile read, triggering
/// VEH naturally for any uncommitted pages (file MAP_PRIVATE demand-read,
/// MAP_NORESERVE demand-commit, etc.).  For committed memory (stack,
/// heap — 99.9% of writes) each read is a cache-line hit (~1 cycle).
/// Zero syscalls, replacing NtQueryVirtualMemory (~8,000 cycles/region).
LIBC_INLINE void prefault_read_pages(const void *addr, SIZE_T len) {
  if (!addr || len == 0)
    return;

  auto *p = static_cast<const volatile unsigned char *>(addr);
  auto *end = p + len;

  // Touch first byte.
  (void)*p;

  // Advance to next page boundary, then stride by page size.
  uintptr_t page_size = get_page_size();
  uintptr_t next = (reinterpret_cast<uintptr_t>(p) | (page_size - 1)) + 1;
  p = reinterpret_cast<const volatile unsigned char *>(next);
  while (p < end) {
    (void)*p;
    p += page_size;
  }
}

/// Access violation sub-types from ExceptionInformation[0].
inline constexpr ULONG AV_READ = 0;
inline constexpr ULONG AV_WRITE = 1;
inline constexpr ULONG AV_DEP = 8; // Data Execution Prevention (execute)

/// Per-thread demand-read I/O error state for SIGBUS delivery.
///
/// When a MAP_PRIVATE file-backed page fault hits an I/O error, the memory
/// filter sets a generation stamp here and returns CONTINUE_SEARCH. The
/// signal filter consumes the stamp to promote SIGSEGV → SIGBUS, matching
/// Linux BUS_ADRERR semantics.
///
/// A monotonic generation counter (not a bool flag) prevents stale state
/// from a prior fault from falsely promoting an unrelated ACCESS_VIOLATION.
/// The signal filter only promotes when its snapshot of the generation
/// matches the current value.
///
/// Thread-local, no synchronization needed — VEH filters run on the
/// faulting thread.
struct PendingSigbus {
  uint32_t generation = 0;     // Bumped by memory filter on I/O error.
  uint32_t consumed = 0;       // Last generation consumed by signal filter.
  uintptr_t fault_address = 0;

  /// Called by the memory fault filter on demand-read I/O error.
  void set(uintptr_t addr) {
    ++generation;
    fault_address = addr;
  }

  /// Called by the signal filter. Returns true exactly once per set().
  bool consume() {
    if (generation != consumed) {
      consumed = generation;
      return true;
    }
    return false;
  }
};
inline thread_local PendingSigbus g_pending_sigbus;

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEM_FAULT_HANDLER_H
