//===---------- Windows implementation of the POSIX ppoll function ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::ppoll() in poll_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/poll/ppoll.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/ppoll.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/pthread/cancel_internal.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, ppoll,
                   (struct pollfd * fds, nfds_t nfds,
                    const struct timespec *__restrict tmo_p,
                    const sigset_t *__restrict sigmask)) {
  cancel_check(); // POSIX cancellation point.

  auto result = windows_syscalls::ppoll(fds, nfds, tmo_p, sigmask);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
