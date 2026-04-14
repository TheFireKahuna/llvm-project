//===-- RLIMIT_CPU soft limit via job IOCP notifications --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RLIMIT_CPU soft limit enforcement using NT job object IOCP notifications.
//
// When setrlimit(RLIMIT_CPU) sets a finite soft limit:
//   1. The self-job's EndOfJobTimeAction is set to POST_AT_END_OF_JOB
//   2. apply_job_limits() sets PerJobUserTimeLimit to rlim_cur (soft limit)
//   3. The job is associated with the reactor's IOCP via watch_job()
//   4. When the kernel posts JOB_OBJECT_MSG_END_OF_JOB_TIME, the reactor
//      callback delivers SIGXCPU and re-arms with a 1-second limit
//   5. The hard limit (rlim_max) uses PerProcessUserTimeLimit which
//      terminates the process (kernel-enforced backstop)
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_CPU_LIMIT_TIMER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_CPU_LIMIT_TIMER_H

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Set up or update the CPU soft limit IOCP notification.
// Called from setrlimit/prlimit after updating cached limits and calling
// apply_job_limits(). Ensures the job is associated with the reactor IOCP
// and that EndOfJobTimeAction is POST_AT_END_OF_JOB.
// Resets cpu_soft_rearming to false (fresh limit from the application).
void cpu_limit_timer_update();

// Reset all CPU limit timer state. Called from fork reinit — the child
// process starts with fresh CPU accounting.
void cpu_limit_timer_reset();

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_CPU_LIMIT_TIMER_H
