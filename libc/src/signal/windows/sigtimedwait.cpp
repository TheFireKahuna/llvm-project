//===-- Windows implementation of sigtimedwait -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::sigtimedwait() in
// signal_syscalls.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/signal/sigtimedwait.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/sigtimedwait.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/pthread/cancel_internal.h"

namespace LIBC_NAMESPACE_DECL {

// ---------------------------------------------------------------------------
// POSIX entry point — libc/kernel ABI bridge.
// ---------------------------------------------------------------------------
LLVM_LIBC_FUNCTION(int, sigtimedwait,
                   (const sigset_t *__restrict set, siginfo_t *__restrict info,
                    const struct timespec *__restrict timeout)) {
  cancel_check(); // POSIX cancellation point.
  auto result = windows_syscalls::sigtimedwait(set, info, timeout);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
