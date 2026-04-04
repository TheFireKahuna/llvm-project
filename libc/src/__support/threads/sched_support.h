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

struct ThreadAttributes;

namespace sched_support {

// Get the scheduling policy and parameters of the thread referenced by
// |attrib|. Returns 0 on success, or a POSIX error code (ESRCH, EINVAL).
//
// Identity discipline: the implementation reaches the lifecycle via
// `attrib->platform_data` for libc-internal state, and reads
// `attrib->tid` only when the underlying NT call requires the thread
// id. POSIX-trust applies to `attrib` (UB after pthread_join/detach).
int get_thread_sched(ThreadAttributes *attrib, int *policy,
                     struct sched_param *param);

// Set the scheduling policy and parameters of the thread referenced by
// |attrib|. Returns 0 on success, or a POSIX error code.
int set_thread_sched(ThreadAttributes *attrib, int policy,
                     const struct sched_param *param);

// Set only the priority of the thread referenced by |attrib|, keeping
// the current scheduling policy. Returns 0 on success, or a POSIX error
// code.
int set_thread_prio(ThreadAttributes *attrib, int prio);

} // namespace sched_support
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_SCHED_SUPPORT_H
