//===-- Windows implementation of epoll_ctl -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::epoll_ctl() in epoll_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/epoll/epoll_ctl.h"

#include "hdr/types/struct_epoll_event.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/epoll_ctl.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, epoll_ctl,
                   (int epfd, int op, int fd, struct epoll_event *event)) {
  auto result = windows_syscalls::epoll_ctl(epfd, op, fd, event);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
