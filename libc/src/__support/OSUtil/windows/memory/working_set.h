//===-- Working set helpers for mlock/mlockall -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Working set quota expansion and region lockability checks shared by
// mlock, munlock, mlockall, and munlockall.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_WORKING_SET_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_WORKING_SET_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"

#include <stdint.h> // SIZE_MAX

namespace LIBC_NAMESPACE_DECL {
namespace windows {

/// Query working set sizes and flags via NtQueryInformationProcess.
LIBC_INLINE bool query_working_set(HANDLE process, SIZE_T &min_ws,
                                   SIZE_T &max_ws, ULONG &flags) {
  QUOTA_LIMITS_EX quota = {};
  NTSTATUS st = ::NtQueryInformationProcess(process, ProcessQuotaLimits,
                                            &quota, sizeof(quota), nullptr);
  if (NT_ERROR(st))
    return false;
  min_ws = quota.MinimumWorkingSetSize;
  max_ws = quota.MaximumWorkingSetSize;
  flags = quota.Flags;
  return true;
}

/// Set working set sizes via NtSetInformationProcess, acquiring
/// SeIncreaseWorkingSet + SeIncBasePriority privileges (best-effort).
LIBC_INLINE bool set_working_set(HANDLE process, SIZE_T min_ws, SIZE_T max_ws,
                                 ULONG flags) {
  ULONG privs[] = {SE_INCREASE_WORKING_SET_PRIVILEGE,
                   SE_INC_BASE_PRIORITY_PRIVILEGE};
  PVOID priv_state = nullptr;
  NTSTATUS priv_st =
      ::RtlAcquirePrivilege(privs, 2, 0, &priv_state);

  QUOTA_LIMITS_EX quota = {};
  quota.MinimumWorkingSetSize = min_ws;
  quota.MaximumWorkingSetSize = max_ws;
  quota.Flags = flags;
  NTSTATUS st = ::NtSetInformationProcess(process, ProcessQuotaLimits,
                                          &quota, sizeof(quota));

  if (NT_SUCCESS(priv_st))
    ::RtlReleasePrivilege(priv_state);

  return NT_SUCCESS(st);
}

/// Expand working set quota to accommodate locking \p additional bytes.
///
/// Strategy: increase minimum working set by (additional + 25% headroom),
/// capped at 256MB headroom to avoid OS rejection on very large requests.
/// The 25% avoids repeated expansion when locking regions incrementally.
///
/// QUOTA_LIMITS_HARDWS_MIN_ENABLE prevents the OS from trimming below the
/// minimum, which is required for mlock's residency guarantee. If the hard
/// limit is denied (insufficient privilege), we fall back to a soft limit
/// that the OS may trim under extreme pressure.
LIBC_INLINE bool expand_working_set(HANDLE process, SIZE_T additional) {
  SIZE_T min_ws, max_ws;
  ULONG flags;

  if (!query_working_set(process, min_ws, max_ws, flags))
    return false;

  constexpr SIZE_T MAX_HEADROOM = 256ULL * 1024 * 1024;
  SIZE_T headroom = additional / 4;
  if (headroom > MAX_HEADROOM)
    headroom = MAX_HEADROOM;

  // Saturating arithmetic for new_min = min_ws + additional + headroom.
  SIZE_T new_min = min_ws;
  if (additional <= SIZE_MAX - new_min) {
    new_min += additional;
    if (headroom <= SIZE_MAX - new_min)
      new_min += headroom;
  } else {
    new_min = SIZE_MAX;
  }

  // max must be >= min; add headroom if we raised it.
  SIZE_T new_max = max_ws;
  if (new_max < new_min) {
    new_max = new_min;
    if (headroom <= SIZE_MAX - new_max)
      new_max += headroom;
  }

  // Hard minimum first; soft fallback if privilege is insufficient.
  if (set_working_set(process, new_min, new_max, QUOTA_LIMITS_HARDWS_MIN_ENABLE))
    return true;
  return set_working_set(process, new_min, new_max, 0);
}

/// Check if a memory region can be locked.
///
/// Windows cannot lock PAGE_NOACCESS or PAGE_GUARD pages — unlike Linux
/// where mlock succeeds on PROT_NONE (the kernel locks physical frames
/// regardless of access protection). Code that does mmap(PROT_NONE) +
/// mlock() must reorder to mprotect() first on Windows.
LIBC_INLINE bool is_lockable(const MEMORY_BASIC_INFORMATION &mbi) {
  if (mbi.State != MEM_COMMIT)
    return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
    return false;
  return true;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_WORKING_SET_H
