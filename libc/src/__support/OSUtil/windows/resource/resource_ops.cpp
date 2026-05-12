//===-- Internal resource limit engine implementation ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Business logic for getrlimit, setrlimit, prlimit, and getrusage.
// Returns 0 on success, -errno on failure. No libc_errno references.
//
//===----------------------------------------------------------------------===//

#include "resource_ops.h"

#include "cpu_limit_timer.h"
#include "hdr/errno_macros.h"
#include "hdr/types/struct_rlimit.h"
#include "hdr/types/struct_rusage.h"
#include "include/llvm-libc-macros/sys-resource-macros.h"
#include "src/__support/OSUtil/windows/nt_pal/working_set.h"
#include "src/__support/OSUtil/windows/nt/nt_job.h"
#include "src/__support/OSUtil/windows/nt/nt_process.h"
#include "src/__support/macros/config.h"
#include "rlimit_state.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// ---------------------------------------------------------------------------
// getrlimit engine
// ---------------------------------------------------------------------------
intptr_t getrlimit(int resource, struct rlimit *lim) {
  if (resource < 0 || resource >= windows::RLIMIT_PROCESS_COUNT ||
      lim == nullptr)
    return -EINVAL;

  windows::ensure_rlimit_init();

  // Return cached limits (set by setrlimit, or RLIM_INFINITY defaults).
  lim->rlim_cur = g_pcb.rlimit.limits[resource].rlim_cur;
  lim->rlim_max = g_pcb.rlimit.limits[resource].rlim_max;

  // For certain resources, query live system state if no explicit limit is set.
  switch (resource) {
  case RLIMIT_NOFILE:
    // Our fd table capacity. If no limit set, return the table size.
    if (lim->rlim_cur == RLIM_INFINITY) {
      lim->rlim_cur = 1048576;
      lim->rlim_max = 1048576;
    }
    break;

  case RLIMIT_STACK:
    // Query current thread's reserved stack size from TEB if no limit set.
    // TEB+0x08 = StackBase (high), TEB+0x1478 = DeallocationStack (low).
    if (lim->rlim_cur == RLIM_INFINITY) {
      ULONG_PTR stack_base, dealloc_stack;
#ifdef __x86_64__
      __asm__ __volatile__("movq %%gs:0x08, %0" : "=r"(stack_base));
      __asm__ __volatile__("movq %%gs:0x1478, %0" : "=r"(dealloc_stack));
#elif defined(__aarch64__)
      __asm__ __volatile__("ldr %0, [x18, #0x08]" : "=r"(stack_base));
      __asm__ __volatile__("ldr %0, [x18, #0x1478]" : "=r"(dealloc_stack));
#endif
      if (stack_base > dealloc_stack) {
        rlim_t stack_size = stack_base - dealloc_stack;
        lim->rlim_cur = stack_size;
        lim->rlim_max = stack_size;
      }
    }
    break;

  case RLIMIT_AS:
  case RLIMIT_DATA:
    // If no explicit limit, report the user-mode VA space size.
    if (lim->rlim_cur == RLIM_INFINITY) {
      SYSTEM_BASIC_INFORMATION sbi;
      NTSTATUS st = ::NtQuerySystemInformation(SystemBasicInformation, &sbi,
                                               sizeof(sbi), nullptr);
      if (NT_SUCCESS(st)) {
        rlim_t va_size =
            sbi.MaximumUserModeAddress - sbi.MinimumUserModeAddress + 1;
        lim->rlim_cur = va_size;
        lim->rlim_max = va_size;
      }
    }
    break;

  case RLIMIT_MEMLOCK:
    // Query the process working set maximum — this is the effective ceiling
    // for NtLockVirtualMemory (mlock).
    if (lim->rlim_cur == RLIM_INFINITY) {
      SIZE_T min_ws, max_ws;
      ULONG ws_flags;
      if (nt_pal::query_working_set(NtCurrentProcess(), min_ws, max_ws,
                                    ws_flags)) {
        lim->rlim_cur = static_cast<rlim_t>(max_ws);
        lim->rlim_max = static_cast<rlim_t>(max_ws);
      }
    }
    break;

  case RLIMIT_SIGPENDING:
    // Default to our signal queue capacity (SIGQUEUE_MAX = 1024).
    if (lim->rlim_cur == RLIM_INFINITY) {
      lim->rlim_cur = 1024;
      lim->rlim_max = 1024;
    }
    break;

  case RLIMIT_NICE:
    // Default: no priority raising allowed (matches Linux unprivileged).
    // RLIMIT_NICE=0 means nice floor = 20, so only nice 0..19 is allowed.
    if (lim->rlim_cur == RLIM_INFINITY) {
      // Query the current process priority class and convert to a nice ceiling.
      PROCESS_PRIORITY_CLASS ppc = {};
      NTSTATUS st = ::NtQueryInformationProcess(
          NtCurrentProcess(), ProcessPriorityClass, &ppc, sizeof(ppc),
          nullptr);
      if (NT_SUCCESS(st)) {
        // Map Windows priority class -> approximate RLIMIT_NICE value.
        // RLIMIT_NICE = 20 - nice_floor, where nice_floor is the lowest
        // (highest-priority) nice value achievable.
        rlim_t nice_val;
        switch (ppc.PriorityClass) {
        case PROCESS_PRIORITY_CLASS_REALTIME:     nice_val = 40; break;
        case PROCESS_PRIORITY_CLASS_HIGH:         nice_val = 40; break;
        case PROCESS_PRIORITY_CLASS_ABOVE_NORMAL: nice_val = 25; break;
        case PROCESS_PRIORITY_CLASS_NORMAL:       nice_val = 20; break;
        case PROCESS_PRIORITY_CLASS_BELOW_NORMAL: nice_val = 15; break;
        case PROCESS_PRIORITY_CLASS_IDLE:         nice_val = 5;  break;
        default:                                  nice_val = 20; break;
        }
        lim->rlim_cur = nice_val;
        lim->rlim_max = 40; // Max possible (nice -20 = RLIMIT_NICE 40)
      }
    }
    break;

  case RLIMIT_RTPRIO:
    // Default: 0 (no real-time scheduling), matching Linux unprivileged.
    if (lim->rlim_cur == RLIM_INFINITY) {
      lim->rlim_cur = 0;
      lim->rlim_max = 0;
    }
    break;

  case RLIMIT_RTTIME:
    // Query the per-process CPU time limit from the process quota.
    if (lim->rlim_cur == RLIM_INFINITY) {
      QUOTA_LIMITS_EX quota = {};
      NTSTATUS st = ::NtQueryInformationProcess(
          NtCurrentProcess(), ProcessQuotaLimits, &quota, sizeof(quota),
          nullptr);
      if (NT_SUCCESS(st) && quota.TimeLimit.QuadPart > 0) {
        // Convert 100ns units -> microseconds.
        rlim_t us = static_cast<rlim_t>(quota.TimeLimit.QuadPart / 10);
        lim->rlim_cur = us;
        lim->rlim_max = us;
      }
    }
    break;

  case RLIMIT_CORE:
    // No live query — returned from cache (default RLIM_INFINITY = dumps
    // allowed). Setting to 0 disables WER crash dialogs via the job flag.
    break;

  default:
    break;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// setrlimit engine
// ---------------------------------------------------------------------------
intptr_t setrlimit(int resource, const struct rlimit *lim) {
  if (resource < 0 || resource >= windows::RLIMIT_PROCESS_COUNT ||
      lim == nullptr)
    return -EINVAL;

  // POSIX: soft limit must not exceed hard limit.
  if (lim->rlim_cur > lim->rlim_max)
    return -EINVAL;

  windows::ensure_rlimit_init();

  // POSIX: unprivileged processes cannot raise the hard limit.
  // We enforce this for job-backed limits since the job is irreversible.
  // For cached-only limits, we allow it (no kernel enforcement to violate).
  struct rlimit &cached = g_pcb.rlimit.limits[resource];
  if (windows::rlimit_needs_job(resource) &&
      cached.rlim_max != RLIM_INFINITY) {
    if (lim->rlim_max > cached.rlim_max)
      return -EPERM;
  }

  // Cache the new limits.
  cached.rlim_cur = lim->rlim_cur;
  cached.rlim_max = lim->rlim_max;

  // If this resource needs kernel enforcement, create the self-job and apply.
  if (windows::rlimit_needs_job(resource)) {
    if (!windows::ensure_self_job())
      return -EPERM;

    NTSTATUS st = windows::apply_job_limits();
    if (!NT_SUCCESS(st))
      return -EINVAL;

    // RLIMIT_CPU: set up IOCP notification for the soft limit (SIGXCPU).
    if (resource == RLIMIT_CPU)
      windows::cpu_limit_timer_update();
  }

  // RLIMIT_MEMLOCK and RLIMIT_RTTIME are enforced via per-process quota.
  if (windows::rlimit_needs_process_quota(resource)) {
    NTSTATUS st = windows::apply_process_quota();
    if (!NT_SUCCESS(st))
      return -EPERM;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// prlimit engine
// ---------------------------------------------------------------------------
intptr_t prlimit(int pid, int resource, const struct rlimit *new_limit,
             struct rlimit *old_limit) {
  // Check if pid refers to the current process.
  if (pid != 0) {
    PROCESS_BASIC_INFORMATION pbi;
    NTSTATUS st = ::NtQueryInformationProcess(
        NtCurrentProcess(), ProcessBasicInformation, &pbi, sizeof(pbi),
        nullptr);
    if (!NT_SUCCESS(st) || static_cast<int>(pbi.UniqueProcessId) != pid)
      return -ESRCH;
  }

  if (resource < 0 || resource >= windows::RLIMIT_PROCESS_COUNT)
    return -EINVAL;

  windows::ensure_rlimit_init();

  // Return old limits before applying new ones (atomic get-and-set).
  if (old_limit != nullptr) {
    old_limit->rlim_cur = g_pcb.rlimit.limits[resource].rlim_cur;
    old_limit->rlim_max = g_pcb.rlimit.limits[resource].rlim_max;
  }

  if (new_limit != nullptr) {
    if (new_limit->rlim_cur > new_limit->rlim_max)
      return -EINVAL;

    // Enforce hard limit ratchet for job-backed resources.
    struct rlimit &cached = g_pcb.rlimit.limits[resource];
    if (windows::rlimit_needs_job(resource) &&
        cached.rlim_max != RLIM_INFINITY) {
      if (new_limit->rlim_max > cached.rlim_max)
        return -EPERM;
    }

    cached.rlim_cur = new_limit->rlim_cur;
    cached.rlim_max = new_limit->rlim_max;

    if (windows::rlimit_needs_job(resource)) {
      if (!windows::ensure_self_job())
        return -EPERM;

      NTSTATUS st = windows::apply_job_limits();
      if (!NT_SUCCESS(st))
        return -EINVAL;

      if (resource == RLIMIT_CPU)
        windows::cpu_limit_timer_update();
    }
  }

  return 0;
}

// ---------------------------------------------------------------------------
// getrusage engine
// ---------------------------------------------------------------------------
namespace {

// Convert 100ns NT time units to struct timeval.
void filetime_to_timeval(LARGE_INTEGER ft, struct timeval &tv) {
  // 100ns ticks -> seconds + microseconds.
  LONGLONG total_us = ft.QuadPart / 10;
  tv.tv_sec = static_cast<decltype(tv.tv_sec)>(total_us / 1000000);
  tv.tv_usec = static_cast<decltype(tv.tv_usec)>(total_us % 1000000);
}

intptr_t getrusage_self(struct rusage *usage) {
  // CPU times.
  KERNEL_USER_TIMES times;
  NTSTATUS st = ::NtQueryInformationProcess(
      NtCurrentProcess(), ProcessTimes, &times, sizeof(times), nullptr);
  if (!NT_SUCCESS(st))
    return -EINVAL;

  filetime_to_timeval(times.UserTime, usage->ru_utime);
  filetime_to_timeval(times.KernelTime, usage->ru_stime);

  // Memory counters — peak RSS and page faults.
  VM_COUNTERS vmc;
  st = ::NtQueryInformationProcess(NtCurrentProcess(), ProcessVmCounters, &vmc,
                                   sizeof(vmc), nullptr);
  if (NT_SUCCESS(st)) {
    // POSIX ru_maxrss is in kilobytes.
    usage->ru_maxrss = static_cast<long>(vmc.PeakWorkingSetSize / 1024);
    // Windows doesn't distinguish hard/soft faults — report all as minor.
    usage->ru_minflt = static_cast<long>(vmc.PageFaultCount);
  }

  // I/O counters.
  IO_COUNTERS ioc;
  st = ::NtQueryInformationProcess(NtCurrentProcess(), ProcessIoCounters, &ioc,
                                   sizeof(ioc), nullptr);
  if (NT_SUCCESS(st)) {
    usage->ru_inblock = static_cast<long>(ioc.ReadOperationCount);
    usage->ru_oublock = static_cast<long>(ioc.WriteOperationCount);
  }

  return 0;
}

// RUSAGE_CHILDREN: query the self-job's cumulative accounting and subtract
// the current process's own usage to isolate children-only totals.
// Without a self-job (no setrlimit was called), return zeroes — there's no
// way to track exited children on Windows without job containment.
intptr_t getrusage_children(struct rusage *usage) {
  windows::ensure_rlimit_init();
  HANDLE job = g_pcb.rlimit.job;
  if (!job)
    return 0; // No self-job — all fields already zeroed by caller.

  // Query job-wide cumulative accounting (all processes, including exited).
  JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION job_info = {};
  NTSTATUS st = ::NtQueryInformationJobObject(
      job, JobObjectBasicAndIoAccountingInformation,
      &job_info, sizeof(job_info), nullptr);
  if (!NT_SUCCESS(st))
    return 0; // Best effort — return zeroes.

  // Query the current process's own usage to subtract.
  KERNEL_USER_TIMES self_times = {};
  ::NtQueryInformationProcess(NtCurrentProcess(), ProcessTimes,
                              &self_times, sizeof(self_times), nullptr);

  IO_COUNTERS self_ioc = {};
  ::NtQueryInformationProcess(NtCurrentProcess(), ProcessIoCounters,
                              &self_ioc, sizeof(self_ioc), nullptr);

  VM_COUNTERS self_vmc = {};
  ::NtQueryInformationProcess(NtCurrentProcess(), ProcessVmCounters,
                              &self_vmc, sizeof(self_vmc), nullptr);

  // Children CPU time = job total - self.
  LARGE_INTEGER child_user;
  child_user.QuadPart = job_info.BasicInfo.TotalUserTime.QuadPart -
                        self_times.UserTime.QuadPart;
  if (child_user.QuadPart < 0)
    child_user.QuadPart = 0;

  LARGE_INTEGER child_kernel;
  child_kernel.QuadPart = job_info.BasicInfo.TotalKernelTime.QuadPart -
                          self_times.KernelTime.QuadPart;
  if (child_kernel.QuadPart < 0)
    child_kernel.QuadPart = 0;

  filetime_to_timeval(child_user, usage->ru_utime);
  filetime_to_timeval(child_kernel, usage->ru_stime);

  // Children page faults = job total - self.
  DWORD self_faults = self_vmc.PageFaultCount;
  DWORD job_faults = job_info.BasicInfo.TotalPageFaultCount;
  usage->ru_minflt =
      static_cast<long>(job_faults > self_faults ? job_faults - self_faults : 0);

  // Children I/O = job total - self.
  ULONGLONG child_reads =
      job_info.IoInfo.ReadOperationCount > self_ioc.ReadOperationCount
          ? job_info.IoInfo.ReadOperationCount - self_ioc.ReadOperationCount
          : 0;
  ULONGLONG child_writes =
      job_info.IoInfo.WriteOperationCount > self_ioc.WriteOperationCount
          ? job_info.IoInfo.WriteOperationCount - self_ioc.WriteOperationCount
          : 0;
  usage->ru_inblock = static_cast<long>(child_reads);
  usage->ru_oublock = static_cast<long>(child_writes);

  // Note: ru_maxrss for children is not available from job accounting —
  // the job tracks peak job-wide working set, not per-child peaks.
  // Left as 0 (already zeroed by caller).

  return 0;
}

} // anonymous namespace

intptr_t getrusage(int who, struct rusage *usage) {
  if (usage == nullptr)
    return -EINVAL;

  // Zero all fields — unmapped fields stay 0.
  __builtin_memset(usage, 0, sizeof(*usage));

  switch (who) {
  case RUSAGE_SELF:
    return getrusage_self(usage);
  case RUSAGE_CHILDREN:
    return getrusage_children(usage);
  default:
    return -EINVAL;
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

namespace {

bool needs_job_limit_restore_after_fork() {
  auto &lim = LIBC_NAMESPACE::g_pcb.rlimit.limits;

  return lim[RLIMIT_CPU].rlim_max != RLIM_INFINITY ||
         lim[RLIMIT_CPU].rlim_cur != RLIM_INFINITY ||
         lim[RLIMIT_AS].rlim_max != RLIM_INFINITY ||
         lim[RLIMIT_NPROC].rlim_max != RLIM_INFINITY ||
         lim[RLIMIT_RSS].rlim_max != RLIM_INFINITY ||
         lim[RLIMIT_CORE].rlim_cur == 0 ||
         lim[RLIMIT_RTPRIO].rlim_cur != RLIM_INFINITY;
}

bool needs_process_quota_restore_after_fork() {
  auto &lim = LIBC_NAMESPACE::g_pcb.rlimit.limits;

  return lim[RLIMIT_MEMLOCK].rlim_cur != RLIM_INFINITY ||
         lim[RLIMIT_RTTIME].rlim_cur != RLIM_INFINITY;
}

} // anonymous namespace

void LIBC_NAMESPACE::internal::rlimit_fork_reinit() {
  int init_state =
      LIBC_NAMESPACE::g_pcb.rlimit.initialized.load(
          LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE);
  bool restore_process_quota =
      init_state == LIBC_NAMESPACE::windows::RLIMIT_INIT_READY &&
      needs_process_quota_restore_after_fork();
  bool restore_job_limits =
      init_state == LIBC_NAMESPACE::windows::RLIMIT_INIT_READY &&
      needs_job_limit_restore_after_fork();

  LIBC_NAMESPACE::windows::rlimit_fork_reinit();

  if (restore_process_quota) {
    NTSTATUS st = LIBC_NAMESPACE::windows::apply_process_quota();
    if (!NT_SUCCESS(st))
      __builtin_trap();
  }

  if (!restore_job_limits)
    return;

  if (!LIBC_NAMESPACE::windows::ensure_self_job())
    __builtin_trap();
  NTSTATUS st = LIBC_NAMESPACE::windows::apply_job_limits();
  if (!NT_SUCCESS(st))
    __builtin_trap();
}

LIBC_REGISTER_FORK_REINIT(rlimit,
                          ::LIBC_NAMESPACE::internal::kForkPrioRlimit,
                          &::LIBC_NAMESPACE::internal::rlimit_fork_reinit)
