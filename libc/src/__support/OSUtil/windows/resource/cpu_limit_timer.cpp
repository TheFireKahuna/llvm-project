//===-- RLIMIT_CPU soft limit via job IOCP notifications -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "cpu_limit_timer.h"

#include "hdr/signal_macros.h"
#include "include/llvm-libc-macros/sys-resource-macros.h"
#include "src/__support/OSUtil/windows/nt/nt_job.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/macros/config.h"
#include "rlimit_state.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

namespace {

// Whether the self-job has been associated with the reactor IOCP.
// Once true, it stays true for the process lifetime (can't disassociate).
bool g_job_iocp_associated = false;

// Reactor token for the job watch.
internal::reactor::ReactorToken g_cpu_token = internal::reactor::INVALID_TOKEN;

// Reactor callback — fires on the drain thread when JOB_OBJECT_LIMIT_JOB_TIME
// is exceeded. The `information` parameter carries the JOB_OBJECT_MSG_* value.
//
// POSIX semantics: deliver SIGXCPU at the soft limit, then every second
// thereafter. The hard limit (rlim_max) is enforced by PerProcessUserTimeLimit
// which terminates the process directly.
static void cpu_limit_reactor_cb(void * /*context*/, NTSTATUS /*status*/,
                                 ULONG_PTR information) {
  // Only handle job-time notifications. Other job messages (process exit,
  // active process limit, etc.) are silently ignored here.
  if (information != JOB_OBJECT_MSG_END_OF_JOB_TIME)
    return;

  // Deliver SIGXCPU to the process (any unblocked thread receives it).
  signal_state::deliver_process_signal(SIGXCPU);

  // Switch to 1-second re-arm mode and re-apply job limits.
  // apply_job_limits() reads cpu_soft_rearming and uses 1 second for
  // PerJobUserTimeLimit instead of the original soft limit.
  g_pcb.rlimit.cpu_soft_rearming = true;

  // Re-apply all job limits with the 1-second re-arm interval.
  // This is safe from the drain thread — apply_job_limits() only reads
  // cached state from g_pcb.rlimit.limits[] and calls NtSetInformationJobObject.
  apply_job_limits();
}

} // anonymous namespace

void cpu_limit_timer_update() {
  // Reset re-arming — the application set a fresh limit.
  g_pcb.rlimit.cpu_soft_rearming = false;

  // If no soft limit, nothing to do. The hard limit (PROCESS_TIME) is
  // already set by apply_job_limits().
  if (g_pcb.rlimit.limits[RLIMIT_CPU].rlim_cur == RLIM_INFINITY)
    return;

  // The self-job must already exist (caller ensured it via ensure_self_job).
  HANDLE job = g_pcb.rlimit.job;
  if (!job)
    return;

  // One-time: set EndOfJobTimeAction to POST so JOB_TIME fires as an
  // IOCP notification instead of terminating all processes.
  // One-time: associate the job with the reactor's IOCP.
  if (!g_job_iocp_associated) {
    JOBOBJECT_END_OF_JOB_TIME_INFORMATION eoj;
    eoj.EndOfJobTimeAction = JOB_OBJECT_POST_AT_END_OF_JOB;
    ::NtSetInformationJobObject(job, JobObjectEndOfJobTimeInformation,
                                &eoj, sizeof(eoj));

    g_cpu_token =
        internal::reactor::watch_job(job, cpu_limit_reactor_cb, nullptr);
    if (!g_cpu_token.valid())
      return; // Reactor not ready — soft limit won't fire. Hard limit
              // still enforced via PROCESS_TIME.

    g_job_iocp_associated = true;
  }
}

void cpu_limit_timer_reset() {
  // Fork child: all job handles are stale. The reactor fork_reinit already
  // invalidated the token. Reset our state so the next setrlimit re-creates
  // the association.
  g_job_iocp_associated = false;
  g_cpu_token = internal::reactor::INVALID_TOKEN;
  g_pcb.rlimit.cpu_soft_rearming = false;
}

static void cpu_limit_timer_fork_reinit_impl() {
  cpu_limit_timer_reset();
}

static void cpu_limit_timer_fork_restore_impl() {
  if (g_pcb.rlimit.initialized.load(cpp::MemoryOrder::ACQUIRE) !=
      RLIMIT_INIT_READY)
    return;
  if (!g_pcb.rlimit.job)
    return;
  if (g_pcb.rlimit.limits[RLIMIT_CPU].rlim_cur == RLIM_INFINITY)
    return;

  cpu_limit_timer_update();
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

// After setitimer reinit, before anything that might call setrlimit
// in the child.
void LIBC_NAMESPACE::internal::cpu_limit_timer_fork_reinit() {
  LIBC_NAMESPACE::windows::cpu_limit_timer_fork_reinit_impl();
}

// After reactor rebuild. Rebind the recreated self-job to the new IOCP
// if RLIMIT_CPU soft enforcement was active in the parent.
void LIBC_NAMESPACE::internal::cpu_limit_timer_fork_restore() {
  LIBC_NAMESPACE::windows::cpu_limit_timer_fork_restore_impl();
}

LIBC_REGISTER_FORK_REINIT(cpu_limit_timer,
                          ::LIBC_NAMESPACE::internal::kForkPrioCpuLimitTimer,
                          &::LIBC_NAMESPACE::internal::cpu_limit_timer_fork_reinit)

LIBC_REGISTER_FORK_REINIT(cpu_limit_timer_restore,
                          ::LIBC_NAMESPACE::internal::kForkPrioCpuLimitTimerRestore,
                          &::LIBC_NAMESPACE::internal::cpu_limit_timer_fork_restore)
