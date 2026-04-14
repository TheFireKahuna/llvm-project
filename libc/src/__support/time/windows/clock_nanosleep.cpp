//===--- clock_nanosleep windows implementation -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Kernel function — implements Linux SYS_clock_nanosleep semantics in
// userspace. Returns 0 on success, -EINTR on signal interruption, -EINVAL
// on bad args. Supports both absolute and relative sleeps across multiple
// clock domains. SA_RESTART is handled transparently.
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "hdr/time_macros.h"

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/time/clock_gettime.h"
#include "src/__support/time/clock_nanosleep.h"
#include "src/__support/time/units.h"
#include "src/__support/OSUtil/windows/signal/signal.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

constexpr long long NS_PER_SEC = 1'000'000'000LL;

long long timespec_to_ns(const timespec *ts) {
  return static_cast<long long>(ts->tv_sec) * NS_PER_SEC +
         static_cast<long long>(ts->tv_nsec);
}

void ns_to_timespec(long long ns, timespec *ts) {
  if (ns <= 0) {
    ts->tv_sec = 0;
    ts->tv_nsec = 0;
    return;
  }
  ts->tv_sec = static_cast<decltype(ts->tv_sec)>(ns / NS_PER_SEC);
  ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>(ns % NS_PER_SEC);
}

long long read_clock_ns(clockid_t clockid) {
  timespec ts;
  internal::clock_gettime(clockid, &ts);
  return timespec_to_ns(&ts);
}

} // namespace

long clock_nanosleep(clockid_t clockid, int flags, const timespec *req,
                     timespec *rem) {
  using namespace time_units;

  // POSIX: CPUTIME clocks and alarm clocks are not valid for nanosleep.
  // Accept all monotonic variants (RAW, COARSE) as aliases.
  switch (clockid) {
  case CLOCK_REALTIME:
  case CLOCK_REALTIME_COARSE:
  case CLOCK_MONOTONIC:
  case CLOCK_MONOTONIC_RAW:
  case CLOCK_MONOTONIC_COARSE:
  case CLOCK_BOOTTIME:
    break;
  default:
    return -EINVAL;
  }

  if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1_s_ns)
    return -EINVAL;

  if (flags != 0 && flags != TIMER_ABSTIME)
    return -EINVAL;

  bool absolute = (flags & TIMER_ABSTIME) != 0;

  // Compute initial sleep duration in nanoseconds.
  long long target_ns;
  if (absolute) {
    target_ns = timespec_to_ns(req);
    long long now_ns = read_clock_ns(clockid);
    if (target_ns - now_ns <= 0)
      return 0; // Deadline already passed.
  }

  // For relative sleeps, record the total duration for remainder computation.
  long long total_ns = absolute ? 0 : timespec_to_ns(req);
  long long start_ns = absolute ? 0 : read_clock_ns(clockid);

  auto compute_remaining_ns = [&]() -> long long {
    if (absolute) {
      long long delta = target_ns - read_clock_ns(clockid);
      return delta > 0 ? delta : 0;
    }
    long long elapsed = read_clock_ns(clockid) - start_ns;
    long long remaining = total_ns - elapsed;
    return remaining > 0 ? remaining : 0;
  };

  for (;;) {
    long long sleep_ns = compute_remaining_ns();
    if (sleep_ns <= 0)
      return 0;

    // Convert to 100ns units, negative = relative for NT.
    LARGE_INTEGER interval;
    interval.QuadPart = -(sleep_ns / 100LL);

    NTSTATUS status = ::NtDelayExecution(/*Alertable=*/TRUE, &interval);

    if (status == STATUS_USER_APC || status == STATUS_ALERTED) {
      if (signal_state::should_restart_syscall())
        continue; // SA_RESTART: recompute interval and retry.

      // POSIX: for absolute sleeps, rem is unused. For relative, write
      // remaining time.
      if (!absolute && rem)
        ns_to_timespec(compute_remaining_ns(), rem);
      return -EINTR;
    }

    // STATUS_SUCCESS / STATUS_TIMEOUT: sleep completed.
    return 0;
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
