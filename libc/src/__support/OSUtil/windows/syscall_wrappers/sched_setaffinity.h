//===-- windows_syscalls::sched_setaffinity() wrapper -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_SCHED_SETAFFINITY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_SCHED_SETAFFINITY_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/cpu_set_t.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/size_t.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include "src/__support/OSUtil/windows/sched/affinity_ops.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

LIBC_INLINE ErrorOr<int> sched_setaffinity(pid_t tid, size_t cpuset_size,
                                           const cpu_set_t *mask) {
  intptr_t ret = internal::sched_setaffinity(tid, cpuset_size, mask);
  if (ret < 0)
    return Error(-static_cast<int>(ret));
  return 0;
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_SCHED_SETAFFINITY_H
