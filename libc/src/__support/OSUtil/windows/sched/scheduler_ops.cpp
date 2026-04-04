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
// API contract: takes a raw NT thread id. Cross-thread paths look the
// id up in the thread registry to find the lifecycle that holds the
// per-thread sched_policy. Lookup is an O(N) walk over the iter list
// because the registry's primary index is keyed on `task_id` (which
// callers of these syscalls do not know); for non-self threads this is
// rare-and-cold and not on any hot path.
//
//===----------------------------------------------------------------------===//

#include "scheduler_ops.h"
#include "sched_helpers.h"
#include "hdr/errno_macros.h"
#include "hdr/sched_macros.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/struct_sched_param.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

// Resolve a TID to a lifecycle by walking the iter list. Returned
// pointer is Crystalline-pinned via the iter reservation index for the
// duration of the caller's frame; do not retain past `clear_all`.
LIBC_INLINE ThreadLifecycle *find_lifecycle_by_tid(DWORD tid) {
  return registry_find_if(
      [&](ThreadLifecycle *lc) -> bool { return lc->tid == tid; });
}

// Resolve the per-thread sched_policy atomic for a target tid. Returns
// nullptr (with the "found a lifecycle" outcome encoded in `*found`)
// when the lifecycle has no signal state. For self, returns the
// thread-local state directly.
LIBC_INLINE cpp::Atomic<int8_t> *resolve_sched_policy(bool is_self,
                                                      DWORD target_tid,
                                                      bool *found) {
  if (is_self) {
    *found = true;
    auto *state = signal_state::get_thread_state();
    return state ? &state->sched_policy : nullptr;
  }
  ThreadLifecycle *lc = find_lifecycle_by_tid(target_tid);
  if (!lc) {
    *found = false;
    return nullptr;
  }
  *found = true;
  auto *sig = lc->signal.load(cpp::MemoryOrder::ACQUIRE);
  return sig ? &sig->sched_policy : nullptr;
}

} // anonymous namespace

intptr_t sched_getscheduler(pid_t tid) {
  if (tid < 0)
    return -EINVAL;

  bool is_self =
      (tid == 0 || static_cast<DWORD>(tid) == ::NtCurrentThreadId());

  if (is_self) {
    auto *state = signal_state::get_thread_state();
    return state ? state->sched_policy.load(cpp::MemoryOrder::RELAXED)
                 : SCHED_OTHER;
  }

  ThreadLifecycle *lc = find_lifecycle_by_tid(static_cast<DWORD>(tid));
  if (!lc)
    return -ESRCH;
  if (auto *sig = lc->signal.load(cpp::MemoryOrder::ACQUIRE))
    return sig->sched_policy.load(cpp::MemoryOrder::RELAXED);
  return SCHED_OTHER;
}

intptr_t sched_setscheduler(pid_t tid, int policy,
                            const struct sched_param *param) {
  if (!param || (policy != SCHED_OTHER && policy != SCHED_FIFO &&
                 policy != SCHED_RR))
    return -EINVAL;

  if (policy == SCHED_OTHER) {
    if (param->sched_priority != 0)
      return -EINVAL;
  } else {
    if (param->sched_priority < 1 || param->sched_priority > 99)
      return -EINVAL;
  }

  bool is_self =
      (tid == 0 || static_cast<DWORD>(tid) == ::NtCurrentThreadId());

  bool found = false;
  cpp::Atomic<int8_t> *policy_atom =
      resolve_sched_policy(is_self, static_cast<DWORD>(tid), &found);
  if (!is_self && !found)
    return -ESRCH;

  int prev_policy = policy_atom
                        ? policy_atom->load(cpp::MemoryOrder::RELAXED)
                        : SCHED_OTHER;

  KPRIORITY nt_prio = sched_impl::posix_to_nt_priority(param->sched_priority);
  HANDLE raw_handle = ::NtCurrentThread();
  windows::ScopedNtHandle owned;

  if (!is_self) {
    NTSTATUS status = sched_impl::open_thread_by_tid(
        static_cast<DWORD>(tid), THREAD_SET_INFORMATION, owned.put());
    if (!NT_SUCCESS(status))
      return -ESRCH;
    raw_handle = owned.get();
  }

  NTSTATUS status = ::NtSetInformationThread(
      raw_handle, ThreadBasePriority, &nt_prio, sizeof(nt_prio));
  if (!NT_SUCCESS(status))
    return -EPERM;

  if (policy_atom)
    policy_atom->store(static_cast<int8_t>(policy),
                       cpp::MemoryOrder::RELEASE);

  return static_cast<intptr_t>(prev_policy);
}

intptr_t sched_getparam(pid_t tid, struct sched_param *param) {
  if (!param)
    return -EINVAL;

  bool is_self =
      (tid == 0 || static_cast<DWORD>(tid) == ::NtCurrentThreadId());

  HANDLE raw_handle = ::NtCurrentThread();
  windows::ScopedNtHandle owned;

  if (!is_self) {
    NTSTATUS status = sched_impl::open_thread_by_tid(
        static_cast<DWORD>(tid), THREAD_QUERY_INFORMATION, owned.put());
    if (!NT_SUCCESS(status))
      return -ESRCH;
    raw_handle = owned.get();
  }

  THREAD_BASIC_INFORMATION info;
  NTSTATUS status = ::NtQueryInformationThread(
      raw_handle, ThreadBasicInformation, &info, sizeof(info), nullptr);
  if (!NT_SUCCESS(status))
    return -ESRCH;

  bool found = false;
  cpp::Atomic<int8_t> *policy_atom =
      resolve_sched_policy(is_self, static_cast<DWORD>(tid), &found);
  int policy =
      policy_atom ? policy_atom->load(cpp::MemoryOrder::RELAXED) : SCHED_OTHER;

  param->sched_priority =
      (policy == SCHED_OTHER)
          ? 0
          : sched_impl::nt_to_posix_priority(info.BasePriority);
  return 0;
}

intptr_t sched_setparam(pid_t tid, const struct sched_param *param) {
  if (!param)
    return -EINVAL;

  bool is_self =
      (tid == 0 || static_cast<DWORD>(tid) == ::NtCurrentThreadId());

  bool found = false;
  cpp::Atomic<int8_t> *policy_atom =
      resolve_sched_policy(is_self, static_cast<DWORD>(tid), &found);
  if (!is_self && !found)
    return -ESRCH;

  int policy =
      policy_atom ? policy_atom->load(cpp::MemoryOrder::RELAXED) : SCHED_OTHER;

  if (policy == SCHED_OTHER) {
    if (param->sched_priority != 0)
      return -EINVAL;
    return 0; // SCHED_OTHER priority is always 0; nothing to change.
  }

  if (param->sched_priority < 1 || param->sched_priority > 99)
    return -EINVAL;

  KPRIORITY nt_prio = sched_impl::posix_to_nt_priority(param->sched_priority);
  HANDLE raw_handle = ::NtCurrentThread();
  windows::ScopedNtHandle owned;

  if (!is_self) {
    NTSTATUS status = sched_impl::open_thread_by_tid(
        static_cast<DWORD>(tid), THREAD_SET_INFORMATION, owned.put());
    if (!NT_SUCCESS(status))
      return -ESRCH;
    raw_handle = owned.get();
  }

  NTSTATUS status = ::NtSetInformationThread(
      raw_handle, ThreadBasePriority, &nt_prio, sizeof(nt_prio));
  if (!NT_SUCCESS(status))
    return -EPERM;
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
