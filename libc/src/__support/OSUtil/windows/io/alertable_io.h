//===--- Alertable I/O helpers for Windows POSIX signals ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RAII guard for SyscallFrame lifecycle linkage and an alertable wait helper
// with SA_RESTART retry logic. Eliminates duplicated boilerplate across
// sync NtRead/NtWriteFile paths and IO Ring CQE wait paths.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALERTABLE_IO_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALERTABLE_IO_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/OSUtil/windows/signal/syscall_frame.h"

namespace LIBC_NAMESPACE_DECL {

// RAII guard that links a SyscallFrame into the current thread's
// active_syscall chain on construction and unlinks it on destruction.
// This allows the signal delivery mechanism to find and cancel in-flight
// I/O operations when delivering signals to the thread.
class SyscallFrameGuard {
  ThreadLifecycle *lc_;
  signal_state::SyscallFrame frame_;

public:
  LIBC_INLINE SyscallFrameGuard(HANDLE file_handle,
                                ioring::RingState *ring = nullptr,
                                HANDLE event = nullptr,
                                ULONGLONG tag = 0,
                                uint32_t batch_count = 0)
      : lc_(get_current_lifecycle()) {
    frame_.prev =
        lc_ ? lc_->active_syscall.load(cpp::MemoryOrder::RELAXED) : nullptr;
    frame_.ring = ring;
    frame_.file_handle = file_handle;
    frame_.event = event;
    frame_.op_tag = tag;
    frame_.batch_count = batch_count;
    if (lc_)
      lc_->active_syscall.store(&frame_, cpp::MemoryOrder::RELEASE);
  }

  LIBC_INLINE ~SyscallFrameGuard() {
    if (lc_)
      lc_->active_syscall.store(frame_.prev, cpp::MemoryOrder::RELEASE);
  }

  SyscallFrameGuard(const SyscallFrameGuard &) = delete;
  SyscallFrameGuard &operator=(const SyscallFrameGuard &) = delete;
};

// Alertable wait on an event handle with SA_RESTART retry logic.
//
// Waits on |event| in alertable mode. If an APC fires (signal delivery):
//   - If SA_RESTART is set for the interrupting signal, the wait retries.
//   - Otherwise, returns STATUS_USER_APC (caller should return EINTR).
//
// On successful completion, returns STATUS_SUCCESS (caller should proceed
// to pop CQEs or read the IO_STATUS_BLOCK).
LIBC_INLINE NTSTATUS alertable_wait_restartable(HANDLE event) {
  for (;;) {
    NTSTATUS status =
        ::NtWaitForSingleObject(event, /*Alertable=*/1, /*Timeout=*/nullptr);
    if (status == STATUS_USER_APC || status == STATUS_ALERTED) {
      if (signal_state::should_restart_syscall())
        continue;
      return STATUS_USER_APC;
    }
    return status;
  }
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALERTABLE_IO_H
