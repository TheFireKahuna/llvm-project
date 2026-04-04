//===-- Windows thread scheduling support -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Cross-thread sched_*scheduler/param paths under the new Crystalline-W
// thread registry. The pthread API hands us a `ThreadAttributes *attrib`
// from a live `pthread_t`; the lifecycle is reachable in O(1) via
// `attrib->platform_data` (the back-pointer wired at thread creation).
// No registry lookup, no SMR pin — POSIX-trust on attrib validity makes
// the bypass safe (UB to use a `pthread_t` after pthread_join /
// pthread_detach, same as glibc/musl).
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/sched_support.h"

#include "hdr/errno_macros.h"
#include "hdr/sched_macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/sched/sched_helpers.h"
#include "src/__support/OSUtil/windows/signal/signal_internal.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/thread.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace sched_support {

namespace {

LIBC_INLINE bool is_self_attrib(ThreadAttributes *attrib) {
  return static_cast<DWORD>(attrib->tid) == ::NtCurrentThreadId();
}

// Bypass into the lifecycle from a libc-internal pthread_t. Returns
// nullptr only if the caller passed an attrib that was never wired
// through ThreadLifecycle (foreign threads that never went through
// alloc_lifecycle have no platform_data — cross-thread sched ops on
// them are out of contract).
LIBC_INLINE ThreadLifecycle *lifecycle_from(ThreadAttributes *attrib) {
  return static_cast<ThreadLifecycle *>(attrib->platform_data);
}

LIBC_INLINE int self_get_sched(int *policy, struct sched_param *param) {
  THREAD_BASIC_INFORMATION info;
  NTSTATUS status = ::NtQueryInformationThread(
      ::NtCurrentThread(), ThreadBasicInformation, &info, sizeof(info),
      nullptr);
  if (!NT_SUCCESS(status))
    return ESRCH;

  int sched_policy = SCHED_OTHER;
  auto *self = signal_state::get_thread_state();
  if (self)
    sched_policy = self->sched_policy.load(cpp::MemoryOrder::RELAXED);

  *policy = sched_policy;
  param->sched_priority =
      (sched_policy == SCHED_OTHER)
          ? 0
          : sched_impl::nt_to_posix_priority(info.BasePriority);
  return 0;
}

} // anonymous namespace

int get_thread_sched(ThreadAttributes *attrib, int *policy,
                     struct sched_param *param) {
  if (!attrib || !policy || !param)
    return EINVAL;

  if (is_self_attrib(attrib))
    return self_get_sched(policy, param);

  auto *lc = lifecycle_from(attrib);
  if (!lc)
    return ESRCH;

  // Borrow the registry-owned thread handle — no NtOpenThread round
  // trip in the common case. The lifecycle is alive for the call frame
  // by POSIX-trust on `attrib`.
  HANDLE handle = registry_borrow_handle(lc);
  bool opened = false;
  if (!handle) {
    NTSTATUS open_st = sched_impl::open_thread_by_tid(
        static_cast<DWORD>(attrib->tid), THREAD_QUERY_INFORMATION, &handle);
    if (!NT_SUCCESS(open_st))
      return ESRCH;
    opened = true;
  }

  THREAD_BASIC_INFORMATION info;
  NTSTATUS status = ::NtQueryInformationThread(
      handle, ThreadBasicInformation, &info, sizeof(info), nullptr);
  if (opened)
    ::NtClose(handle);
  if (!NT_SUCCESS(status))
    return ESRCH;

  int sched_policy = SCHED_OTHER;
  if (auto *sig = lc->signal.load(cpp::MemoryOrder::ACQUIRE))
    sched_policy = sig->sched_policy.load(cpp::MemoryOrder::RELAXED);

  *policy = sched_policy;
  param->sched_priority =
      (sched_policy == SCHED_OTHER)
          ? 0
          : sched_impl::nt_to_posix_priority(info.BasePriority);
  return 0;
}

int set_thread_sched(ThreadAttributes *attrib, int policy,
                     const struct sched_param *param) {
  if (!attrib || !param)
    return EINVAL;

  KPRIORITY nt_prio = sched_impl::posix_to_nt_priority(param->sched_priority);

  if (is_self_attrib(attrib)) {
    NTSTATUS status = ::NtSetInformationThread(
        ::NtCurrentThread(), ThreadBasePriority, &nt_prio, sizeof(nt_prio));
    if (!NT_SUCCESS(status))
      return EPERM;

    auto *self = signal_state::get_thread_state();
    if (self)
      self->sched_policy.store(static_cast<int8_t>(policy),
                               cpp::MemoryOrder::RELAXED);
    return 0;
  }

  auto *lc = lifecycle_from(attrib);
  if (!lc)
    return ESRCH;

  HANDLE handle;
  NTSTATUS status = sched_impl::open_thread_by_tid(
      static_cast<DWORD>(attrib->tid), THREAD_SET_INFORMATION, &handle);
  if (!NT_SUCCESS(status))
    return ESRCH;

  status = ::NtSetInformationThread(handle, ThreadBasePriority, &nt_prio,
                                    sizeof(nt_prio));
  ::NtClose(handle);

  if (!NT_SUCCESS(status))
    return EPERM;

  // Record the new POSIX policy in the lifecycle's signal state. The
  // lifecycle is alive for the call frame (POSIX-trust on attrib).
  if (auto *sig = lc->signal.load(cpp::MemoryOrder::ACQUIRE))
    sig->sched_policy.store(static_cast<int8_t>(policy),
                             cpp::MemoryOrder::RELAXED);
  return 0;
}

int set_thread_prio(ThreadAttributes *attrib, int prio) {
  if (!attrib)
    return EINVAL;

  bool self = is_self_attrib(attrib);
  int policy = SCHED_OTHER;
  if (self) {
    auto *st = signal_state::get_thread_state();
    if (st)
      policy = st->sched_policy.load(cpp::MemoryOrder::RELAXED);
  } else {
    auto *lc = lifecycle_from(attrib);
    if (!lc)
      return ESRCH;
    if (auto *sig = lc->signal.load(cpp::MemoryOrder::ACQUIRE))
      policy = sig->sched_policy.load(cpp::MemoryOrder::RELAXED);
  }

  if (policy == SCHED_OTHER) {
    if (prio != 0)
      return EINVAL;
    return 0; // SCHED_OTHER priority is always 0; nothing to change.
  }

  if (prio < 1 || prio > 99)
    return EINVAL;

  KPRIORITY nt_prio = sched_impl::posix_to_nt_priority(prio);
  HANDLE handle = ::NtCurrentThread();
  bool opened = false;

  if (!self) {
    NTSTATUS status = sched_impl::open_thread_by_tid(
        static_cast<DWORD>(attrib->tid), THREAD_SET_INFORMATION, &handle);
    if (!NT_SUCCESS(status))
      return ESRCH;
    opened = true;
  }

  NTSTATUS status = ::NtSetInformationThread(handle, ThreadBasePriority,
                                             &nt_prio, sizeof(nt_prio));
  if (opened)
    ::NtClose(handle);

  return NT_SUCCESS(status) ? 0 : EPERM;
}

} // namespace sched_support
} // namespace LIBC_NAMESPACE_DECL
