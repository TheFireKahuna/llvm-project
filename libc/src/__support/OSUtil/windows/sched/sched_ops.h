//===-- Internal sched simple op declarations -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: sched functions that implement Linux
// syscall semantics (return 0/value on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SCHED_SCHED_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SCHED_SCHED_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"
#include <time.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t sched_yield();
intptr_t getcpu(unsigned int *cpu, unsigned int *node);
intptr_t sched_getcpu();
intptr_t sched_get_priority_max(int policy);
intptr_t sched_get_priority_min(int policy);
intptr_t sched_rr_get_interval(pid_t tid, struct timespec *tp);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SCHED_SCHED_OPS_H
