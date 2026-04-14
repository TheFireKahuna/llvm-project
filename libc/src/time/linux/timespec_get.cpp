//===-- Implementation of timespec_get for Linux --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/time/timespec_get.h"
#include "hdr/time_macros.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/time/clock_gettime.h"

namespace LIBC_NAMESPACE_DECL {

// Internal constants matching the public TIME_* macros.
// The library supports all bases; user visibility is controlled by headers.
constexpr int TIMESPEC_UTC = 1;           // C11
constexpr int TIMESPEC_MONOTONIC = 2;     // C23
constexpr int TIMESPEC_ACTIVE = 3;        // C23
constexpr int TIMESPEC_THREAD_ACTIVE = 4; // C23

LLVM_LIBC_FUNCTION(int, timespec_get, (timespec * ts, int base)) {
  clockid_t clockid;
  switch (base) {
  case TIMESPEC_UTC:
    clockid = CLOCK_REALTIME;
    break;
  case TIMESPEC_MONOTONIC:
    clockid = CLOCK_MONOTONIC;
    break;
  case TIMESPEC_ACTIVE:
    clockid = CLOCK_PROCESS_CPUTIME_ID;
    break;
  case TIMESPEC_THREAD_ACTIVE:
    clockid = CLOCK_THREAD_CPUTIME_ID;
    break;
  default:
    return 0;
  }

  auto result = internal::clock_gettime(clockid, ts);
  if (!result.has_value()) {
    libc_errno = result.error();
    return 0;
  }
  return base;
}

} // namespace LIBC_NAMESPACE_DECL
