//===-- Robust mutex cleanup for dying threads --------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_ROBUST_LIST_CLEANUP_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_ROBUST_LIST_CLEANUP_H

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace robust_mutex {

// Walk the lifecycle-owned robust record list for a dying thread and mark
// all held mutexes as OWNER_DIED. Called from the TLS cleanup callback.
// Takes a void* to break the header dependency on thread_lifecycle.h;
// the implementation casts to ThreadLifecycle*.
void robust_list_cleanup(void *lifecycle_ptr);

} // namespace robust_mutex
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_ROBUST_LIST_CLEANUP_H
