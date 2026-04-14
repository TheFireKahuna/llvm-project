//===-- Windows implementation of epoll_pwait -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// epoll_pwait = sigprocmask + epoll_wait + sigprocmask restore.
// The signal mask is atomically applied during the wait via the
// alertable NtRemoveIoCompletionEx call.
//
//===----------------------------------------------------------------------===//

#include "src/sys/epoll/epoll_pwait.h"

#include "hdr/types/sigset_t.h"
#include "hdr/types/struct_epoll_event.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/signal/sigprocmask.h"
#include "src/sys/epoll/epoll_wait.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, epoll_pwait,
                   (int epfd, struct epoll_event *events, int maxevents,
                    int timeout, const sigset_t *sigmask)) {
  sigset_t oldmask;
  if (sigmask)
    LIBC_NAMESPACE::sigprocmask(SIG_SETMASK, sigmask, &oldmask);

  int ret = LIBC_NAMESPACE::epoll_wait(epfd, events, maxevents, timeout);

  if (sigmask)
    LIBC_NAMESPACE::sigprocmask(SIG_SETMASK, &oldmask, nullptr);

  return ret;
}

} // namespace LIBC_NAMESPACE_DECL
