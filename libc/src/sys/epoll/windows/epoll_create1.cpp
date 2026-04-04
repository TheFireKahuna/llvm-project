//===-- Windows implementation of epoll_create1 ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::epoll_create1() in epoll_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/epoll/epoll_create1.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/epoll_create1.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, epoll_create1, (int flags)) {
  auto result = windows_syscalls::epoll_create1(flags);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
