//===-- nt_pal::working_set — process working-set quota ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Layer 0 PAL — process-wide working-set quota state. Wraps
// `NtQueryInformationProcess(ProcessQuotaLimits)` /
// `NtSetInformationProcess(ProcessQuotaLimits)` plus a small policy
// helper for the lock-on-reservation pattern, and the per-MBI
// lockability predicate that callers of `nt_pal::lock_range` use to
// pre-skip `PAGE_NOACCESS` / `PAGE_GUARD` chunks.
//
// Public surface:
//
//   * `query_working_set`  — read min / max / flags from the process
//                            quota record.
//   * `set_working_set`    — write min / max / flags. Acquires the
//                            relevant token privileges (best-effort)
//                            so callers do not have to chase the
//                            `RtlAdjustPrivilege` ceremony.
//   * `expand_working_set` — query + set with mlock-shaped policy
//                            (25 % headroom, 256 MiB cap, hard-min
//                            preferred with soft fallback). Used by
//                            every `mlock` / `mlockall` / VEH lock-on-
//                            fault path before touching `lock_range`.
//   * `is_lockable`        — predicate over an `MBI` for whether
//                            `nt_pal::lock_range` can accept the
//                            chunk. Bundled here because Windows lock
//                            acceptance is tightly coupled with the
//                            working-set quota model.
//
// Why working-set state is PAL: it is process-wide kernel-resource
// state below POSIX, identical in shape to the other Layer 0 process
// queries (PCB cookie, large-pages availability). The mlock-shaped
// policy in `expand_working_set` is a thin wrapper, not an opinionated
// allocator strategy — every NT lock primitive needs the same headroom
// or it bounces on `STATUS_WORKING_SET_QUOTA`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_WORKING_SET_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_WORKING_SET_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stdint.h> // SIZE_MAX

namespace LIBC_NAMESPACE_DECL {
namespace nt_pal {

/// Read the process working-set quota record. On success `*min_ws`,
/// `*max_ws`, `*flags` receive the current values (any may be null
/// if the caller does not need that field). Returns true on success.
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

/// Write the process working-set quota record. Acquires
/// `SE_INCREASE_WORKING_SET_PRIVILEGE` and `SE_INC_BASE_PRIORITY_PRIVILEGE`
/// for the syscall window and releases them before returning. The
/// privilege probe is best-effort — a process running without the
/// privileges still gets the syscall attempt; the kernel rejects it
/// with `STATUS_PRIVILEGE_NOT_HELD` and the helper returns false.
LIBC_INLINE bool set_working_set(HANDLE process, SIZE_T min_ws,
                                 SIZE_T max_ws, ULONG flags) {
  ULONG privs[] = {SE_INCREASE_WORKING_SET_PRIVILEGE,
                   SE_INC_BASE_PRIORITY_PRIVILEGE};
  PVOID priv_state = nullptr;
  NTSTATUS priv_st = ::RtlAcquirePrivilege(privs, 2, 0, &priv_state);

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

/// Bump the working-set minimum to make room for `additional` bytes
/// of locked pages. The 25 % headroom amortises repeat calls when
/// callers lock many small ranges in succession; the 256 MiB cap
/// keeps the kernel from rejecting outright on very large requests.
///
/// `QUOTA_LIMITS_HARDWS_MIN_ENABLE` is preferred so the OS cannot
/// trim below the minimum (mlock's residency guarantee depends on
/// it). On a token without `SE_INCREASE_WORKING_SET_PRIVILEGE` the
/// hard-min set fails; the helper retries with the soft flag, which
/// the OS may trim under extreme pressure.
///
/// Returns true on either successful set; false only if both
/// attempts failed (which the lock retry loop maps to `ENOMEM`).
LIBC_INLINE bool expand_working_set(HANDLE process, SIZE_T additional) {
  SIZE_T min_ws, max_ws;
  ULONG flags;

  if (!query_working_set(process, min_ws, max_ws, flags))
    return false;

  constexpr SIZE_T kMaxHeadroom = 256ULL * 1024 * 1024;
  SIZE_T headroom = additional / 4;
  if (headroom > kMaxHeadroom)
    headroom = kMaxHeadroom;

  // Saturating arithmetic for new_min = min_ws + additional + headroom.
  SIZE_T new_min = min_ws;
  if (additional <= SIZE_MAX - new_min) {
    new_min += additional;
    if (headroom <= SIZE_MAX - new_min)
      new_min += headroom;
  } else {
    new_min = SIZE_MAX;
  }

  SIZE_T new_max = max_ws;
  if (new_max < new_min) {
    new_max = new_min;
    if (headroom <= SIZE_MAX - new_max)
      new_max += headroom;
  }

  if (set_working_set(process, new_min, new_max,
                      QUOTA_LIMITS_HARDWS_MIN_ENABLE))
    return true;
  return set_working_set(process, new_min, new_max, 0);
}

/// True iff `nt_pal::lock_range` can accept the chunk described by
/// `mbi`. The Windows lock primitive refuses `PAGE_NOACCESS` and
/// `PAGE_GUARD` pages — the kernel cannot raise either trap on a
/// locked-resident page — and refuses uncommitted reservations
/// outright. Callers walking a range pre-filter on this so per-chunk
/// lock failures do not poison best-effort sweeps (`mlockall`,
/// `munlockall`).
///
/// Distinct from Linux, where `mlock` succeeds on `PROT_NONE` ranges
/// (the kernel pins physical frames regardless of access protection).
/// Programs that rely on the Linux semantic must reorder
/// `mprotect(... PROT_NONE)` to follow the lock on Windows.
LIBC_INLINE bool is_lockable(const MEMORY_BASIC_INFORMATION &mbi) {
  if (mbi.State != MEM_COMMIT)
    return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
    return false;
  return true;
}

} // namespace nt_pal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_WORKING_SET_H
