//===-- Windows implementation of sigaction -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to signal_state::rt_sigaction() in
// signal_syscalls.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/signal/sigaction.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/sigaction.h"
#include "hdr/types/struct_sigaction.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// ---------------------------------------------------------------------------
// POSIX entry point — libc/kernel ABI bridge.
// ---------------------------------------------------------------------------
LLVM_LIBC_FUNCTION(int, sigaction,
                   (int signum, const struct sigaction *__restrict act,
                    struct sigaction *__restrict oldact)) {
  auto result = windows_syscalls::sigaction(signum, act, oldact);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
