//===-- Windows implementation of alarm ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX: alarm(n) is equivalent to setitimer(ITIMER_REAL, ...) with a
// one-shot timer of n seconds and no repeat interval.
//
//===----------------------------------------------------------------------===//

#include "src/unistd/alarm.h"
#include "hdr/types/struct_itimerval.h"
#include "include/llvm-libc-macros/sys-time-macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/sys/time/setitimer.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(unsigned, alarm, (unsigned seconds)) {
  struct itimerval new_val = {};
  new_val.it_value.tv_sec = seconds;

  struct itimerval old_val = {};
  LIBC_NAMESPACE::setitimer(ITIMER_REAL, &new_val, &old_val);

  // Return remaining seconds from previous alarm, rounded up.
  unsigned remaining = static_cast<unsigned>(old_val.it_value.tv_sec);
  if (old_val.it_value.tv_usec > 0)
    ++remaining;
  return remaining;
}

} // namespace LIBC_NAMESPACE_DECL
