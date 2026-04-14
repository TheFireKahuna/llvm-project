//===-- Implementation of thrd_sleep ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/threads/thrd_sleep.h"
#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/libc_errno.h"
#include "src/time/nanosleep.h"

#include <threads.h>

namespace LIBC_NAMESPACE_DECL {

// C11 7.26.4.1: returns 0 on success, -1 if interrupted by a signal,
// or a negative value other than -1 on error.
LLVM_LIBC_FUNCTION(int, thrd_sleep,
                   (const struct timespec *duration,
                    struct timespec *remaining)) {
  int ret = LIBC_NAMESPACE::nanosleep(duration, remaining);
  if (ret == 0)
    return 0;
  if (libc_errno == EINTR)
    return -1;
  // EINVAL or other error.
  return -2;
}

} // namespace LIBC_NAMESPACE_DECL
