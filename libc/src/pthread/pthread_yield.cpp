//===-- Implementation of pthread_yield -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_yield.h"

#include "src/__support/OSUtil/syscall.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// pthread_yield was deprecated by POSIX-1003.1-2008 in favor of
// sched_yield (and removed entirely in glibc 2.34's headers behind a
// strict-mode gate). Programs from before that era still link against
// it, so we keep an honest implementation that delegates to the same
// scheduler hint syscall that sched_yield uses. Returns 0 on success;
// the engine's sched_yield never fails on Windows (always returns 0)
// and Linux returns 0 unconditionally too.
LLVM_LIBC_FUNCTION(int, pthread_yield, ()) {
  long ret = syscall_impl<long>(SYS_sched_yield);
  return ret == 0 ? 0 : static_cast<int>(-ret);
}

} // namespace LIBC_NAMESPACE_DECL
