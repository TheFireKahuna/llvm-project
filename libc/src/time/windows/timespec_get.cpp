//===-- Implementation of timespec_get for Windows ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/time/timespec_get.h"
#include "hdr/errno_macros.h"
#include "hdr/time_macros.h"
#include "hdr/types/clockid_t.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/time/clock_gettime.h"

namespace LIBC_NAMESPACE_DECL {

// Match the TIME_* macros from time_macros.h without including <time.h>.
constexpr int TIMESPEC_UTC = 1;
constexpr int TIMESPEC_MONOTONIC = 2;
constexpr int TIMESPEC_ACTIVE = 3;
constexpr int TIMESPEC_THREAD_ACTIVE = 4;

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
    libc_errno = EINVAL;
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
