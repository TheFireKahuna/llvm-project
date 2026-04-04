//===-- Internal scheduler policy/param declarations -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: scheduler functions that implement Linux
// syscall semantics (return 0/value on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SCHED_SCHEDULER_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SCHED_SCHEDULER_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/struct_sched_param.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t sched_getscheduler(pid_t tid);
intptr_t sched_setscheduler(pid_t tid, int policy,
                        const struct sched_param *param);
intptr_t sched_getparam(pid_t tid, struct sched_param *param);
intptr_t sched_setparam(pid_t tid, const struct sched_param *param);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SCHED_SCHEDULER_OPS_H
