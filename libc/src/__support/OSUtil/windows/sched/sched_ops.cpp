//===-- Windows internal sched simple operations ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal kernel functions for sched_yield, getcpu, sched_getcpu,
// sched_get_priority_max, sched_get_priority_min, sched_rr_get_interval on
// Windows. These implement Linux syscall semantics: 0/value on success,
// -errno on failure. Called from windows_syscalls:: wrappers.
//
//===----------------------------------------------------------------------===//

#include "sched_ops.h"
#include "sched_helpers.h"
#include "hdr/errno_macros.h"
#include "hdr/sched_macros.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/nt_pal/numa_topology.h"
#include "src/__support/OSUtil/windows/nt_pal/pal_state.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

// Find the NUMA node for a given processor group + mask.
//
// Reads the sealed Zone 0 NUMA topology snapshot built once at libc
// init — no syscall, no scratch allocation, no buffer-walk on the
// `getcpu()` hot path. The snapshot was built from
// `NtQuerySystemInformationEx(SystemLogicalProcessorInformationEx,
// RelationNumaNode, ...)` so the answer is identical to the previous
// on-demand walk; only the cost shape changed.
//
// `proc_mask` carries the caller's affinity bits (typically a single
// processor's bit selected from `RtlGetCurrentProcessorNumberEx`); we
// return the node id of the first matching CPU. Returns 0 if no CPU
// in the mask was enumerated by the topology probe — same safe default
// as the previous implementation.
unsigned int find_numa_node(WORD group, KAFFINITY proc_mask) {
  if (group >= 4 || proc_mask == 0)
    return 0;
  const windows::NumaTopology &topo = nt_pal::numa_topology();
  KAFFINITY mask = proc_mask;
  while (mask != 0) {
    ULONG bit = __builtin_ctzll(mask);
    mask &= mask - 1;
    uint32_t idx = static_cast<uint32_t>(group) * 64u + bit;
    if (idx >= windows::kNumaCpuTableSize)
      continue;
    uint8_t node = topo.cpu_to_node[idx];
    if (node != windows::kNumaNodeUnassigned)
      return node;
  }
  return 0;
}

} // namespace

intptr_t sched_yield() {
  ::NtYieldExecution();
  return 0;
}

intptr_t getcpu(unsigned int *cpu, unsigned int *node) {
  PROCESSOR_NUMBER pn;
  ULONG proc_num = ::NtGetCurrentProcessorNumberEx(&pn);

  if (cpu != nullptr)
    *cpu = proc_num;

  if (node != nullptr)
    *node = find_numa_node(pn.Group, static_cast<KAFFINITY>(1) << pn.Number);

  return 0;
}

intptr_t sched_getcpu() {
  PROCESSOR_NUMBER pn;
  ::NtGetCurrentProcessorNumberEx(&pn);
  return static_cast<intptr_t>(pn.Group) * 64 + pn.Number;
}

intptr_t sched_get_priority_max(int policy) {
  switch (policy) {
  case SCHED_OTHER:
    return 0;
  case SCHED_FIFO:
  case SCHED_RR:
    return 99;
  default:
    return -EINVAL;
  }
}

intptr_t sched_get_priority_min(int policy) {
  switch (policy) {
  case SCHED_OTHER:
    return 0;
  case SCHED_FIFO:
  case SCHED_RR:
    return 1;
  default:
    return -EINVAL;
  }
}

intptr_t sched_rr_get_interval(pid_t tid, struct timespec *tp) {
  if (!tp)
    return -EINVAL;
  if (tid < 0)
    return -EINVAL;

  // Validate that the thread exists (POSIX: ESRCH).
  if (tid != 0 && static_cast<DWORD>(tid) != ::NtCurrentThreadId()) {
    windows::ScopedNtHandle handle;
    NTSTATUS status = sched_impl::open_thread_by_tid(
        static_cast<DWORD>(tid), THREAD_QUERY_INFORMATION, handle.put());
    if (!NT_SUCCESS(status))
      return -ESRCH;
  }

  // Query the current timer resolution. NtQueryTimerResolution returns values
  // in 100-ns units. The "current" resolution is what the scheduler uses.
  ULONG min_res, max_res, cur_res;
  NTSTATUS status = ::NtQueryTimerResolution(&min_res, &max_res, &cur_res);
  if (!NT_SUCCESS(status)) {
    // Fallback: default Windows quantum is ~15.625ms.
    tp->tv_sec = 0;
    tp->tv_nsec = 15625000;
    return 0;
  }

  // Convert 100-ns units to timespec.
  tp->tv_sec = static_cast<time_t>(cur_res / 10000000UL);
  tp->tv_nsec = static_cast<long>((cur_res % 10000000UL) * 100);
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
