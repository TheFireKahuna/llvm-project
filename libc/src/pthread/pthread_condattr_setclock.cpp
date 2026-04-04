//===-- Implementation of the pthread_condattr_setclock -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_condattr_setclock.h"

#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"

#include "hdr/time_macros.h" // CLOCK_MONOTONIC, CLOCK_REALTIME
#include <pthread.h>         // pthread_condattr_t
#include <sys/types.h>       // clockid_t

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "src/__support/time/windows/clock_conversion.h"
#endif

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_condattr_setclock,
                   (pthread_condattr_t * attr, clockid_t clock)) {

#ifdef LIBC_TARGET_OS_IS_WINDOWS
  if (!internal::is_valid_wait_clock(clock))
    return EINVAL;
#else
  if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)
    return EINVAL;
#endif

  attr->clock = clock;
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
