//===-- windows_syscalls::getppid() wrapper ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETPPID_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETPPID_H

#include "hdr/types/pid_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include "hdr/errno_macros.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

LIBC_INLINE ErrorOr<pid_t> getppid() {
  // Parent PID never changes. PID 0 (System Idle Process) can never be a
  // parent, so it serves as the "not yet queried" sentinel.
  static cpp::Atomic<pid_t> cached_ppid{0};
  pid_t ppid = cached_ppid.load(cpp::MemoryOrder::RELAXED);
  if (ppid != 0)
    return ppid;
  PROCESS_BASIC_INFORMATION pbi;
  NTSTATUS status = ::NtQueryInformationProcess(
      NtCurrentProcess(), ProcessBasicInformation, &pbi, sizeof(pbi), nullptr);
  if (!NT_SUCCESS(status))
    return Error(EIO);
  ppid = static_cast<pid_t>(pbi.InheritedFromUniqueProcessId);
  cached_ppid.store(ppid, cpp::MemoryOrder::RELAXED);
  return ppid;
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETPPID_H
