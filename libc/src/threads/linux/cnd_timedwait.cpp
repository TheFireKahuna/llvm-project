//===-- Linux implementation of cnd_timedwait -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/threads/cnd_timedwait.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/CndVar.h"
#include "src/__support/threads/mutex.h"
#include "src/__support/time/abs_timeout.h"

#include <threads.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, cnd_timedwait,
                   (cnd_t *__restrict cond, mtx_t *__restrict mtx,
                    const struct timespec *__restrict ts)) {
  auto timeout = internal::AbsTimeout::from_timespec(
      *ts, /*is_realtime=*/true);
  if (!timeout)
    return thrd_error;

  CndVar *cv = reinterpret_cast<CndVar *>(cond);
  Mutex *m = reinterpret_cast<Mutex *>(mtx);
  int ret = cv->wait(m, timeout.value());
  if (ret == ETIMEDOUT)
    return thrd_timedout;
  if (ret == 0)
    return thrd_success;
  return thrd_error;
}

} // namespace LIBC_NAMESPACE_DECL
