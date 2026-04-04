//===-- windows_syscalls::getsid/setsid wrapper ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX session ID management.
//
// The POSIX "session" concept (setsid/getsid) has no direct NT kernel
// equivalent. We maintain a process-global session ID:
//
//   - Initialized at startup to the parent's ProcessGroupId (the closest
//     NT analog to a session/process-group leader).
//   - Updated by setsid() to the calling process's own PID.
//   - getsid(0) and getsid(getpid()) return the local session ID.
//   - getsid(other_pid) returns EPERM — cross-process session queries
//     would require shared-memory infrastructure not yet implemented.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETSID_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETSID_H

#include "hdr/errno_macros.h"
#include "hdr/types/pid_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process/console.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/__support/process/windows/child_table.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

// Process-global session ID. Initialized lazily from PEB on first access.
// Updated atomically by setsid().
inline cpp::Atomic<pid_t> g_session_id{0};
inline cpp::Atomic<bool> g_session_id_initialized{false};

LIBC_INLINE pid_t get_initial_session_id() {
  auto *params = NtCurrentPeb()->ProcessParameters;
  if (params)
    return static_cast<pid_t>(params->ProcessGroupId);
  return static_cast<pid_t>(NtCurrentProcessId());
}

LIBC_INLINE pid_t get_session_id() {
  if (!g_session_id_initialized.load(cpp::MemoryOrder::ACQUIRE)) {
    g_session_id.store(get_initial_session_id(), cpp::MemoryOrder::RELAXED);
    g_session_id_initialized.store(true, cpp::MemoryOrder::RELEASE);
  }
  return g_session_id.load(cpp::MemoryOrder::RELAXED);
}

LIBC_INLINE ErrorOr<pid_t> getsid(pid_t pid) {
  pid_t self = static_cast<pid_t>(NtCurrentProcessId());
  if (pid == 0 || pid == self)
    return get_session_id();
  // Cross-process session query not yet supported.
  return Error(EPERM);
}

LIBC_INLINE ErrorOr<pid_t> setsid() {
  pid_t self = static_cast<pid_t>(NtCurrentProcessId());
  auto *params = NtCurrentPeb()->ProcessParameters;

  // POSIX: EPERM if already a process group leader.
  if (params && static_cast<pid_t>(params->ProcessGroupId) == self)
    return Error(EPERM);

  // Become session leader: session ID = our PID.
  g_session_id.store(self, cpp::MemoryOrder::RELAXED);
  g_session_id_initialized.store(true, cpp::MemoryOrder::RELEASE);

  // Become process group leader — update both PEB and our pgid tracking.
  if (params)
    params->ProcessGroupId = static_cast<ULONG>(self);
  process::set_self_pgid(self);

  // Detach from controlling terminal (console).
  (void)console::disconnect();

  return self;
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETSID_H
