//===-- Windows mutex support ------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thread death detection for robust mutexes and per-thread intrusive robust
// list for TLS-based proactive cleanup on thread exit.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_MUTEX_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_MUTEX_H

#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_word.h"

namespace LIBC_NAMESPACE_DECL {

class Futex;
struct RobustRecord; // Defined in mutex.cpp / thread_lifecycle.h.
struct ThreadLifecycle;

namespace robust_mutex {

// Check whether the thread that owns a robust mutex (identified by task_id
// stored in the futex word) is dead. Walks the thread registry to find the
// thread's cached handle, then probes it with NtWaitForSingleObject.
bool is_owner_dead(FutexValueType owner_task_id);

// Link/unlink a robust mutex record in the calling thread's lifecycle-owned
// robust list. O(1) operations. The mutex itself stores only a pointer-sized
// handle to its sidecar record.
void robust_list_add(Futex *futex_word, RobustRecord **record_slot);
void robust_list_remove(RobustRecord **record_slot);

// Release all robust sidecar storage owned by the lifecycle.
void lifecycle_destroy(ThreadLifecycle *lc);

// Return the current thread's task_id for robust mutex ownership encoding.
// Attaches a foreign thread to lifecycle tracking on first use when possible.
FutexValueType get_robust_owner_id();

} // namespace robust_mutex
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_MUTEX_H
