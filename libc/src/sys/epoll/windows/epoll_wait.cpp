//===-- Windows implementation of epoll_wait ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::epoll_wait() in epoll_ops.cpp.
// cancel_check() and signal restart logic remain here at the entry point.
//
//===----------------------------------------------------------------------===//

#include "src/sys/epoll/epoll_wait.h"

#include "hdr/types/struct_epoll_event.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/epoll_wait.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/pthread/cancel_internal.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, epoll_wait,
                   (int epfd, struct epoll_event *events, int maxevents,
                    int timeout)) {
  cancel_check(); // POSIX cancellation point.

  auto result = windows_syscalls::epoll_wait(epfd, events, maxevents, timeout);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
