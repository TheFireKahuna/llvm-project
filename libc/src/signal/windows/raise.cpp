//===-- Windows implementation of raise -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::raise() in signal_syscalls.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/signal/raise.h"

#include "src/__support/OSUtil/windows/signal/signal_syscalls.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// ---------------------------------------------------------------------------
// POSIX entry point — libc/kernel ABI bridge.
// ---------------------------------------------------------------------------
LLVM_LIBC_FUNCTION(int, raise, (int signum)) {
  intptr_t ret = internal::raise(signum);
  if (ret < 0) {
    libc_errno = static_cast<int>(-ret);
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
