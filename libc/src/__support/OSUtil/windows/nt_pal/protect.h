//===-- nt_pal::protect — page protection + offer/reclaim --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//   * `protect`  — `NtProtectVirtualMemory`. Change page protection on a
//                  committed range.
//   * `offer`    — mark a committed range as discardable. The kernel
//                  may reclaim physical memory under pressure; the next
//                  access either sees the original content (if the OS
//                  hasn't repurposed the pages) or zero (if it has).
//                  Pages stay MEM_COMMIT — no recommit needed on access.
//                  Used by `MADV_FREE` (POSIX façade) and Layer 6
//                  reclamation on cold ranges.
//   * `reclaim`  — undo a prior `offer`. Returns true if all pages still
//                  contain their original content; false if the OS
//                  reclaimed any page (now zero-filled). Caller must
//                  re-init on the false branch.
//
// **NT-equivalent of `OfferVirtualMemory` / `ReclaimVirtualMemory`.**
// Per `windows-itanium-reference/MMAP_OPTIMIZATION_RESEARCH.md` Test
// 5.6, `MEM_RESET` + `MEM_RESET_UNDO` provide the same observable
// semantics as the Win32 offer/reclaim pair without going through
// kernel32 — staying inside the design's "pure `Nt*` syscalls in the
// memory PAL" invariant. The `priority` parameter is implemented via
// `NtSetInformationVirtualMemory(VmPagePriorityInformation)` which
// influences kernel eviction order under memory pressure (per RA7,
// LOW and NORMAL both work on Build 26200; values > NORMAL are
// kernel-mode only and rejected).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_PROTECT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_PROTECT_H

#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/nt_pal/query.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace nt_pal {

// Offer-priority levels. Mirrors the Win32 `OFFER_PRIORITY` enum, with
// values mapped to the `MEMORY_PRIORITY_*` constants accepted by
// `VmPagePriorityInformation`.
enum class OfferPriority : unsigned int {
  VeryLow = MEMORY_PRIORITY_VERY_LOW,         // = 1
  Low = MEMORY_PRIORITY_LOW,                  // = 2
  BelowNormal = MEMORY_PRIORITY_BELOW_NORMAL, // = 4
  Normal = MEMORY_PRIORITY_NORMAL,            // = 5
};

// Change page protection on [addr, addr+size). On success, *old_prot
// receives the previous protection (may be nullptr if the caller does
// not need it).
//
// On multi-page ranges with mixed prior protections, NT only returns
// the first page's prior protection in `*old_prot` — the standard
// `NtProtectVirtualMemory` semantic. Callers needing per-page snapshot
// must walk via `query.h::RegionWalker`.
//
// CFG-secured / `MmSecureVirtualMemory`-locked ranges cache the live
// protection in the kernel; an `NtProtect` against a stale cached
// entry returns `STATUS_INVALID_PAGE_PROTECTION`. `RtlFlushSecureMemoryCache`
// invalidates the cache and the retry surfaces the real result. The
// retry only fires on that NTSTATUS and only with `NtCurrentProcess`
// (cross-process protect is not a supported configuration of the
// secure-memory cache).
LIBC_INLINE bool protect(void *addr, size_t size, ULONG new_prot,
                         ULONG *old_prot = nullptr) {
  ULONG previous = 0;
  ULONG *out = old_prot ? old_prot : &previous;
  PVOID base = addr;
  SIZE_T sz = size;
  NTSTATUS st =
      ::NtProtectVirtualMemory(NtCurrentProcess(), &base, &sz, new_prot, out);
  if (st == STATUS_INVALID_PAGE_PROTECTION) {
    if (::RtlFlushSecureMemoryCache(addr, size)) {
      base = addr;
      sz = size;
      st = ::NtProtectVirtualMemory(NtCurrentProcess(), &base, &sz, new_prot,
                                     out);
    }
  }
  return NT_SUCCESS(st);
}

// Apply a page-priority hint to a range. No-op on failure (priority is
// advisory — the kernel may ignore or clamp on older builds).
//
// `priority` is one of the `MEMORY_PRIORITY_*` constants accepted by
// `VmPagePriorityInformation` (NORMAL=5, BELOW_NORMAL=4, LOW=2,
// VERY_LOW=1). Values > NORMAL are kernel-mode only and rejected. Used
// directly by `madvise(MADV_COLD)` / `MADV_PAGEOUT` and as a building
// block for `offer` / `reclaim`.
LIBC_INLINE void set_page_priority(void *addr, size_t size, ULONG priority) {
  MEMORY_RANGE_ENTRY range{addr, size};
  MEMORY_PAGE_PRIORITY_INFORMATION info{priority};
  (void)::NtSetInformationVirtualMemory(NtCurrentProcess(),
                                         VmPagePriorityInformation,
                                         /* NumberOfEntries= */ 1, &range,
                                         &info, sizeof(info));
}

// Mark a committed range as discardable. The kernel may reclaim
// physical memory; the next access returns either the original content
// or zero. VA stays MEM_COMMIT — no recommit on access.
//
// Two syscalls: `VmPagePriorityInformation` (priority hint) +
// `MEM_RESET`. The priority hint is best-effort — older kernels may
// ignore it, in which case the kernel uses NORMAL.
//
// Returns true on success. The MEM_RESET path itself does not return a
// "did content survive" indicator (the kernel decides asynchronously);
// `reclaim` is what surfaces that status.
LIBC_INLINE bool offer(void *addr, size_t size,
                       OfferPriority priority = OfferPriority::Normal) {
  set_page_priority(addr, size, static_cast<ULONG>(priority));
  PVOID base = addr;
  SIZE_T sz = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &base, &sz,
                                             MEM_RESET, PAGE_READWRITE,
                                             nullptr, 0);
  return NT_SUCCESS(st);
}

// Attempt to undo a prior `offer`. Returns true if every page in the
// range still holds its original content; false if the kernel
// reclaimed any page (the page is now zero-filled and the caller must
// treat it as fresh memory). Safe on non-offered pages — the kernel
// checks per-PTE state and treats non-reset pages as a no-op.
//
// Composes `MEM_RESET_UNDO` with a priority restore to NORMAL.
LIBC_INLINE bool reclaim(void *addr, size_t size) {
  PVOID base = addr;
  SIZE_T sz = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &base, &sz,
                                             MEM_RESET_UNDO, PAGE_READWRITE,
                                             nullptr, 0);
  // Best-effort priority restore — the page may now be NORMAL anyway
  // if the kernel reclaimed and re-faulted, but explicit is cheap.
  set_page_priority(addr, size, MEMORY_PRIORITY_NORMAL);
  return NT_SUCCESS(st);
}

// Arm a one-shot guard trap on every committed page in the range.
//
// ORs `PAGE_GUARD` into each chunk's existing protection via a
// kernel-VAD walk + per-chunk `NtProtectVirtualMemory`. The kernel
// raises `STATUS_GUARD_PAGE_VIOLATION` on the first access to any
// armed page and clears `PAGE_GUARD` as part of dispatching the
// exception — so the trap fires exactly once per page per arm, and a
// consumer must re-arm if it wants further trips.
//
// Per-chunk-protection-aware: the existing `Protect` value is read
// from MBI and ORed (not replaced) so PROT_READ / PROT_WRITE / PROT_EXEC
// stay correct after the trap clears. Chunks that already carry
// `PAGE_GUARD` or `PAGE_NOACCESS` are skipped (re-arming a guarded
// page is a no-op; arming a no-access page would prevent the kernel
// from delivering the trap as a guard violation).
//
// Best-effort per chunk: a chunk whose `NtProtect` call fails simply
// will not trap. Used by `mlock2(MLOCK_ONFAULT)` to arm fault-on-touch
// across already-committed pages — pairs with the
// `try_mlock_onfault` filter in `mem_fault_handler.cpp` which reads
// `region_flag::MLOCK_ONFAULT` on the resolved desc to decide what
// to do with the trap.
LIBC_INLINE void arm_guard_trap(void *addr, size_t size) {
  RegionWalker walk(addr, static_cast<SIZE_T>(size));
  if (!walk)
    return;
  while (walk.next()) {
    if (walk.entry->State != MEM_COMMIT)
      continue;
    if (walk.entry->Protect & (PAGE_NOACCESS | PAGE_GUARD))
      continue;
    PVOID base = walk.chunk;
    SIZE_T sz = walk.chunk_size;
    ULONG old_prot = 0;
    (void)::NtProtectVirtualMemory(NtCurrentProcess(), &base, &sz,
                                    walk.entry->Protect | PAGE_GUARD,
                                    &old_prot);
  }
}

// Evict pages from the process working set (commit + VA preserved;
// only the WS-residency hint is dropped). Wraps
// `NtSetInformationVirtualMemory(VmRemoveFromWorkingSetInformation)`.
//
// Distinct from `offer` (`MEM_RESET`): `offer` marks the pages as
// discardable so the kernel may reclaim physical memory under
// pressure, with no guarantee on the next read. `evict_working_set`
// only removes the pages from the working set — the kernel pages
// them out (write-back to pagefile if dirty), but the next access
// faults the original content back in. Used by `MADV_PAGEOUT` /
// `MADV_FREE`'s "soft eviction" mode and by cold-section eviction
// where preserving content matters.
//
// `ranges` must point to an array of `MEMORY_RANGE_ENTRY` (each
// describing a `[VirtualAddress, NumberOfBytes)` interval). `flags`
// is reserved (pass 0).
//
// Returns true on success.
LIBC_INLINE bool evict_working_set_ranges(MEMORY_RANGE_ENTRY *ranges,
                                           ULONG count, ULONG flags = 0) {
  NTSTATUS st = ::NtSetInformationVirtualMemory(
      NtCurrentProcess(), VmRemoveFromWorkingSetInformation, count, ranges,
      &flags, sizeof(flags));
  return NT_SUCCESS(st);
}

} // namespace nt_pal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_PROTECT_H
