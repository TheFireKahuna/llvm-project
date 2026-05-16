//===---------- Windows mincore engine (kernel function) -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX mincore - query page residency in physical memory.
//
// Windows has no direct mincore equivalent. This implementation uses
// NtQueryVirtualMemory(MemoryWorkingSetExInformation) to query per-page
// physical memory residency.
//
// A page is reported as resident (1) when either:
//   - Valid=1: page is in the process working set (actively mapped), or
//   - Valid=0, Location=MemoryLocationResident: page has been trimmed from the
//     working set but is still in RAM on the standby or modified list.
//
// This closely matches Linux mincore, which reports 1 for pages in the system
// page cache regardless of whether any process has them mapped.
//
// The implementation batches queries (256 entries) to amortize the cost of
// NtQueryVirtualMemory syscalls across large ranges. 256 entries at 16 bytes
// each = 4KB, fitting a single page on the stack.
//
//===----------------------------------------------------------------------===//

#include "mincore_ops.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stdint.h> // UINTPTR_MAX

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t mincore(void *addr, size_t len, unsigned char *vec) {
  // POSIX: EFAULT when vec points to an illegal address. Match Linux.
  if (LIBC_UNLIKELY(!vec))
    return -EFAULT;

  // Linux returns EINVAL for unaligned addr, ENOMEM for NULL (unmapped).
  // Let NULL fall through to the MEM_FREE check for ENOMEM consistency.
  if (LIBC_UNLIKELY(addr && !windows::is_page_aligned(addr)))
    return -EINVAL;

  if (len == 0)
    return 0;

  const SIZE_T page_size = windows::get_page_size();
  const SIZE_T rounded_len = windows::round_to_page(len);
  if (LIBC_UNLIKELY(rounded_len == 0))
    return -ENOMEM;

  // Guard against pointer overflow before entering the region loop.
  uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
  if (LIBC_UNLIKELY(addr_val > UINTPTR_MAX - rounded_len))
    return -ENOMEM;

  HANDLE current_process = NtCurrentProcess();

  // Walk regions and query working set in batches. For each region:
  //   MEM_FREE     -> ENOMEM (matches Linux for unmapped addresses)
  //   MEM_RESERVE  -> fill zeros (uncommitted pages can't be resident)
  //   MEM_COMMIT   -> batch-query MemoryWorkingSetExInformation
  //
  // 256 entries * 16 bytes = 4KB stack allocation.
  constexpr SIZE_T MAX_BATCH = 256;
  MEMORY_WORKING_SET_EX_INFORMATION ws_info[MAX_BATCH];

  auto ws = windows::byte_scratch(4096);
  if (!ws) return -ENOMEM;
  nt_pal::RegionWalker walk(addr, rounded_len, ws.data(), ws.size());
  SIZE_T vec_index = 0;

  while (walk.next()) {
    if (LIBC_UNLIKELY(walk.entry->State == MEM_FREE))
      return -ENOMEM;

    SIZE_T chunk_pages = walk.chunk_size / page_size;

    // Reserved (uncommitted) pages cannot be in the working set.
    if (walk.entry->State == MEM_RESERVE) {
      for (SIZE_T i = 0; i < chunk_pages; ++i)
        vec[vec_index++] = 0;
      continue;
    }

    // Committed pages: query working set residency in batches.
    SIZE_T pages_done = 0;
    while (pages_done < chunk_pages) {
      SIZE_T batch_size = chunk_pages - pages_done;
      if (batch_size > MAX_BATCH)
        batch_size = MAX_BATCH;

      for (SIZE_T i = 0; i < batch_size; ++i) {
        ws_info[i].VirtualAddress =
            walk.chunk + ((pages_done + i) * page_size);
        ws_info[i].VirtualAttributes.Flags = 0;
      }

      NTSTATUS status = ::NtQueryVirtualMemory(
          current_process, nullptr, MemoryWorkingSetExInformation, ws_info,
          batch_size * sizeof(MEMORY_WORKING_SET_EX_INFORMATION), nullptr);

      if (LIBC_UNLIKELY(NT_ERROR(status)))
        return -static_cast<intptr_t>(windows_util::ntstatus_to_errno(status));

      for (SIZE_T i = 0; i < batch_size; ++i) {
        auto &attr = ws_info[i].VirtualAttributes;
        if (attr.Valid)
          vec[vec_index++] = 1;
        else
          vec[vec_index++] =
              (attr.Invalid.Location == MemoryLocationResident) ? 1 : 0;
      }

      pages_done += batch_size;
    }
  }

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
