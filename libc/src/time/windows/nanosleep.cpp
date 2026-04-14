//===-- Windows implementation of nanosleep -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/time/nanosleep.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/nanosleep.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/pthread/cancel_internal.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, nanosleep,
                   (const timespec *req, timespec *rem)) {
  cancel_check(); // POSIX cancellation point.
  auto result = windows_syscalls::nanosleep(req, rem);
  cancel_check(); // Re-check after wakeup — cancel may have been requested
                   // while we were sleeping (NtAlertThreadByThreadId wakes us).
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
