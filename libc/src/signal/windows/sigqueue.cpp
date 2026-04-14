//===-- Windows implementation of sigqueue ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::sigqueue() in
// signal_syscalls.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/signal/sigqueue.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/sigqueue.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/union_sigval.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// ---------------------------------------------------------------------------
// POSIX entry point — libc/kernel ABI bridge.
// ---------------------------------------------------------------------------
LLVM_LIBC_FUNCTION(int, sigqueue,
                   (pid_t pid, int sig, const union sigval value)) {
  auto result = windows_syscalls::sigqueue(pid, sig, value);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
