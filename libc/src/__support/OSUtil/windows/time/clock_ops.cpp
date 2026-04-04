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
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/time/units.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t clock_getres(clockid_t id, struct timespec *res) {
  if (is_thread_cpuclockid(id))
    id = CLOCK_THREAD_CPUTIME_ID;
  // POSIX allows nullptr to be passed as res, in which case the function should
  // do nothing.
  if (res == nullptr)
    return 0;

  // Resolution sources, by clock family:
  //   CLOCK_MONOTONIC*  / CLOCK_BOOTTIME* — 1 / RtlQueryPerformanceFrequency
  //   CLOCK_REALTIME*                     — 100 ns (RtlGetSystemTimePrecise)
  //   CLOCK_*_CPUTIME_ID                  — 100 ns (KERNEL_USER_TIMES unit)

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
  case CLOCK_THREAD_CPUTIME_ID:
    // CLOCK_*_CPUTIME_ID is sourced from KERNEL_USER_TIMES.{Kernel,User}Time
    // in clock_gettime above — those QuadPart fields are 100ns-unit counters,
    // so 100ns IS the smallest representable difference (the "resolution"
    // POSIX asks for here). Linux returns 1ns for the same reason: it
    // reports the unit of measurement, not the kernel's tick rate.
    //
    // The previous implementation queried SystemTimeAdjustmentInformation,
    // which describes the *realtime clock's* tick adjustment knob (used by
    // SetSystemTimeAdjustment) — a different quantity entirely. That call
    // is irrelevant to CPU-time counters even when it succeeds.
    res->tv_sec = 0;
    res->tv_nsec = 100;
    break;

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
  windows::ScopedNtHandle h;
  NTSTATUS st = ::NtOpenProcessById(h.put(), PROCESS_QUERY_LIMITED_INFORMATION,
                                    static_cast<DWORD>(pid));
  if (!NT_SUCCESS(st))
    return (st == STATUS_ACCESS_DENIED) ? -EPERM : -ESRCH;

  // Our CLOCK_PROCESS_CPUTIME_ID implementation only reads the current
  // process's times. Per-pid CPU clocks would require holding the handle
  // and routing through NtQueryInformationProcess — not yet supported.
  // POSIX permits EPERM here.
  return -EPERM;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
