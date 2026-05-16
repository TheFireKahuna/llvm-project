//===- mem_fault_handler.h - Memory-subsystem VEH classifier ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Memory-subsystem VEH filters. Registered into the master dispatch
// table at VEH_PRIORITY_MEMORY / VEH_PRIORITY_MLOCK; the implementation
// TU carries the per-filter probe order and the VEH-path lock-free
// reasoning.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEM_FAULT_HANDLER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEM_FAULT_HANDLER_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/macros/config.h"
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Returns EXCEPTION_CONTINUE_EXECUTION when a synchronous commit
// resolved the fault, EXCEPTION_CONTINUE_SEARCH otherwise.
LONG try_demand_commit(EXCEPTION_POINTERS *ep);

// Catches STATUS_GUARD_PAGE_VIOLATION on pages that
// mlock2(MLOCK_ONFAULT) armed with PAGE_GUARD. Foreign-VA guard trips
// (e.g. the loader's stack-grow page) fall through to CONTINUE_SEARCH.
LONG try_mlock_onfault(EXCEPTION_POINTERS *ep);

// Drives demand-commit via the natural fault path before handing a
// buffer to a kernel call. Cheaper than NtQueryVirtualMemory per
// region: already-committed pages cost one cache-line load each.
LIBC_INLINE void prefault_read_pages(const void *addr, SIZE_T len) {
  if (!addr || len == 0)
    return;

  auto *p = static_cast<const volatile unsigned char *>(addr);
  auto *end = p + len;

  (void)*p;
  uintptr_t page_size = get_page_size();
  uintptr_t next = (reinterpret_cast<uintptr_t>(p) | (page_size - 1)) + 1;
  p = reinterpret_cast<const volatile unsigned char *>(next);
  while (p < end) {
    (void)*p;
    p += page_size;
  }
}

// EXCEPTION_RECORD::ExceptionInformation[0] access-type codes.
inline constexpr ULONG AV_READ = 0;
inline constexpr ULONG AV_WRITE = 1;
inline constexpr ULONG AV_DEP = 8; // Data Execution Prevention (execute).

// Cross-filter handoff: producer calls `set()` on a MAP_PRIVATE file-
// backed I/O error (STATUS_IN_PAGE_ERROR path); the signal filter
// consumes once to promote the in-flight SIGSEGV to SIGBUS/BUS_ADRERR
// (Linux semantics). Generation (not bool) prevents a stale flag from
// promoting an unrelated subsequent ACCESS_VIOLATION. Thread-local
// because VEH filters always run on the faulting thread.
//
// FIXME: producer side is unwired — no filter currently catches
// STATUS_IN_PAGE_ERROR. The consumer in signal/transport/veh_transport
// is live and harmless without a producer (generation == consumed).
struct PendingSigbus {
  uint32_t generation = 0;
  uint32_t consumed = 0;
  uintptr_t fault_address = 0;

  void set(uintptr_t addr) {
    ++generation;
    fault_address = addr;
  }

  // Returns true exactly once per set() call.
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
