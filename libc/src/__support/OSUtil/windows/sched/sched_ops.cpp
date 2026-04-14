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
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

// Find the NUMA node for a given processor group + mask.
// Returns the node number, or 0 if lookup fails (safe default).
unsigned int find_numa_node(WORD group, KAFFINITY proc_mask) {
  ULONG needed = 0;
  ULONG relationship = RelationNumaNode;
  NTSTATUS st = ::NtQuerySystemInformationEx(
      SystemLogicalProcessorInformationEx, &relationship, sizeof(relationship),
      nullptr, 0, &needed);

  if (st != STATUS_INFO_LENGTH_MISMATCH || needed == 0)
    return 0;

  // Each NUMA entry is ~48 bytes; 4KB covers ~80 nodes.
  auto ss = internal::byte_scratch(4096);
  if (!ss)
    return 0;
  if (needed > ss.size())
    return 0;

  st = ::NtQuerySystemInformationEx(
      SystemLogicalProcessorInformationEx, &relationship, sizeof(relationship),
      ss.data(), needed, nullptr);
  if (!NT_SUCCESS(st))
    return 0;

  // Minimum valid entry: Relationship (4) + Size (4) = 8 bytes.
  constexpr DWORD MIN_ENTRY = 8;
  const char *ptr = ss.data();
  const char *end = ss.data() + needed;
  while (ptr + MIN_ENTRY <= end) {
    auto *entry =
        reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(ptr);
    if (entry->Size < MIN_ENTRY || ptr + entry->Size > end)
      break;

    if (entry->Relationship == RelationNumaNode &&
        entry->NumaNode.GroupMask.Group == group &&
        (entry->NumaNode.GroupMask.Mask & proc_mask) != 0) {
      return entry->NumaNode.NodeNumber;
    }

    ptr += entry->Size;
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
    HANDLE handle;
    NTSTATUS status = sched_impl::open_thread_by_tid(
        static_cast<DWORD>(tid), THREAD_QUERY_INFORMATION, &handle);
    if (!NT_SUCCESS(status))
      return -ESRCH;
    ::NtClose(handle);
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
