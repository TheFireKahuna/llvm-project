//===--- Clock domain conversion for Windows --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Converts POSIX clock deadlines to AbsTimeout on Windows. Handles domain
// conversion for clocks that have no direct NT wait equivalent:
//
//   CLOCK_BOOTTIME → CLOCK_REALTIME domain (absolute system time).
//   Both advance during suspend, so the delta is preserved. The kernel's
//   absolute wait mode enforces the exact deadline with no TOCTOU.
//
//   CLOCK_MONOTONIC variants → passed through as-is (non-realtime).
//   CLOCK_REALTIME variants  → passed through as-is (realtime).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_TIME_WINDOWS_CLOCK_CONVERSION_H
#define LLVM_LIBC_SRC___SUPPORT_TIME_WINDOWS_CLOCK_CONVERSION_H

#include "hdr/time_macros.h"
#include "hdr/types/struct_timespec.h"
#include "src/__support/CPP/expected.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/time/abs_timeout.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Convert a clock-qualified absolute timespec to an AbsTimeout, performing
// domain conversion where needed. Returns AbsTimeout or an error.
LIBC_INLINE cpp::expected<AbsTimeout, AbsTimeout::Error>
abs_timeout_from_clock(clockid_t clockid, const timespec &ts) {
  switch (clockid) {
  case CLOCK_REALTIME:
  case CLOCK_REALTIME_COARSE:
    return AbsTimeout::from_timespec(ts, /*realtime=*/true);

  case CLOCK_MONOTONIC:
  case CLOCK_MONOTONIC_RAW:
  case CLOCK_MONOTONIC_COARSE:
    return AbsTimeout::from_timespec(ts, /*realtime=*/false);

  case CLOCK_BOOTTIME: {
    // Convert boottime deadline to realtime domain. Both clocks advance
    // during suspend, so the delta is stable. Absolute system time wait
    // lets the kernel enforce the deadline with no TOCTOU.
    constexpr long long EPOCH_DIFF_HNS = 116444736000000000LL;
    ULONGLONG boottime_now;
    ::QueryInterruptTime(&boottime_now);
    long long system_now_hns =
        ::RtlGetSystemTimePrecise() - EPOCH_DIFF_HNS;

    long long deadline_hns =
        static_cast<long long>(ts.tv_sec) * 10000000LL + ts.tv_nsec / 100;
    long long delta = deadline_hns - static_cast<long long>(boottime_now);
    long long sys_deadline = system_now_hns + delta;

    timespec converted;
    if (sys_deadline <= 0) {
      converted.tv_sec = 0;
      converted.tv_nsec = 0;
    } else {
      converted.tv_sec =
          static_cast<decltype(converted.tv_sec)>(sys_deadline / 10000000LL);
      converted.tv_nsec =
          static_cast<decltype(converted.tv_nsec)>(
              (sys_deadline % 10000000LL) * 100);
    }
    return AbsTimeout::from_timespec(converted, /*realtime=*/true);
  }

  default:
    return cpp::unexpected(AbsTimeout::Error::Invalid);
  }
}

// Check if a clockid is valid for timed waits.
LIBC_INLINE bool is_valid_wait_clock(clockid_t clockid) {
  switch (clockid) {
  case CLOCK_REALTIME:
  case CLOCK_REALTIME_COARSE:
  case CLOCK_MONOTONIC:
  case CLOCK_MONOTONIC_RAW:
  case CLOCK_MONOTONIC_COARSE:
  case CLOCK_BOOTTIME:
    return true;
  default:
    return false;
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_TIME_WINDOWS_CLOCK_CONVERSION_H
