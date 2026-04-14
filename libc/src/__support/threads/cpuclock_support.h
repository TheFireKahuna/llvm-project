//===-- Platform-neutral thread CPU clock support ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Provides the platform hook for pthread_getcpuclockid(). The public pthread
// entrypoint extracts the thread identity from pthread_t and delegates the
// OS-specific clock-id mapping here.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_CPUCLOCK_SUPPORT_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_CPUCLOCK_SUPPORT_H

#include "hdr/types/clockid_t.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace cpuclock_support {

// Returns 0 on success and stores a clock id that can be passed to
// clock_gettime/clock_getres for the target thread. Returns a POSIX error code
// such as ESRCH or EINVAL on failure.
int get_thread_cpuclockid(int tid, clockid_t *clock_id);

} // namespace cpuclock_support
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_CPUCLOCK_SUPPORT_H
