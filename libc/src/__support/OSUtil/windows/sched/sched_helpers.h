//===-- Windows sched helper functions ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SCHED_SCHED_HELPERS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SCHED_SCHED_HELPERS_H

#include "hdr/sched_macros.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace sched_impl {

// Map POSIX priority (1..99) to NT base priority increment (-2..+2, 15).
// SCHED_OTHER always uses 0. For FIFO/RR we spread 1..99 across five Windows
// priority levels, with 99 mapping to TIME_CRITICAL (+15).
LIBC_INLINE KPRIORITY posix_to_nt_priority(int posix_prio) {
  if (posix_prio <= 0)
    return 0;
  if (posix_prio >= 99)
    return 15; // TIME_CRITICAL
  if (posix_prio >= 75)
    return 2;  // HIGHEST
  if (posix_prio >= 51)
    return 1;  // ABOVE_NORMAL
  if (posix_prio >= 25)
    return 0;  // NORMAL
  return posix_prio >= 1 ? -2 : 0; // LOWEST for 1..24
}

// Reverse map: NT base priority → representative POSIX priority.
LIBC_INLINE int nt_to_posix_priority(KPRIORITY nt_prio) {
  if (nt_prio <= -2)
    return 12;
  if (nt_prio == -1)
    return 37;
  if (nt_prio == 0)
    return 50;
  if (nt_prio == 1)
    return 63;
  if (nt_prio == 2)
    return 87;
  return 99; // TIME_CRITICAL or above
}

// Open a thread by TID for information queries/sets.
LIBC_INLINE NTSTATUS open_thread_by_tid(DWORD tid, ACCESS_MASK access,
                                        HANDLE *out) {
  CLIENT_ID cid{nullptr,
                reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(tid))};
  return ::NtOpenThread(out, access, nullptr, &cid);
}

// Maximum logical processors supported for CPU set enumeration.
// 4 processor groups × 64 LPs each = 256 (Windows maximum as of Win11).
inline constexpr size_t MAX_CPU_SETS = 256;

// Enumerate all CPU sets in the system for the current process.
//
// Calls NtQuerySystemInformationEx(SystemCpuSetInformation, &process_handle)
// to retrieve one SYSTEM_CPU_SET_INFORMATION per logical processor. The
// AllocatedToTargetProcess flag in each entry reflects the current process.
//
// out:         caller buffer, should be at least MAX_CPU_SETS entries.
// max_entries: capacity of out[].
// Returns:     number of entries written, or 0 on failure (buffer overflow
//              or NT call failure). Typical failure: >256 LPs (future HW).
LIBC_INLINE size_t enumerate_cpu_sets(SYSTEM_CPU_SET_INFORMATION *out,
                                      size_t max_entries) {
  // The input buffer for SystemCpuSetInformation is a pointer to a HANDLE
  // identifying the target process. NtCurrentProcess() = current.
  HANDLE process = NtCurrentProcess();
  ULONG buf_len = 0;

  // Probe for required buffer size. Returns STATUS_INFO_LENGTH_MISMATCH
  // and writes the needed size into buf_len.
  ::NtQuerySystemInformationEx(SystemCpuSetInformation, &process,
                               sizeof(process), nullptr, 0, &buf_len);
  if (buf_len == 0)
    return 0;

  // Scratch buffer — each entry is ~32 bytes, 256 × 32 = 8KB.
  auto ss = internal::byte_scratch(MAX_CPU_SETS * sizeof(SYSTEM_CPU_SET_INFORMATION));
  if (!ss)
    return 0;
  if (buf_len > ss.size())
    return 0;

  NTSTATUS status = ::NtQuerySystemInformationEx(
      SystemCpuSetInformation, &process, sizeof(process), ss.data(), buf_len,
      &buf_len);
  if (!NT_SUCCESS(status))
    return 0;

  // Entries are variable-sized (walk via entry->Size). Copy into flat array
  // for random access by the bit↔ID conversion helpers below.
  size_t count = 0;
  for (ULONG off = 0; off < buf_len && count < max_entries;) {
    auto *entry =
        reinterpret_cast<SYSTEM_CPU_SET_INFORMATION *>(ss.data() + off);
    if (entry->Size == 0)
      break;
    out[count++] = *entry;
    off += entry->Size;
  }
  return count;
}

// Map a cpu_set_t bit position to a Windows CPU set ID.
//
// Bit layout: bit = Group * 64 + LogicalProcessorIndex.
// Scans the enumerated table for a matching (Group, LogicalProcessorIndex)
// and returns its CpuSet.Id. Returns 0 if no match (offline/non-existent LP).
//
// CPU set IDs are opaque kernel-assigned values (typically starting at 256).
// They are stable for the lifetime of the OS boot but not across reboots.
LIBC_INLINE ULONG bit_to_cpu_set_id(const SYSTEM_CPU_SET_INFORMATION *sets,
                                    size_t count, size_t bit_index) {
  WORD group = static_cast<WORD>(bit_index / 64);
  BYTE logical_index = static_cast<BYTE>(bit_index % 64);
  for (size_t i = 0; i < count; ++i) {
    if (sets[i].CpuSet.Group == group &&
        sets[i].CpuSet.LogicalProcessorIndex == logical_index)
      return sets[i].CpuSet.Id;
  }
  return 0;
}

// Map a Windows CPU set ID back to a cpu_set_t bit position.
//
// Scans the enumerated table for a matching CpuSet.Id and returns
// Group * 64 + LogicalProcessorIndex. Returns SIZE_MAX if the ID is
// unknown (should not happen with a freshly enumerated table).
LIBC_INLINE size_t cpu_set_id_to_bit(const SYSTEM_CPU_SET_INFORMATION *sets,
                                     size_t count, ULONG id) {
  for (size_t i = 0; i < count; ++i) {
    if (sets[i].CpuSet.Id == id)
      return static_cast<size_t>(sets[i].CpuSet.Group) * 64 +
             sets[i].CpuSet.LogicalProcessorIndex;
  }
  return ~static_cast<size_t>(0);
}

} // namespace sched_impl
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SCHED_SCHED_HELPERS_H
