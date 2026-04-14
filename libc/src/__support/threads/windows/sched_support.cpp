//===-- Windows thread scheduling support -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/sched_support.h"

#include "hdr/errno_macros.h"
#include "hdr/sched_macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/sched/sched_helpers.h"
#include "src/__support/OSUtil/windows/signal/signal_internal.h"
#include "src/__support/threads/windows/thread_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace sched_support {

int get_thread_sched(int tid, int *policy, struct sched_param *param) {
  DWORD dtid = static_cast<DWORD>(tid);
  bool is_self = (dtid == ::NtCurrentThreadId());

  // Query NT thread base priority. For remote threads, prefer borrowing the
  // handle from the registry (avoids NtOpenThread + NtClose round-trip).
  // The EpochGuard must span the borrow AND the query — the epoch pin keeps
  // the lifecycle (and its handle) alive for the duration.
  HANDLE handle = ::NtCurrentThread();
  bool opened = false;
  THREAD_BASIC_INFORMATION info;
  NTSTATUS status;

  if (!is_self) {
    EpochGuard guard;
    if (auto *lc = guard.find(dtid)) {
      HANDLE borrowed = registry_borrow_handle(lc);
      if (borrowed) {
        handle = borrowed;
      } else {
        NTSTATUS open_st = sched_impl::open_thread_by_tid(
            dtid, THREAD_QUERY_INFORMATION, &handle);
        if (!NT_SUCCESS(open_st))
          return ESRCH;
        opened = true;
      }
    } else {
      NTSTATUS open_st = sched_impl::open_thread_by_tid(
          dtid, THREAD_QUERY_INFORMATION, &handle);
      if (!NT_SUCCESS(open_st))
        return ESRCH;
      opened = true;
    }

    // Query while the epoch pin keeps the borrowed handle alive.
    status = ::NtQueryInformationThread(
        handle, ThreadBasicInformation, &info, sizeof(info), nullptr);

    if (opened)
      ::NtClose(handle);
    // guard released here — epoch pin no longer needed.
  } else {
    status = ::NtQueryInformationThread(
        handle, ThreadBasicInformation, &info, sizeof(info), nullptr);
  }

  if (!NT_SUCCESS(status))
    return ESRCH;

  // Read policy from per-thread signal state.
  int sched_policy = SCHED_OTHER;
  if (is_self) {
    auto *self = signal_state::get_thread_state();
    if (self)
      sched_policy = self->sched_policy.load(cpp::MemoryOrder::RELAXED);
  } else {
    EpochGuard guard;
    if (auto *lc = guard.find(dtid)) {
      if (lc->signal)
        sched_policy = lc->signal->sched_policy.load(cpp::MemoryOrder::RELAXED);
    }
  }

  *policy = sched_policy;
  if (sched_policy == SCHED_OTHER) {
    param->sched_priority = 0;
  } else {
    param->sched_priority = sched_impl::nt_to_posix_priority(info.BasePriority);
  }

  return 0;
}

int set_thread_sched(int tid, int policy, const struct sched_param *param) {
  DWORD dtid = static_cast<DWORD>(tid);
  bool is_self = (dtid == ::NtCurrentThreadId());

  // Set NT thread base priority.
  KPRIORITY nt_prio = sched_impl::posix_to_nt_priority(param->sched_priority);
  HANDLE handle = ::NtCurrentThread();
  bool opened = false;

  if (!is_self) {
    NTSTATUS status = sched_impl::open_thread_by_tid(
        dtid, THREAD_SET_INFORMATION, &handle);
    if (!NT_SUCCESS(status))
      return ESRCH;
    opened = true;
  }

  NTSTATUS status = ::NtSetInformationThread(
      handle, ThreadBasePriority, &nt_prio, sizeof(nt_prio));

  if (opened)
    ::NtClose(handle);

  if (!NT_SUCCESS(status))
    return EPERM;

  // Record policy in per-thread signal state.
  if (is_self) {
    auto *self = signal_state::get_thread_state();
    if (self)
      self->sched_policy.store(static_cast<int8_t>(policy),
                               cpp::MemoryOrder::RELAXED);
  } else {
    EpochGuard guard;
    if (auto *lc = guard.find(dtid)) {
      if (lc->signal)
        lc->signal->sched_policy.store(static_cast<int8_t>(policy),
                                       cpp::MemoryOrder::RELAXED);
    }
  }

  return 0;
}

int set_thread_prio(int tid, int prio) {
  DWORD dtid = static_cast<DWORD>(tid);
  bool is_self = (dtid == ::NtCurrentThreadId());

  // Read current policy to validate the priority range.
  int policy = SCHED_OTHER;
  if (is_self) {
    auto *self = signal_state::get_thread_state();
    if (self)
      policy = self->sched_policy.load(cpp::MemoryOrder::RELAXED);
  } else {
    EpochGuard guard;
    if (auto *lc = guard.find(dtid)) {
      if (lc->signal)
        policy = lc->signal->sched_policy.load(cpp::MemoryOrder::RELAXED);
    }
  }

  if (policy == SCHED_OTHER) {
    if (prio != 0)
      return EINVAL;
    return 0; // Nothing to change.
  }

  if (prio < 1 || prio > 99)
    return EINVAL;

  KPRIORITY nt_prio = sched_impl::posix_to_nt_priority(prio);
  HANDLE handle = ::NtCurrentThread();
  bool opened = false;

  if (!is_self) {
    NTSTATUS status = sched_impl::open_thread_by_tid(
        dtid, THREAD_SET_INFORMATION, &handle);
    if (!NT_SUCCESS(status))
      return ESRCH;
    opened = true;
  }

  NTSTATUS status = ::NtSetInformationThread(
      handle, ThreadBasePriority, &nt_prio, sizeof(nt_prio));

  if (opened)
    ::NtClose(handle);

  return NT_SUCCESS(status) ? 0 : EPERM;
}

} // namespace sched_support
} // namespace LIBC_NAMESPACE_DECL
