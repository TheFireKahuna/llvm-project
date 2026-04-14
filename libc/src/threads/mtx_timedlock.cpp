//===-- Implementation of mtx_timedlock -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/threads/mtx_timedlock.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/mutex.h"
#include "src/__support/time/abs_timeout.h"

#include <threads.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, mtx_timedlock,
                   (mtx_t *__restrict mutex,
                    const timespec *__restrict ts)) {
  auto timeout = internal::AbsTimeout::from_timespec(
      *ts, /*is_realtime=*/true);
  if (!timeout)
    return thrd_error;

  auto err = reinterpret_cast<Mutex *>(mutex)->timed_lock(timeout.value());
  if (err == MutexError::NONE)
    return thrd_success;
  if (err == MutexError::TIMEOUT)
    return thrd_timedout;
  return thrd_error;
}

} // namespace LIBC_NAMESPACE_DECL
