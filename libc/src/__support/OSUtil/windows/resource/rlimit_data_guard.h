//===-- Public-call RLIMIT_DATA guard for Windows ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RLIMIT_DATA on modern Linux constrains public memory growth paths including
// mmap(2). On Windows we implement that policy as a public-call scope:
// public entry points opt in, and lower layers consult the current private
// commit charge before performing growth.
//
// This keeps unrelated internal background allocations exempt while still
// covering real user-visible growth triggered by malloc, mmap, pthread_create,
// and similar APIs.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RLIMIT_DATA_GUARD_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RLIMIT_DATA_GUARD_H

#include <stddef.h>

#include "src/__support/OSUtil/windows/resource/rlimit_state.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

inline thread_local unsigned rlimit_data_public_scope_depth = 0;

class ScopedRlimitDataPublicCall {
public:
  ScopedRlimitDataPublicCall() { ++rlimit_data_public_scope_depth; }
  ~ScopedRlimitDataPublicCall() { --rlimit_data_public_scope_depth; }

  ScopedRlimitDataPublicCall(const ScopedRlimitDataPublicCall &) = delete;
  ScopedRlimitDataPublicCall &
  operator=(const ScopedRlimitDataPublicCall &) = delete;
};

LIBC_INLINE bool rlimit_data_public_scope_active() {
  return rlimit_data_public_scope_depth != 0;
}

// Best-effort admission control using the kernel's current private commit
// charge. VM_COUNTERS::PagefileUsage is the process private commit metric,
// equivalent to PrivateUsage in VM_COUNTERS_EX.
LIBC_INLINE bool allows_public_rlimit_data_growth(size_t growth) {
  if (growth == 0 || !rlimit_data_public_scope_active())
    return true;

  ensure_rlimit_init();

  rlim_t limit = g_pcb.rlimit.limits[RLIMIT_DATA].rlim_cur;
  if (limit == RLIM_INFINITY)
    return true;

  if (growth > static_cast<size_t>(limit))
    return false;

  VM_COUNTERS vmc = {};
  NTSTATUS st = ::NtQueryInformationProcess(NtCurrentProcess(),
                                            ProcessVmCounters, &vmc,
                                            sizeof(vmc), nullptr);
  if (!NT_SUCCESS(st))
    return true;

  rlim_t current = static_cast<rlim_t>(vmc.PagefileUsage);
  return current <= limit - static_cast<rlim_t>(growth);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RLIMIT_DATA_GUARD_H
