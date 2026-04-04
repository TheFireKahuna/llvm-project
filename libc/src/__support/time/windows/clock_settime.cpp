//===--- clock_settime windows implementation -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "hdr/time_macros.h"

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/time/clock_settime.h"
#include "src/__support/time/units.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Temporarily enable SeSystemtimePrivilege in the process token, call
// NtSetSystemTime, then restore the previous privilege state. This makes
// clock_settime work for admin processes without requiring callers to
// manage Windows-specific privilege tokens — matching POSIX expectations.
ErrorOr<int> clock_settime(clockid_t clockid, const timespec *ts) {
  using namespace time_units;

  // Only CLOCK_REALTIME is settable. All others are hardware-derived.
  if (clockid != CLOCK_REALTIME)
    return cpp::unexpected(EINVAL);

  if (!ts)
    return cpp::unexpected(EINVAL);

  if (ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)
    return cpp::unexpected(EINVAL);

  // Convert POSIX epoch (1970) to Windows epoch (1601), in 100ns units.
  constexpr unsigned long long HNS_PER_SEC = 1_s_ns / 100ULL;
  constexpr unsigned long long EPOCH_DIFF_HNS = 11644473600ULL * HNS_PER_SEC;

  // Overflow check: tv_sec * HNS_PER_SEC must not wrap.
  if (ts->tv_sec < 0)
    return cpp::unexpected(EINVAL);

  unsigned long long sec_hns =
      static_cast<unsigned long long>(ts->tv_sec) * HNS_PER_SEC;
  unsigned long long nsec_hns =
      static_cast<unsigned long long>(ts->tv_nsec) / 100ULL;
  unsigned long long posix_hns = sec_hns + nsec_hns;

  // Check for overflow when adding epoch offset.
  if (posix_hns > ~0ULL - EPOCH_DIFF_HNS)
    return cpp::unexpected(EOVERFLOW);

  LARGE_INTEGER new_time;
  new_time.QuadPart = static_cast<long long>(posix_hns + EPOCH_DIFF_HNS);

  // Enable SeSystemtimePrivilege for the duration of the call.
  HANDLE token = NtCurrentProcessToken();
  TOKEN_PRIVILEGES tp;
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = {SE_SYSTEMTIME_PRIVILEGE, 0};
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  TOKEN_PRIVILEGES prev;
  ULONG prev_len = 0;
  NTSTATUS priv_status = ::NtAdjustPrivilegesToken(
      token, 0, &tp, sizeof(prev), &prev, &prev_len);

  // If privilege adjustment fails, the token doesn't hold the privilege.
  if (!NT_SUCCESS(priv_status))
    return cpp::unexpected(EPERM);

  NTSTATUS status = ::NtSetSystemTime(&new_time, nullptr);

  // Restore previous privilege state regardless of NtSetSystemTime result.
  if (prev_len > 0)
    ::NtAdjustPrivilegesToken(token, 0, &prev, 0, nullptr, nullptr);

  if (!NT_SUCCESS(status))
    return cpp::unexpected(EPERM);

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
