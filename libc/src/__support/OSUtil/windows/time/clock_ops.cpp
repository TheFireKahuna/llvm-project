//===-- Windows internal clock_getres/clock_getcpuclockid ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal kernel functions for clock_getres and clock_getcpuclockid on
// Windows. These implement Linux syscall semantics: 0 on success, -errno on
// failure. Called from windows_syscalls:: wrappers.
//
//===----------------------------------------------------------------------===//

#include "clock_ops.h"
#include "hdr/errno_macros.h"
#include "hdr/time_macros.h"
#include "include/llvm-libc-types/struct_timespec.h"
#include "src/__support/CPP/limits.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/time/units.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t clock_getres(clockid_t id, struct timespec *res) {
  using namespace time_units;
  if (is_thread_cpuclockid(id))
    id = CLOCK_THREAD_CPUTIME_ID;
  // POSIX allows nullptr to be passed as res, in which case the function should
  // do nothing.
  if (res == nullptr)
    return 0;

  constexpr unsigned long long HNS_PER_SEC = 1_s_ns / 100ULL;
  constexpr unsigned long long SEC_LIMIT =
      cpp::numeric_limits<decltype(res->tv_sec)>::max();

  // CLOCK_MONOTONIC: 1 / RtlQueryPerformanceFrequency.
  // CLOCK_REALTIME: 100ns (RtlGetSystemTimePrecise granularity).
  // CLOCK_*_CPUTIME_ID: NtQuerySystemInformationEx(SystemTimeAdjustmentInformation).

  switch (id) {
  default:
    return -EINVAL;

  // Monotonic clocks derive from QueryPerformanceCounter, so their
  // resolution is 1 / RtlQueryPerformanceFrequency.
  case CLOCK_MONOTONIC:
  case CLOCK_MONOTONIC_RAW:
  case CLOCK_MONOTONIC_COARSE:
  case CLOCK_BOOTTIME:
  case CLOCK_BOOTTIME_ALARM: {
    LARGE_INTEGER freq;
    ::RtlQueryPerformanceFrequency(&freq);
    res->tv_sec = 0;
    res->tv_nsec =
        static_cast<decltype(res->tv_nsec)>(1'000'000'000LL / freq.QuadPart);
    // Clamp to 1 ns minimum — QPC frequency can exceed 1 GHz on some
    // hardware, but the resolution is still at least 1 ns.
    if (res->tv_nsec == 0)
      res->tv_nsec = 1;
    break;
  }

  // RtlGetSystemTimePrecise returns 100ns-unit timestamps interpolated
  // with QPC, so the achievable resolution is 100 ns.
  case CLOCK_REALTIME:
  case CLOCK_REALTIME_ALARM:
  case CLOCK_REALTIME_COARSE: {
    res->tv_sec = 0;
    res->tv_nsec = 100;
    break;
  }

  case CLOCK_PROCESS_CPUTIME_ID:
  case CLOCK_THREAD_CPUTIME_ID: {
    [[clang::uninitialized]] SYSTEM_QUERY_TIME_ADJUST_INFORMATION_PRECISE info;
    NTSTATUS status = ::NtQuerySystemInformationEx(
        SystemTimeAdjustmentInformation, nullptr, 0, &info, sizeof(info),
        nullptr);
    if (NT_ERROR(status))
      return -EINVAL;
    unsigned long long tv_sec = info.TimeIncrement / HNS_PER_SEC;
    unsigned long long tv_nsec = (info.TimeIncrement % HNS_PER_SEC) * 100ULL;
    if (LIBC_UNLIKELY(tv_sec > SEC_LIMIT))
      return -EOVERFLOW;
    res->tv_sec = static_cast<decltype(res->tv_sec)>(tv_sec);
    res->tv_nsec = static_cast<decltype(res->tv_nsec)>(tv_nsec);
    break;
  }

  case CLOCK_TAI:
    return -ENOTSUP;
  }
  return 0;
}

intptr_t clock_getcpuclockid(pid_t pid, clockid_t *clock_id) {
  // POSIX: pid 0 means the calling process.
  if (pid == 0 || static_cast<DWORD>(pid) == NtCurrentProcessId()) {
    *clock_id = CLOCK_PROCESS_CPUTIME_ID;
    return 0;
  }

  // Validate that the target process exists. PROCESS_QUERY_LIMITED_INFORMATION
  // is the minimum access right needed to confirm existence.
  HANDLE h = nullptr;
  NTSTATUS st = ::NtOpenProcessById(&h, PROCESS_QUERY_LIMITED_INFORMATION,
                                    static_cast<DWORD>(pid));
  if (!NT_SUCCESS(st))
    return (st == STATUS_ACCESS_DENIED) ? -EPERM : -ESRCH;
  ::NtClose(h);

  // Our CLOCK_PROCESS_CPUTIME_ID implementation only reads the current
  // process's times. Per-pid CPU clocks would require holding the handle
  // and routing through NtQueryInformationProcess — not yet supported.
  // POSIX permits EPERM here.
  return -EPERM;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
