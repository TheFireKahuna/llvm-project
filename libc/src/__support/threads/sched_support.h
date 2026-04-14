//===-- Platform-neutral thread scheduling support -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Platform-neutral interface for pthread_getschedparam, pthread_setschedparam,
// and pthread_setschedprio. Each platform implements these in terms of its
// native scheduling APIs.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_SCHED_SUPPORT_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_SCHED_SUPPORT_H

#include "src/__support/macros/config.h"

#include <sched.h> // struct sched_param

namespace LIBC_NAMESPACE_DECL {
namespace sched_support {

// Get the scheduling policy and parameters of a thread identified by |tid|.
// Returns 0 on success, or a POSIX error code (ESRCH, EINVAL, etc.).
int get_thread_sched(int tid, int *policy, struct sched_param *param);

// Set the scheduling policy and parameters of a thread identified by |tid|.
// Returns 0 on success, or a POSIX error code (ESRCH, EINVAL, EPERM, etc.).
int set_thread_sched(int tid, int policy, const struct sched_param *param);

// Set only the priority of a thread identified by |tid|, keeping the current
// scheduling policy. Returns 0 on success, or a POSIX error code.
int set_thread_prio(int tid, int prio);

} // namespace sched_support
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_SCHED_SUPPORT_H
