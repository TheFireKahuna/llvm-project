//===-- NT process query utilities ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin helpers over NtQueryInformationProcess for process-level metadata.
// Used by vt_pty.cpp and pty_tree.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_NT_PROCESS_UTILS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_NT_PROCESS_UTILS_H

#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace process_util {

// Returns the creation timestamp of the current process in 100ns NT epoch
// units (ticks since 1601-01-01).  Returns 0 on failure.
LIBC_INLINE uint64_t current_process_create_time() {
  KERNEL_USER_TIMES times = {};
  NTSTATUS status = ::NtQueryInformationProcess(
      NtCurrentProcess(), ProcessTimes, &times, sizeof(times), nullptr);
  if (!NT_SUCCESS(status))
    return 0;
  return static_cast<uint64_t>(times.CreateTime.QuadPart);
}

LIBC_INLINE uint64_t query_process_create_time(HANDLE process) {
  if (!process)
    return 0;
  KERNEL_USER_TIMES times = {};
  NTSTATUS status = ::NtQueryInformationProcess(
      process, ProcessTimes, &times, sizeof(times), nullptr);
  if (!NT_SUCCESS(status))
    return 0;
  return static_cast<uint64_t>(times.CreateTime.QuadPart);
}

// Returns true when the given process handle has already exited (zero-timeout
// wait succeeds).  Returns false for null handles or still-running processes.
LIBC_INLINE bool process_has_exited(HANDLE process) {
  if (!process)
    return false;
  LARGE_INTEGER zero_timeout = {};
  return ::NtWaitForSingleObject(process, FALSE, &zero_timeout) ==
         STATUS_SUCCESS;
}

} // namespace process_util
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_NT_PROCESS_UTILS_H
