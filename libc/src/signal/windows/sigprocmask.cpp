//===-- Windows implementation of sigprocmask -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to signal_state::rt_sigprocmask() in
// signal_syscalls.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/signal/sigprocmask.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/sigprocmask.h"
#include "hdr/types/sigset_t.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// ---------------------------------------------------------------------------
// POSIX entry point — libc/kernel ABI bridge.
// ---------------------------------------------------------------------------
LLVM_LIBC_FUNCTION(int, sigprocmask,
                   (int how, const sigset_t *__restrict set,
                    sigset_t *__restrict oldset)) {
  auto result = windows_syscalls::sigprocmask(how, set, oldset);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
