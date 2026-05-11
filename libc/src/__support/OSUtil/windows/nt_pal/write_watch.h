//===-- nt_pal::write_watch — atomic dirty-page query+reset ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `MEM_WRITE_WATCH` enables per-page dirty tracking on `MEM_PRIVATE`
// regions at zero additional syscall cost — the flag is set on the
// commit syscall (always; see `nt_pal::commit_replace` invariant) and
// the kernel maintains a one-bit-per-page bitmap updated by the
// hardware PTE dirty mechanism.
//
// `write_watch_get_reset` is the canonical query+reset entry point —
// atomic (no race window between query and reset), the primary path
// used by quarantine sweep, fork CoW preservation, and slab telemetry.
//
// Interaction rules (from RA17-22, restated for the new namespace):
//   TRIGGERS dirty:  user writes, NtReadFile, IoRing READ (unregistered),
//                    NtLockVirtualMemory (virgin), VmPrefetch(TO_WS, virgin)
//   CLEARS dirty:    MEM_DECOMMIT, MEM_RESET, NtGetWriteWatch(RESET),
//                    NtResetWriteWatch
//   NO EFFECT:       NtProtectVirtualMemory, VmRemoveFromWS, IoRing WRITE,
//                    IoRing READ (registered buffer — MDL bypass)
//
// Not available on section views (MEM_MAPPED) — only MEM_PRIVATE.
// Incompatible with MEM_LARGE_PAGES (large-page commits go through
// `commit_replace_large` which omits MEM_WRITE_WATCH).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_WRITE_WATCH_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_WRITE_WATCH_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace nt_pal {

// Atomically query and reset dirty pages in [addr, addr+size).
//
// After this call, only pages written after the reset will appear in
// subsequent queries. Returns the number of dirty page addresses
// written to `page_addrs`. The array must hold at least
// `size / PAGE_SIZE` entries for complete results; if undersized, the
// kernel silently truncates (no overflow error). Granularity is always
// 4096 on current NT kernels.
//
// Returns 0 on failure (region not allocated with MEM_WRITE_WATCH, or
// other error).
[[nodiscard]] LIBC_INLINE size_t
write_watch_get_reset(void *addr, size_t size, void **page_addrs,
                      size_t max_entries) {
  ULONG_PTR count = max_entries;
  ULONG granularity = 0;
  NTSTATUS st = ::NtGetWriteWatch(NtCurrentProcess(), WRITE_WATCH_FLAG_RESET,
                                  addr, size, page_addrs, &count, &granularity);
  return NT_SUCCESS(st) ? static_cast<size_t>(count) : 0;
}

// Query without resetting. Use when an interleaved peek (without
// disturbing the bitmap) is needed; the canonical path is the get+reset
// variant above.
[[nodiscard]] LIBC_INLINE size_t write_watch_get(void *addr, size_t size,
                                                 void **page_addrs,
                                                 size_t max_entries) {
  ULONG_PTR count = max_entries;
  ULONG granularity = 0;
  NTSTATUS st = ::NtGetWriteWatch(NtCurrentProcess(), 0, addr, size,
                                   page_addrs, &count, &granularity);
  return NT_SUCCESS(st) ? static_cast<size_t>(count) : 0;
}

// Reset without querying. After this call, only new writes will appear
// dirty. Used after bulk pre-fault or IoRing buffer registration to
// establish a clean baseline.
LIBC_INLINE bool write_watch_reset(void *addr, size_t size) {
  return NT_SUCCESS(::NtResetWriteWatch(NtCurrentProcess(), addr, size));
}

// Cross-process variant — parent queries child's dirty bitmap after
// fork. Process must hold PROCESS_VM_READ | PROCESS_VM_OPERATION.
[[nodiscard]] LIBC_INLINE size_t
write_watch_get_remote(HANDLE process, void *addr, size_t size,
                       void **page_addrs, size_t max_entries) {
  ULONG_PTR count = max_entries;
  ULONG granularity = 0;
  NTSTATUS st = ::NtGetWriteWatch(process, 0, addr, size, page_addrs, &count,
                                   &granularity);
  return NT_SUCCESS(st) ? static_cast<size_t>(count) : 0;
}

[[nodiscard]] LIBC_INLINE size_t
write_watch_get_reset_remote(HANDLE process, void *addr, size_t size,
                             void **page_addrs, size_t max_entries) {
  ULONG_PTR count = max_entries;
  ULONG granularity = 0;
  NTSTATUS st = ::NtGetWriteWatch(process, WRITE_WATCH_FLAG_RESET, addr, size,
                                   page_addrs, &count, &granularity);
  return NT_SUCCESS(st) ? static_cast<size_t>(count) : 0;
}

} // namespace nt_pal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_WRITE_WATCH_H
