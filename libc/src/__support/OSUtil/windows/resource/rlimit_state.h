//===-- Resource limit helpers using PCB rlimit fields -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Process-global state for POSIX resource limits on Windows.
//
// All rlimit state now lives in the ProcessControlBlock (g_pcb). This header
// provides the inline helper functions that operate on those PCB fields.
//
// Limits that require kernel enforcement (RLIMIT_CPU, RLIMIT_AS, RLIMIT_NPROC,
// RLIMIT_RSS) are backed by a lazily-created self-job object. The self-job is
// created on the first setrlimit call that needs enforcement, and the current
// process is assigned to it. On Win11, this creates a nested child job if the
// process is already in a job (e.g., CI sandbox), with most-restrictive-wins
// semantics.
//
// Limits without a kernel analog (RLIMIT_NOFILE, RLIMIT_STACK, RLIMIT_FSIZE,
// etc.) are cached in-process and returned by getrlimit. RLIMIT_NOFILE is
// enforced at the fd table layer; RLIMIT_STACK affects future thread creation.
//
// Children spawned via posix_spawn automatically join the self-job (no
// breakaway flags), inheriting all enforced limits -- matching POSIX semantics.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RLIMIT_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RLIMIT_STATE_H

#include "hdr/types/struct_rlimit.h"
#include "include/llvm-libc-macros/sys-resource-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/nt/nt_job.h"
#include "src/__support/OSUtil/windows/nt/nt_process.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

inline constexpr int RLIMIT_INIT_UNINITIALIZED = 0;
inline constexpr int RLIMIT_INIT_IN_PROGRESS = 1;
inline constexpr int RLIMIT_INIT_READY = 2;

// One-time initialization of the cached limits to RLIM_INFINITY.
inline void ensure_rlimit_init() {
  for (;;) {
    int state = g_pcb.rlimit.initialized.load(cpp::MemoryOrder::ACQUIRE);
    if (state == RLIMIT_INIT_READY)
      return;

    if (state == RLIMIT_INIT_UNINITIALIZED) {
      int expected = RLIMIT_INIT_UNINITIALIZED;
      if (!g_pcb.rlimit.initialized.compare_exchange_strong(
              expected, RLIMIT_INIT_IN_PROGRESS, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::ACQUIRE))
        continue;

      // PCB is constinit demand-zero, so rlimit_job and
      // rlimit_active_flags are already nullptr/0. Only the limits array
      // needs explicit init since RLIM_INFINITY != 0.
      for (int i = 0; i < RLIMIT_PROCESS_COUNT; ++i) {
        g_pcb.rlimit.limits[i].rlim_cur = RLIM_INFINITY;
        g_pcb.rlimit.limits[i].rlim_max = RLIM_INFINITY;
      }

      g_pcb.rlimit.initialized.store(RLIMIT_INIT_READY,
                                     cpp::MemoryOrder::RELEASE);
      futex_addr::wake(&g_pcb.rlimit.initialized, static_cast<uint32_t>(-1));
      return;
    }

    futex_addr::wait(&g_pcb.rlimit.initialized, RLIMIT_INIT_IN_PROGRESS,
                     nullptr);
  }
}

inline void rlimit_fork_reinit() {
  int init_state = g_pcb.rlimit.initialized.load(cpp::MemoryOrder::ACQUIRE);
  if (init_state == RLIMIT_INIT_IN_PROGRESS) {
    g_pcb.rlimit.job = nullptr;
    g_pcb.rlimit.active_flags = 0;
    g_pcb.rlimit.initialized.store(RLIMIT_INIT_UNINITIALIZED,
                                   cpp::MemoryOrder::RELAXED);
    return;
  }

  if (init_state != RLIMIT_INIT_READY)
    return;

  // In the fork child the cached job handle is stale and must never be reused.
  g_pcb.rlimit.job = nullptr;
  g_pcb.rlimit.active_flags = 0;
}

// Create the self-job and assign the current process to it.
// Returns true on success. Idempotent -- second call is a no-op.
inline bool ensure_self_job() {
  ensure_rlimit_init();

  if (g_pcb.rlimit.job != nullptr)
    return true;

  ScopedNtHandle job;
  auto oa = internal_oa();
  NTSTATUS st =
      ::NtCreateJobObject(job.put(), JOB_OBJECT_ALL_ACCESS, &oa);
  if (!NT_SUCCESS(st))
    return false;

  st = ::NtAssignProcessToJobObject(job.get(), NtCurrentProcess());
  if (!NT_SUCCESS(st))
    return false;

  g_pcb.rlimit.job = job.release();
  return true;
}

// Apply the current cached limits to the self-job.
// Caller must have called ensure_self_job() first.
inline NTSTATUS apply_job_limits() {
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION eli = {};
  auto &bl = eli.BasicLimitInformation;
  DWORD flags = 0;
  auto &lim = g_pcb.rlimit.limits;

  // RLIMIT_CPU hard limit -> PerProcessUserTimeLimit (kernel termination).
  // Only set when rlim_max is finite — this is the backstop that kills
  // the process. The soft limit (SIGXCPU) is handled by the cpu_limit_timer
  // module via JOB_OBJECT_LIMIT_JOB_TIME notifications.
  if (lim[RLIMIT_CPU].rlim_max != RLIM_INFINITY) {
    bl.PerProcessUserTimeLimit.QuadPart =
        static_cast<LONGLONG>(lim[RLIMIT_CPU].rlim_max) * 10000000LL;
    flags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
  }

  // RLIMIT_CPU soft limit -> PerJobUserTimeLimit (SIGXCPU notification).
  // The cpu_limit_timer module sets POST_AT_END_OF_JOB so these fire as
  // IOCP notifications rather than terminating the process. After the first
  // SIGXCPU, cpu_soft_rearming switches to 1-second re-arm intervals.
  if (lim[RLIMIT_CPU].rlim_cur != RLIM_INFINITY) {
    rlim_t soft_secs = g_pcb.rlimit.cpu_soft_rearming
                           ? 1
                           : lim[RLIMIT_CPU].rlim_cur;
    bl.PerJobUserTimeLimit.QuadPart =
        static_cast<LONGLONG>(soft_secs) * 10000000LL;
    flags |= JOB_OBJECT_LIMIT_JOB_TIME;
  }

  // RLIMIT_AS -> ProcessMemoryLimit (bytes).
  rlim_t as_hard = lim[RLIMIT_AS].rlim_max;
  if (as_hard != RLIM_INFINITY) {
    eli.ProcessMemoryLimit = static_cast<SIZE_T>(as_hard);
    flags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
  }

  // RLIMIT_NPROC -> ActiveProcessLimit.
  if (lim[RLIMIT_NPROC].rlim_max != RLIM_INFINITY) {
    DWORD limit = static_cast<DWORD>(lim[RLIMIT_NPROC].rlim_max);
    if (limit == 0)
      limit = 1; // Job requires at least 1.
    bl.ActiveProcessLimit = limit;
    flags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
  }

  // RLIMIT_RSS -> working set limits.
  if (lim[RLIMIT_RSS].rlim_max != RLIM_INFINITY) {
    bl.MaximumWorkingSetSize = static_cast<SIZE_T>(lim[RLIMIT_RSS].rlim_max);
    // Minimum = soft limit, capped to not exceed max.
    SIZE_T min_ws = static_cast<SIZE_T>(lim[RLIMIT_RSS].rlim_cur);
    if (min_ws > bl.MaximumWorkingSetSize)
      min_ws = bl.MaximumWorkingSetSize;
    bl.MinimumWorkingSetSize = min_ws;
    flags |= JOB_OBJECT_LIMIT_WORKINGSET;
  }

  // RLIMIT_CORE semantics on NT:
  //   rlim_cur == 0        -> suppress WER (no dialog, no dump, no post-mortem
  //                           debugger) by setting SEM_NOGPFAULTERRORBOX and
  //                           JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION.
  //   rlim_cur != 0 / INF  -> leave WER on its default path: WerFault captures
  //                           a minidump per system policy and AeDebug (if
  //                           registered) launches a post-mortem debugger.
  //
  // The hard-error-mode write is symmetric: we unconditionally publish the
  // current desired value so that raising the limit after a prior 0-set
  // actually re-enables WER delivery. Without this, SEM_NOGPFAULTERRORBOX is
  // sticky and subsequent crashes stay silent.
  ULONG error_mode = 0;
  if (lim[RLIMIT_CORE].rlim_cur == 0) {
    flags |= JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    error_mode = SEM_NOGPFAULTERRORBOX;
  }
  ::NtSetInformationProcess(NtCurrentProcess(),
                            ProcessDefaultHardErrorMode, &error_mode,
                            sizeof(error_mode));

  // RLIMIT_RTPRIO -> cap the priority class for all processes in the job.
  // A limit of 0 means no real-time scheduling allowed (cap at HIGH).
  // Values 1+ allow REALTIME; the exact mapping is coarse since Windows
  // has a flat REALTIME class, not per-level like Linux SCHED_FIFO.
  if (lim[RLIMIT_RTPRIO].rlim_cur != RLIM_INFINITY) {
    if (lim[RLIMIT_RTPRIO].rlim_cur == 0)
      bl.PriorityClass = PROCESS_PRIORITY_CLASS_HIGH;
    else
      bl.PriorityClass = PROCESS_PRIORITY_CLASS_REALTIME;
    flags |= JOB_OBJECT_LIMIT_PRIORITY_CLASS;
  }

  bl.LimitFlags = flags;
  g_pcb.rlimit.active_flags = flags;

  return ::NtSetInformationJobObject(
      g_pcb.rlimit.job, JobObjectExtendedLimitInformation, &eli,
      sizeof(eli));
}

// Returns true if this RLIMIT_* value requires a job object for enforcement.
inline bool rlimit_needs_job(int resource) {
  switch (resource) {
  case RLIMIT_CPU:
  case RLIMIT_AS:
  case RLIMIT_NPROC:
  case RLIMIT_RSS:
  case RLIMIT_CORE:
  case RLIMIT_RTPRIO:
    return true;
  default:
    return false;
  }
}

// Returns true if this RLIMIT_* value is enforced via per-process quota
// (NtSetInformationProcess with ProcessQuotaLimits).
inline bool rlimit_needs_process_quota(int resource) {
  switch (resource) {
  case RLIMIT_MEMLOCK:
  case RLIMIT_RTTIME:
    return true;
  default:
    return false;
  }
}

// Apply RLIMIT_MEMLOCK and RLIMIT_RTTIME via NtSetInformationProcess
// (ProcessQuotaLimits). These are per-process, not per-job.
inline NTSTATUS apply_process_quota() {
  auto &lim = g_pcb.rlimit.limits;

  // Query current quota so we only override what's been set.
  QUOTA_LIMITS_EX quota = {};
  NTSTATUS st = ::NtQueryInformationProcess(NtCurrentProcess(),
                                            ProcessQuotaLimits, &quota,
                                            sizeof(quota), nullptr);
  if (!NT_SUCCESS(st))
    return st;

  bool changed = false;

  // RLIMIT_MEMLOCK -> MaximumWorkingSetSize.
  // The working set maximum is the effective mlock ceiling -- NtLockVirtualMemory
  // fails with STATUS_WORKING_SET_QUOTA when the locked set exceeds this.
  if (lim[RLIMIT_MEMLOCK].rlim_cur != RLIM_INFINITY) {
    quota.MaximumWorkingSetSize =
        static_cast<SIZE_T>(lim[RLIMIT_MEMLOCK].rlim_cur);
    // Ensure minimum <= maximum.
    if (quota.MinimumWorkingSetSize > quota.MaximumWorkingSetSize)
      quota.MinimumWorkingSetSize = quota.MaximumWorkingSetSize;
    quota.Flags |= QUOTA_LIMITS_HARDWS_MAX_ENABLE;
    changed = true;
  }

  // RLIMIT_RTTIME -> TimeLimit (per-process CPU time, 100ns units).
  // POSIX RLIMIT_RTTIME is in microseconds; convert us -> 100ns.
  if (lim[RLIMIT_RTTIME].rlim_cur != RLIM_INFINITY) {
    quota.TimeLimit.QuadPart =
        static_cast<LONGLONG>(lim[RLIMIT_RTTIME].rlim_cur) * 10;
    changed = true;
  }

  if (!changed)
    return 0; // STATUS_SUCCESS

  // Acquire privileges needed for working set adjustment.
  ULONG privs[] = {SE_INCREASE_WORKING_SET_PRIVILEGE,
                   SE_INC_BASE_PRIORITY_PRIVILEGE};
  PVOID priv_state = nullptr;
  ::RtlAcquirePrivilege(privs, 2, 0, &priv_state);

  st = ::NtSetInformationProcess(NtCurrentProcess(), ProcessQuotaLimits,
                                 &quota, sizeof(quota));

  if (priv_state)
    ::RtlReleasePrivilege(priv_state);

  return st;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RLIMIT_STATE_H
