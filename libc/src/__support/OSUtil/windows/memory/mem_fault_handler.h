//===- mem_fault_handler.h - Memory-subsystem VEH classifier ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Memory-subsystem VEH filter for `EXCEPTION_ACCESS_VIOLATION` faults.
///
/// Registered into the master VEH dispatch table via the `.libcveh`
/// section registry at `VEH_PRIORITY_MEMORY`. The master handler
/// invokes this filter only for access violations - the
/// `exception_mask` on the registration record narrows dispatch.
///
/// The filter classifies a fault VA against the libc's two address-space
/// indexes (pagemap and `va_tracker`) and dispatches to the appropriate
/// handler, or returns `EXCEPTION_CONTINUE_SEARCH` so the master can
/// fall through to the next filter and ultimately to signal delivery.
///
/// Probe order:
///
///   1. `pagemap::classify` - cordons (Kernel / Image / Foreign) and
///       libc-internal tags fast-reject; only `Empty` chunks proceed.
///   2. `va_tracker::resolve` - authoritative for POSIX VA.
///   3. NT-protection compatibility check (read vs PAGE_READONLY,
///       execute vs PAGE_READWRITE, ...).
///   4. Demand-commit dispatch (NUMA-interleave or plain cluster).
///
/// State read on the VEH path MUST be lock-free. VEH fires on the
/// faulting thread, so any lock the thread might hold would deadlock
/// against itself; the pattern is atomic projections (e.g. the
/// pagemap entry, the `RegionShape`/`flags` atomic on the desc)
/// readable wait-free, with the full mutable table behind a separate
/// lock that the VEH path never acquires.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEM_FAULT_HANDLER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEM_FAULT_HANDLER_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/macros/config.h"
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {

/// Classifies an `EXCEPTION_ACCESS_VIOLATION` and, when the fault names
/// an uncommitted page belonging to a demand-commit-shaped mapping,
/// commits the page in place.
///
/// \returns `EXCEPTION_CONTINUE_EXECUTION` when the fault was resolved
///          by a synchronous commit; `EXCEPTION_CONTINUE_SEARCH`
///          otherwise (cordon hit, tracker miss, incompatible
///          protection, or commit failure).
LONG try_demand_commit(EXCEPTION_POINTERS *ep);

/// Catches `STATUS_GUARD_PAGE_VIOLATION` on pages that
/// `mlock2(MLOCK_ONFAULT)` armed. Resolves the desc, checks the
/// `region_flag::MLOCK_ONFAULT` bit, and locks the faulting page on
/// hit. Returns `EXCEPTION_CONTINUE_EXECUTION` on hit, `CONTINUE_SEARCH`
/// otherwise (foreign-VA guard-page trips fall through to the next
/// filter — typically the loader's stack-grow handler).
LONG try_mlock_onfault(EXCEPTION_POINTERS *ep);

/// Pre-faults source pages by touching one byte per page.
///
/// Used by I/O paths to drive lazy commits via the natural fault path
/// before a kernel call inspects the buffer. Already-committed pages
/// (stack, heap - the common case) cost a single cache-line load
/// (~1 cycle each); uncommitted pages take the VEH demand-commit
/// trip. Zero syscalls, in contrast with the
/// `NtQueryVirtualMemory`-per-region alternative.
LIBC_INLINE void prefault_read_pages(const void *addr, SIZE_T len) {
  if (!addr || len == 0)
    return;

  auto *p = static_cast<const volatile unsigned char *>(addr);
  auto *end = p + len;

  // Touch the first byte, then stride one byte per page.
  (void)*p;
  uintptr_t page_size = get_page_size();
  uintptr_t next = (reinterpret_cast<uintptr_t>(p) | (page_size - 1)) + 1;
  p = reinterpret_cast<const volatile unsigned char *>(next);
  while (p < end) {
    (void)*p;
    p += page_size;
  }
}

/// `EXCEPTION_RECORD::ExceptionInformation[0]` access-type codes,
/// documented in phnt-style headers as the access-violation sub-type.
inline constexpr ULONG AV_READ = 0;
inline constexpr ULONG AV_WRITE = 1;
inline constexpr ULONG AV_DEP = 8; ///< Data Execution Prevention (execute).

/// Per-thread demand-read I/O error flag, consumed by the signal filter
/// to promote `SIGSEGV` to `SIGBUS` after a MAP_PRIVATE file-backed
/// page-fault hits an I/O error.
///
/// The memory filter sets a generation stamp and returns
/// `EXCEPTION_CONTINUE_SEARCH`; the signal filter observes the stamp
/// and replaces the in-flight `SIGSEGV` with `BUS_ADRERR`, matching
/// Linux semantics for irrecoverable storage failures behind a
/// mapping.
///
/// The generation counter (not a bool flag) guards against stale state
/// from a prior fault promoting an unrelated `ACCESS_VIOLATION`; the
/// signal filter only promotes when its consumed generation snapshot
/// matches the current generation.
///
/// Thread-local - VEH filters run on the faulting thread, no
/// synchronisation needed.
struct PendingSigbus {
  uint32_t generation = 0;     ///< Bumped on each I/O-error fault.
  uint32_t consumed = 0;       ///< Last generation consumed by signals.
  uintptr_t fault_address = 0; ///< Faulting VA, captured at set time.

  /// Bumps the generation and records the faulting address. Called by
  /// the memory fault filter on a demand-read I/O error.
  void set(uintptr_t addr) {
    ++generation;
    fault_address = addr;
  }

  /// \returns `true` exactly once per matching `set()` call.
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
