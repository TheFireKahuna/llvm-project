//===-- Windows internal scheduler policy/param operations -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal kernel functions for sched_getscheduler, sched_setscheduler,
// sched_getparam, and sched_setparam on Windows. These implement Linux syscall
// semantics: 0/value on success, -errno on failure. Called from
// windows_syscalls:: wrappers.
//
//===----------------------------------------------------------------------===//

#include "scheduler_ops.h"
#include "sched_helpers.h"
#include "hdr/errno_macros.h"
#include "hdr/sched_macros.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/struct_sched_param.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/thread_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t sched_getscheduler(pid_t tid) {
  if (tid < 0)
    return -EINVAL;

  // tid == 0 means the calling thread (POSIX convention).
  if (tid == 0 || static_cast<DWORD>(tid) == ::NtCurrentThreadId()) {
    auto *state = signal_state::get_thread_state();
    return state ? state->sched_policy.load(cpp::MemoryOrder::RELAXED)
                 : SCHED_OTHER;
  }

  EpochGuard guard;
  auto *lc = guard.find(static_cast<DWORD>(tid));
  if (!lc)
    return -ESRCH;
  if (lc->signal)
    return lc->signal->sched_policy.load(cpp::MemoryOrder::RELAXED);
  return SCHED_OTHER;
}

intptr_t sched_setscheduler(pid_t tid, int policy,
                        const struct sched_param *param) {
  if (!param || (policy != SCHED_OTHER && policy != SCHED_FIFO &&
                 policy != SCHED_RR))
    return -EINVAL;

  // Validate priority range for the requested policy.
  if (policy == SCHED_OTHER) {
    if (param->sched_priority != 0)
      return -EINVAL;
  } else {
    if (param->sched_priority < 1 || param->sched_priority > 99)
      return -EINVAL;
  }

  bool is_self = (tid == 0 ||
                  static_cast<DWORD>(tid) == ::NtCurrentThreadId());

  // Read the previous policy before changing it. POSIX requires returning it.
  int prev_policy = SCHED_OTHER;
  if (is_self) {
    auto *self = signal_state::get_thread_state();
    if (self)
      prev_policy = self->sched_policy.load(cpp::MemoryOrder::RELAXED);
  } else {
    EpochGuard guard;
    auto *lc = guard.find(static_cast<DWORD>(tid));
    if (!lc)
      return -ESRCH;
    if (lc->signal)
      prev_policy = lc->signal->sched_policy.load(cpp::MemoryOrder::RELAXED);
  }

  // Set the NT thread base priority.
  KPRIORITY nt_prio = sched_impl::posix_to_nt_priority(param->sched_priority);
  HANDLE handle = ::NtCurrentThread();
  bool opened = false;

  if (!is_self) {
    NTSTATUS status = sched_impl::open_thread_by_tid(
        static_cast<DWORD>(tid), THREAD_SET_INFORMATION, &handle);
    if (!NT_SUCCESS(status))
      return -ESRCH;
    opened = true;
  }

  NTSTATUS status = ::NtSetInformationThread(
      handle, ThreadBasePriority, &nt_prio, sizeof(nt_prio));

  if (opened)
    ::NtClose(handle);

  if (!NT_SUCCESS(status))
    return -EPERM;

  // Record the new POSIX policy. Windows has no scheduler policy concept --
  // we track it per-thread in the signal state.
  if (is_self) {
    auto *self = signal_state::get_thread_state();
    if (self)
      self->sched_policy.store(static_cast<int8_t>(policy),
                               cpp::MemoryOrder::RELEASE);
  } else {
    EpochGuard guard;
    if (auto *lc = guard.find(static_cast<DWORD>(tid))) {
      if (lc->signal)
        lc->signal->sched_policy.store(static_cast<int8_t>(policy),
                                       cpp::MemoryOrder::RELEASE);
    }
  }

  // POSIX: return the previous scheduling policy on success.
  return static_cast<intptr_t>(prev_policy);
}

intptr_t sched_getparam(pid_t tid, struct sched_param *param) {
  if (!param)
    return -EINVAL;

  // Use the pseudo-handle for the current thread, avoiding NtOpenThread.
  HANDLE handle = ::NtCurrentThread();
  bool opened = false;

  if (tid != 0 && static_cast<DWORD>(tid) != ::NtCurrentThreadId()) {
    NTSTATUS status = sched_impl::open_thread_by_tid(
        static_cast<DWORD>(tid), THREAD_QUERY_INFORMATION, &handle);
    if (!NT_SUCCESS(status))
      return -ESRCH;
    opened = true;
  }

  THREAD_BASIC_INFORMATION info;
  NTSTATUS status = ::NtQueryInformationThread(
      handle, ThreadBasicInformation, &info, sizeof(info), nullptr);

  if (opened)
    ::NtClose(handle);

  if (!NT_SUCCESS(status))
    return -ESRCH;

  // Check whether this thread uses a realtime policy.
  bool is_self = (tid == 0 ||
                  static_cast<DWORD>(tid) == ::NtCurrentThreadId());
  int policy = SCHED_OTHER;
  if (is_self) {
    auto *self = signal_state::get_thread_state();
    if (self)
      policy = self->sched_policy.load(cpp::MemoryOrder::RELAXED);
  } else {
    EpochGuard guard;
    if (auto *lc = guard.find(static_cast<DWORD>(tid))) {
      if (lc->signal)
        policy = lc->signal->sched_policy.load(cpp::MemoryOrder::RELAXED);
    }
  }

  if (policy == SCHED_OTHER) {
    param->sched_priority = 0;
  } else {
    param->sched_priority = sched_impl::nt_to_posix_priority(info.BasePriority);
  }
  return 0;
}

intptr_t sched_setparam(pid_t tid, const struct sched_param *param) {
  if (!param)
    return -EINVAL;

  // Look up the current policy to validate the priority range.
  bool is_self = (tid == 0 ||
                  static_cast<DWORD>(tid) == ::NtCurrentThreadId());
  int policy = SCHED_OTHER;
  if (is_self) {
    auto *self = signal_state::get_thread_state();
    if (self)
      policy = self->sched_policy.load(cpp::MemoryOrder::RELAXED);
  } else {
    EpochGuard guard;
    if (auto *lc = guard.find(static_cast<DWORD>(tid))) {
      if (lc->signal)
        policy = lc->signal->sched_policy.load(cpp::MemoryOrder::RELAXED);
    }
  }

  if (policy == SCHED_OTHER) {
    if (param->sched_priority != 0)
      return -EINVAL;
    return 0; // SCHED_OTHER priority is always 0; nothing to change.
  }

  if (param->sched_priority < 1 || param->sched_priority > 99)
    return -EINVAL;

  KPRIORITY nt_prio = sched_impl::posix_to_nt_priority(param->sched_priority);
  HANDLE handle = ::NtCurrentThread();
  bool opened = false;

  if (!is_self) {
    NTSTATUS status = sched_impl::open_thread_by_tid(
        static_cast<DWORD>(tid), THREAD_SET_INFORMATION, &handle);
    if (!NT_SUCCESS(status))
      return -ESRCH;
    opened = true;
  }

  NTSTATUS status = ::NtSetInformationThread(
      handle, ThreadBasePriority, &nt_prio, sizeof(nt_prio));

  if (opened)
    ::NtClose(handle);

  if (!NT_SUCCESS(status))
    return -EPERM;
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
