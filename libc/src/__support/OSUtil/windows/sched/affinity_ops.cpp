//===-- Windows internal CPU affinity operations ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal kernel functions for sched_getaffinity and sched_setaffinity on
// Windows. These implement Linux syscall semantics: 0 on success, -errno on
// failure. Called from windows_syscalls:: wrappers.
//
//===----------------------------------------------------------------------===//

#include "affinity_ops.h"
#include "sched_helpers.h"
#include "hdr/errno_macros.h"
#include "hdr/types/cpu_set_t.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/size_t.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

// Write a GROUP_AFFINITY into the cpu_set_t at the correct bit offset.
void write_group_affinity(cpu_set_t *mask, size_t cpuset_size,
                          WORD group, KAFFINITY bits) {
  constexpr size_t BITS_PER_GROUP = sizeof(KAFFINITY) * 8;
  size_t base_byte = static_cast<size_t>(group) * BITS_PER_GROUP / 8;
  if (base_byte + sizeof(KAFFINITY) <= cpuset_size) {
    auto *raw = reinterpret_cast<unsigned char *>(mask);
    __builtin_memcpy(raw + base_byte, &bits, sizeof(KAFFINITY));
  }
}

// Query active processor masks for all groups via
// SystemLogicalProcessorInformationEx(RelationGroup).
// Writes active CPU bits for each group the process belongs to.
// Returns true on success.
bool fill_process_affinity(cpu_set_t *mask, size_t cpuset_size) {
  // First, get which groups the process spans.
  // 64 groups covers all current Windows configurations.
  constexpr ULONG MAX_GROUPS = 64;
  ScratchAlloc<USHORT> groups(MAX_GROUPS);
  if (!groups)
    return false;

  ULONG ret_len = 0;
  NTSTATUS status = ::NtQueryInformationProcess(
      NtCurrentProcess(), ProcessGroupInformation,
      groups.data(), MAX_GROUPS * sizeof(USHORT), &ret_len);

  if (!NT_SUCCESS(status) || ret_len < sizeof(USHORT))
    return false;

  ULONG num_groups = ret_len / sizeof(USHORT);

  // Query topology for active processor masks.
  ULONG topo_len = 0;
  ::NtQuerySystemInformation(SystemLogicalProcessorInformationEx,
                             nullptr, 0, &topo_len);
  if (topo_len == 0)
    return false;

  // Scratch buffer for typical systems. 8KB covers up to ~64 groups.
  auto ss = internal::byte_scratch(8192);
  if (!ss)
    return false;
  if (topo_len > ss.size())
    return false; // Extraordinary topology — fall back to single-group.
  char *buf = ss.data();

  status = ::NtQuerySystemInformation(SystemLogicalProcessorInformationEx,
                                      buf, topo_len, &topo_len);
  if (!NT_SUCCESS(status))
    return false;

  // Walk topology entries, looking for RelationGroup.
  for (ULONG off = 0; off < topo_len;) {
    auto *entry = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(
        buf + off);
    if (entry->Size == 0)
      break;

    if (entry->Relationship == RelationGroup) {
      // GROUP_RELATIONSHIP contains PROCESSOR_GROUP_INFO per group.
      auto *grp_rel = &entry->Group;
      for (WORD g = 0;
           g < grp_rel->MaximumGroupCount && g < grp_rel->ActiveGroupCount;
           ++g) {
        auto &info = grp_rel->GroupInfo[g];
        // Only include groups the process belongs to.
        for (ULONG pg = 0; pg < num_groups; ++pg) {
          if (groups[pg] == g) {
            write_group_affinity(mask, cpuset_size, g,
                                info.ActiveProcessorMask);
            break;
          }
        }
      }
      return true;
    }

    off += entry->Size;
  }

  return false;
}

// Query a thread's selected CPU sets via ThreadSelectedCpuSets and convert
// the returned CPU set IDs to cpu_set_t bits. Returns true if the thread
// has explicit CPU set restrictions, false if unrestricted (empty set) or
// on failure.
bool fill_thread_cpu_sets(HANDLE thread_handle, cpu_set_t *mask,
                          size_t cpuset_size) {
  // First call: get required buffer size.
  ULONG ret_len = 0;
  NTSTATUS status = ::NtQueryInformationThread(
      thread_handle, ThreadSelectedCpuSets, nullptr, 0, &ret_len);

  // Empty set = unrestricted thread. STATUS_BUFFER_TOO_SMALL means there
  // are IDs to retrieve.
  if (ret_len == 0)
    return false;

  constexpr size_t MAX_IDS = sched_impl::MAX_CPU_SETS;
  ULONG ids[MAX_IDS];
  if (ret_len > MAX_IDS * sizeof(ULONG))
    return false;

  status = ::NtQueryInformationThread(thread_handle, ThreadSelectedCpuSets,
                                      ids, ret_len, &ret_len);
  if (!NT_SUCCESS(status) || ret_len == 0)
    return false;

  ULONG id_count = ret_len / sizeof(ULONG);
  if (id_count == 0)
    return false;

  // Enumerate system CPU sets to map IDs → bit positions.
  auto sets_s = internal::byte_scratch(MAX_IDS * sizeof(SYSTEM_CPU_SET_INFORMATION));
  if (!sets_s)
    return false;
  auto *sets = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION *>(sets_s.data());
  size_t num_sets = sched_impl::enumerate_cpu_sets(sets, MAX_IDS);
  if (num_sets == 0)
    return false;

  for (ULONG i = 0; i < id_count; ++i) {
    size_t bit = sched_impl::cpu_set_id_to_bit(sets, num_sets, ids[i]);
    if (bit == ~static_cast<size_t>(0))
      continue; // Unknown ID — skip.
    size_t word = bit / (sizeof(unsigned long) * 8);
    size_t bit_in_word = bit % (sizeof(unsigned long) * 8);
    if (word < cpuset_size / sizeof(unsigned long))
      mask->__mask[word] |= (1UL << bit_in_word);
  }
  return true;
}

} // namespace

intptr_t sched_getaffinity(pid_t tid, size_t cpuset_size, cpu_set_t *mask) {
  if (!mask || cpuset_size == 0)
    return -EINVAL;

  __builtin_memset(mask, 0, cpuset_size);

  bool is_self =
      (tid == 0 || static_cast<DWORD>(tid) == ::NtCurrentThreadId());

  if (!is_self) {
    // Foreign thread: try ThreadSelectedCpuSets for multi-group support,
    // fall back to ThreadGroupInformation if unrestricted.
    windows::ScopedNtHandle handle;
    NTSTATUS status = sched_impl::open_thread_by_tid(
        static_cast<DWORD>(tid), THREAD_QUERY_INFORMATION, handle.put());
    if (!NT_SUCCESS(status))
      return (status == STATUS_ACCESS_DENIED) ? -EPERM : -ESRCH;

    bool has_cpu_sets = fill_thread_cpu_sets(handle.get(), mask, cpuset_size);
    if (has_cpu_sets)
      return 0;

    // No explicit CPU sets — fall back to single-group affinity query.
    GROUP_AFFINITY aff{};
    ULONG ret_len;
    status = ::NtQueryInformationThread(
        handle.get(), ThreadGroupInformation, &aff, sizeof(aff), &ret_len);

    if (!NT_SUCCESS(status))
      return -EINVAL;

    write_group_affinity(mask, cpuset_size, aff.Group, aff.Mask);
    return 0;
  }

  // Current thread: try ThreadSelectedCpuSets first.
  if (fill_thread_cpu_sets(NtCurrentThread(), mask, cpuset_size))
    return 0;

  // No explicit CPU sets — return process-wide allowed CPU set across all
  // groups. This matches Linux's sched_getaffinity(0) returning cpus_allowed.
  if (fill_process_affinity(mask, cpuset_size))
    return 0;

  // Fallback: single-group query on current thread.
  GROUP_AFFINITY aff{};
  ULONG ret_len;
  NTSTATUS status = ::NtQueryInformationThread(
      NtCurrentThread(), ThreadGroupInformation,
      &aff, sizeof(aff), &ret_len);

  if (!NT_SUCCESS(status))
    return -EINVAL;

  write_group_affinity(mask, cpuset_size, aff.Group, aff.Mask);
  return 0;
}

intptr_t sched_setaffinity(pid_t tid, size_t cpuset_size, const cpu_set_t *mask) {
  if (!mask || cpuset_size == 0)
    return -EINVAL;

  // Enumerate system CPU sets to map bit positions → CPU set IDs.
  auto sets_s = internal::byte_scratch(sched_impl::MAX_CPU_SETS * sizeof(SYSTEM_CPU_SET_INFORMATION));
  if (!sets_s)
    return -ENOMEM;
  auto *sets = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION *>(sets_s.data());
  size_t num_sets =
      sched_impl::enumerate_cpu_sets(sets, sched_impl::MAX_CPU_SETS);
  if (num_sets == 0)
    return -ENOSYS;

  // Walk cpu_set_t bits, collect matching CPU set IDs.
  ULONG ids[sched_impl::MAX_CPU_SETS];
  ULONG id_count = 0;
  size_t total_bits = cpuset_size * 8;

  for (size_t bit = 0; bit < total_bits && id_count < sched_impl::MAX_CPU_SETS;
       ++bit) {
    size_t word = bit / (sizeof(unsigned long) * 8);
    size_t bit_in_word = bit % (sizeof(unsigned long) * 8);
    if (word < cpuset_size / sizeof(unsigned long) &&
        (mask->__mask[word] & (1UL << bit_in_word))) {
      ULONG id = sched_impl::bit_to_cpu_set_id(sets, num_sets, bit);
      if (id == 0)
        return -EINVAL;
      ids[id_count++] = id;
    }
  }

  if (id_count == 0)
    return -EINVAL;

  bool is_self =
      (tid == 0 || static_cast<DWORD>(tid) == ::NtCurrentThreadId());

  HANDLE handle = ::NtCurrentThread();
  bool opened = false;

  if (!is_self) {
    NTSTATUS status = sched_impl::open_thread_by_tid(
        static_cast<DWORD>(tid), THREAD_SET_INFORMATION, &handle);
    if (!NT_SUCCESS(status))
      return (status == STATUS_ACCESS_DENIED) ? -EPERM : -ESRCH;
    opened = true;
  }

  NTSTATUS status = ::NtSetInformationThread(
      handle, ThreadSelectedCpuSets, ids, id_count * sizeof(ULONG));

  if (opened)
    ::NtClose(handle);

  if (!NT_SUCCESS(status))
    return (status == STATUS_ACCESS_DENIED) ? -EPERM : -EINVAL;

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
