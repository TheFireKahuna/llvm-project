//===-- Windows implementation of inotify_add_watch ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/inotify/inotify_add_watch.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/inotify_add_watch.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, inotify_add_watch,
                   (int fd, const char *pathname, uint32_t mask)) {
  auto result = windows_syscalls::inotify_add_watch(fd, pathname, mask);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
