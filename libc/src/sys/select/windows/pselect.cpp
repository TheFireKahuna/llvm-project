//===-- Windows implementation of pselect ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::pselect() in select_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "pselect.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/pselect.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/pthread/cancel_internal.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pselect,
                   (int nfds, fd_set *__restrict read_set,
                    fd_set *__restrict write_set,
                    fd_set *__restrict error_set,
                    const struct timespec *__restrict timeout,
                    const sigset_t *__restrict sigmask)) {
  cancel_check(); // POSIX cancellation point.

  auto result = windows_syscalls::pselect(nfds, read_set, write_set, error_set,
                                          timeout, sigmask);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
