//===-- Internal sched affinity declarations --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: CPU affinity functions that implement
// Linux syscall semantics (return 0 on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SCHED_AFFINITY_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SCHED_AFFINITY_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/cpu_set_t.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/size_t.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t sched_getaffinity(pid_t tid, size_t cpuset_size, cpu_set_t *mask);
intptr_t sched_setaffinity(pid_t tid, size_t cpuset_size, const cpu_set_t *mask);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SCHED_AFFINITY_OPS_H
