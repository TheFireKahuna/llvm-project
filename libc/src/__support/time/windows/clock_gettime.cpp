//===--- clock_gettime windows implementation -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "hdr/time_macros.h"

#include "src/__support/OSUtil/windows/time/clock_ops.h"
#include "src/__support/CPP/limits.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/time/clock_gettime.h"
#include "src/__support/time/units.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

ErrorOr<int> clock_gettime(clockid_t clockid, timespec *ts) {
  using namespace time_units;
  constexpr unsigned long long HNS_PER_SEC = 1_s_ns / 100ULL;
  constexpr long long SEC_LIMIT =
      cpp::numeric_limits<decltype(ts->tv_sec)>::max();
  ErrorOr<int> ret = 0;

  if (is_thread_cpuclockid(clockid)) {
    const uint32_t tid = thread_cpuclockid_tid(clockid);
    HANDLE handle = NtCurrentThread();
    bool opened = false;

    if (tid != static_cast<uint32_t>(::NtCurrentThreadId())) {
      CLIENT_ID cid{nullptr,
                    reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(tid))};
      NTSTATUS open_status =
          ::NtOpenThread(&handle, THREAD_QUERY_INFORMATION, nullptr, &cid);
      if (!NT_SUCCESS(open_status))
        return cpp::unexpected(ESRCH);
      opened = true;
    }

    KERNEL_USER_TIMES times;
    NTSTATUS status =
        ::NtQueryInformationThread(handle, ThreadTimes, &times, sizeof(times),
                                   nullptr);
    if (opened)
      ::NtClose(handle);
    if (!NT_SUCCESS(status))
      return cpp::unexpected(ESRCH);

    unsigned long long total_time_hns =
        static_cast<unsigned long long>(times.KernelTime.QuadPart) +
        static_cast<unsigned long long>(times.UserTime.QuadPart);
    unsigned long long tv_sec = total_time_hns / HNS_PER_SEC;
    unsigned long long tv_nsec = (total_time_hns % HNS_PER_SEC) * 100ULL;
    if (LIBC_UNLIKELY(tv_sec > SEC_LIMIT))
      return cpp::unexpected(EOVERFLOW);

    ts->tv_sec = static_cast<decltype(ts->tv_sec)>(tv_sec);
    ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>(tv_nsec);
    return 0;
  }

  switch (clockid) {
  default:
    ret = cpp::unexpected(EINVAL);
    break;

  // Interrupt time is hardware-driven and not NTP-slewed, so RAW and COARSE
  // are identical to MONOTONIC on Windows.
  case CLOCK_MONOTONIC:
  case CLOCK_MONOTONIC_RAW:
  case CLOCK_MONOTONIC_COARSE: {
    // RtlQueryUnbiasedInterruptTime: 100ns units, excludes suspend time.
    // Matches Linux CLOCK_MONOTONIC semantics. Reads KUSER_SHARED_DATA
    // with a seqlock — zero syscalls, no frequency division needed.
    ULONGLONG interrupt_time;
    ::RtlQueryUnbiasedInterruptTime(&interrupt_time);
    unsigned long long tv_sec = interrupt_time / HNS_PER_SEC;
    unsigned long long tv_nsec = (interrupt_time % HNS_PER_SEC) * 100ULL;
    if (LIBC_UNLIKELY(tv_sec > SEC_LIMIT)) {
      ret = cpp::unexpected(EOVERFLOW);
      break;
    }
    ts->tv_sec = static_cast<decltype(ts->tv_sec)>(tv_sec);
    ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>(tv_nsec);
    break;
  }

  // Alarm clocks read identically to their non-alarm counterparts.
  // The "alarm" (wake-from-suspend) behavior only applies to timer_create.
  case CLOCK_BOOTTIME_ALARM:
  case CLOCK_BOOTTIME: {
    // InterruptTime from KUSER_SHARED_DATA: 100ns units, includes suspend.
    // Matches Linux CLOCK_BOOTTIME semantics. Single atomic 64-bit load.
    ULONGLONG interrupt_time;
    ::QueryInterruptTime(&interrupt_time);
    unsigned long long tv_sec = interrupt_time / HNS_PER_SEC;
    unsigned long long tv_nsec = (interrupt_time % HNS_PER_SEC) * 100ULL;
    if (LIBC_UNLIKELY(tv_sec > SEC_LIMIT)) {
      ret = cpp::unexpected(EOVERFLOW);
      break;
    }
    ts->tv_sec = static_cast<decltype(ts->tv_sec)>(tv_sec);
    ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>(tv_nsec);
    break;
  }

  case CLOCK_REALTIME_ALARM:
  case CLOCK_REALTIME: {
    // RtlGetSystemTimePrecise: 100ns units since 1601-01-01, sub-us precision.
    // Interpolates between kernel ticks using QPC — zero syscalls.
    ULARGE_INTEGER time;
    time.QuadPart = static_cast<unsigned long long>(::RtlGetSystemTimePrecise());

    // Adjust to POSIX epoch (from Jan 1, 1601 to Jan 1, 1970)
    constexpr unsigned long long POSIX_TIME_SHIFT =
        (11644473600ULL * HNS_PER_SEC);
    if (LIBC_UNLIKELY(POSIX_TIME_SHIFT > time.QuadPart)) {
      ret = cpp::unexpected(EOVERFLOW);
      break;
    }
    time.QuadPart -= (11644473600ULL * HNS_PER_SEC);
    unsigned long long tv_sec = time.QuadPart / HNS_PER_SEC;
    unsigned long long tv_nsec = (time.QuadPart % HNS_PER_SEC) * 100ULL;
    if (LIBC_UNLIKELY(tv_sec > SEC_LIMIT)) {
      ret = cpp::unexpected(EOVERFLOW);
      break;
    }
    ts->tv_sec = static_cast<decltype(ts->tv_sec)>(tv_sec);
    ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>(tv_nsec);
    break;
  }

  case CLOCK_PROCESS_CPUTIME_ID:
  case CLOCK_THREAD_CPUTIME_ID: {
    // Query CPU times directly via NT API — avoids kernel32 indirection.
    // Both info classes return KERNEL_USER_TIMES with 100ns-unit fields.
    KERNEL_USER_TIMES times;
    NTSTATUS status;
    if (clockid == CLOCK_PROCESS_CPUTIME_ID) {
      status = ::NtQueryInformationProcess(NtCurrentProcess(), ProcessTimes,
                                           &times, sizeof(times), nullptr);
    } else {
      status = ::NtQueryInformationThread(NtCurrentThread(), ThreadTimes,
                                          &times, sizeof(times), nullptr);
    }
    if (!NT_SUCCESS(status)) {
      ret = cpp::unexpected(EINVAL);
      break;
    }
    unsigned long long total_time_hns =
        static_cast<unsigned long long>(times.KernelTime.QuadPart) +
        static_cast<unsigned long long>(times.UserTime.QuadPart);

    unsigned long long tv_sec = total_time_hns / HNS_PER_SEC;
    unsigned long long tv_nsec = (total_time_hns % HNS_PER_SEC) * 100ULL;

    if (LIBC_UNLIKELY(tv_sec > SEC_LIMIT)) {
      ret = cpp::unexpected(EOVERFLOW);
      break;
    }

    ts->tv_sec = static_cast<decltype(ts->tv_sec)>(tv_sec);
    ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>(tv_nsec);

    break;
  }

  case CLOCK_REALTIME_COARSE: {
    // KUSER_SHARED_DATA.SystemTime: tick-granularity (~1ms), no QPC
    // interpolation. Single atomic load from 0x7FFE0014.
    ULONGLONG system_time;
    ::QueryCoarseSystemTime(&system_time);
    constexpr unsigned long long POSIX_TIME_SHIFT =
        (11644473600ULL * HNS_PER_SEC);
    if (LIBC_UNLIKELY(POSIX_TIME_SHIFT > system_time)) {
      ret = cpp::unexpected(EOVERFLOW);
      break;
    }
    system_time -= POSIX_TIME_SHIFT;
    unsigned long long tv_sec = system_time / HNS_PER_SEC;
    unsigned long long tv_nsec = (system_time % HNS_PER_SEC) * 100ULL;
    if (LIBC_UNLIKELY(tv_sec > SEC_LIMIT)) {
      ret = cpp::unexpected(EOVERFLOW);
      break;
    }
    ts->tv_sec = static_cast<decltype(ts->tv_sec)>(tv_sec);
    ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>(tv_nsec);
    break;
  }

  case CLOCK_TAI: {
    ret = cpp::unexpected(ENOTSUP);
    break;
  }
  }
  return ret;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
