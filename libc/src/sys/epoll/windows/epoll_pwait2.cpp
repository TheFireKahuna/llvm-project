//===-- Windows implementation of epoll_pwait2 ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// epoll_pwait2 = epoll_pwait with timespec instead of int timeout.
//
//===----------------------------------------------------------------------===//

#include "src/sys/epoll/epoll_pwait2.h"

#include "hdr/types/sigset_t.h"
#include "hdr/types/struct_epoll_event.h"
#include "hdr/types/struct_timespec.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/signal/sigprocmask.h"
#include "src/sys/epoll/epoll_wait.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, epoll_pwait2,
                   (int epfd, struct epoll_event *events, int maxevents,
                    const struct timespec *timeout, const sigset_t *sigmask)) {
  // Convert timespec to milliseconds for epoll_wait.
  int timeout_ms = -1;
  if (timeout) {
    long long ms = static_cast<long long>(timeout->tv_sec) * 1000 +
                   static_cast<long long>(timeout->tv_nsec) / 1000000;
    if (ms > 0x7FFFFFFF)
      ms = 0x7FFFFFFF;
    timeout_ms = static_cast<int>(ms);
  }

  sigset_t oldmask;
  if (sigmask)
    LIBC_NAMESPACE::sigprocmask(SIG_SETMASK, sigmask, &oldmask);

  int ret = LIBC_NAMESPACE::epoll_wait(epfd, events, maxevents, timeout_ms);

  if (sigmask)
    LIBC_NAMESPACE::sigprocmask(SIG_SETMASK, &oldmask, nullptr);

  return ret;
}

} // namespace LIBC_NAMESPACE_DECL
