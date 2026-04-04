//===--- nanosleep windows implementation ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Kernel function — implements Linux SYS_nanosleep semantics in userspace.
// Returns 0 on success, -EINTR on signal interruption, -EINVAL on bad args.
// SA_RESTART is handled here: if the interrupted signal has SA_RESTART set,
// the sleep resumes transparently (matching Linux kernel behavior).
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "hdr/time_macros.h"

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/time/clock_gettime.h"
#include "src/__support/time/nanosleep.h"
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

long long read_monotonic_ns() {
  timespec ts;
  internal::clock_gettime(CLOCK_MONOTONIC, &ts);
  return timespec_to_ns(&ts);
}

} // namespace

long nanosleep(const timespec *req, timespec *rem) {
  using namespace time_units;

  if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1_s_ns)
    return -EINVAL;

  long long total_ns = timespec_to_ns(req);

  // Zero or negative duration: return immediately.
  if (total_ns <= 0) {
    if (rem)
      ns_to_timespec(0, rem);
    return 0;
  }

  // Convert to 100ns units, negative = relative timeout for NT APIs.
  LARGE_INTEGER interval;
  interval.QuadPart = -(total_ns / 100LL);

  long long start_ns = read_monotonic_ns();

  auto remaining_ns = [&]() -> long long {
    long long elapsed = read_monotonic_ns() - start_ns;
    long long r = total_ns - elapsed;
    return r > 0 ? r : 0;
  };

  for (;;) {
    NTSTATUS status = ::NtDelayExecution(/*Alertable=*/TRUE, &interval);

    if (status == STATUS_USER_APC || status == STATUS_ALERTED) {
      if (signal_state::should_restart_syscall()) {
        long long r = remaining_ns();
        if (r == 0)
          return 0;
        interval.QuadPart = -(r / 100LL);
        continue;
      }

      if (rem)
        ns_to_timespec(remaining_ns(), rem);
      return -EINTR;
    }

    // STATUS_SUCCESS or STATUS_TIMEOUT: sleep completed.
    if (rem)
      ns_to_timespec(0, rem);
    return 0;
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
