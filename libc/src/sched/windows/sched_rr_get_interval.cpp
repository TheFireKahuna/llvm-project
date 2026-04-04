//===-- Windows implementation of sched_rr_get_interval -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sched/sched_rr_get_interval.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/sched_rr_get_interval.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, sched_rr_get_interval,
                   (pid_t tid, struct timespec *tp)) {
  auto result = windows_syscalls::sched_rr_get_interval(tid, tp);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
