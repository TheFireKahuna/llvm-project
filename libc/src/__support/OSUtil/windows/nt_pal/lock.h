//===-- nt_pal::lock — page lock / unlock --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Layer 0 PAL — page locking. The libc surface for `mlock` / `munlock`
// / `mlockall` / `munlockall` / kernel-side post-fault page-locking
// policies. Wraps `NtLockVirtualMemory` and `NtUnlockVirtualMemory`.
//
// MapType selects which working set the lock applies to:
//
//   * `MAP_PROCESS` (default) — lock pages into the process working
//     set. Forbids kernel-driven eviction; caller's `mlock` budget
//     consumed. POSIX `mlock` semantic.
//   * `MAP_SYSTEM`  — lock pages into the system working set
//     (resident-but-pageable across the system). Requires
//     `SeLockMemoryPrivilege`; kernel-only callers (most NT libc paths
//     do NOT hold this token bit). Documented for completeness; not
//     used by any current libc consumer.
//
// `NtLockVirtualMemory` is the kernel surface. The two map flags pass
// straight through.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_LOCK_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_LOCK_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace nt_pal {

// Lock the range [addr, addr+size) into the working set selected by
// `map`. Returns the raw NTSTATUS so callers can distinguish quota
// exhaustion (`STATUS_WORKING_SET_QUOTA`) from access-denied
// (`STATUS_ACCESS_DENIED` for non-committed pages or
// `STATUS_PRIVILEGE_NOT_HELD` for `MAP_SYSTEM` without the token).
//
// The kernel rounds the range to page boundaries internally; callers
// that care should pre-align.
[[nodiscard]] LIBC_INLINE NTSTATUS lock_range(void *addr, size_t size,
                                                ULONG map = MAP_PROCESS) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtLockVirtualMemory(NtCurrentProcess(), &base, &sz, map);
}

// Unlock the range previously locked via `lock_range`. Same `map`
// must be passed (the kernel tracks per-(VA, working-set) lock
// counts; a `MAP_PROCESS`-locked page can't be released through
// `MAP_SYSTEM`). Returns NTSTATUS verbatim.
[[nodiscard]] LIBC_INLINE NTSTATUS unlock_range(void *addr, size_t size,
                                                  ULONG map = MAP_PROCESS) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtUnlockVirtualMemory(NtCurrentProcess(), &base, &sz, map);
}

} // namespace nt_pal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_LOCK_H
