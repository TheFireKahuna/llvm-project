//===---------- Windows posix_madvise engine (kernel function) -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX posix_madvise -- advisory memory hints per POSIX.1-2008.
//
// POSIX invariant: "posix_madvise() shall have no effect on the semantics of
// access to memory in the specified range" (POSIX.1-2008 S2.9.8). This rules
// out any operation that may alter page content, including MEM_RESET. All
// advice values here are implemented as pure performance hints only.
//
// Key differences from madvise():
//   - Returns error number directly (not -1 with errno)
//   - POSIX_MADV_DONTNEED uses priority demotion only -- no MEM_RESET
//     (glibc and musl make it a no-op for the same reason; we preserve the
//     eviction-pressure hint since it cannot cause data loss)
//   - Only POSIX-standard advice values are accepted
//
//===----------------------------------------------------------------------===//

#include "posix_madvise_ops.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_region.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stdint.h> // UINTPTR_MAX

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t posix_madvise(void *addr, size_t size, int advice) {
  if (LIBC_UNLIKELY(!addr))
    return -EINVAL;

  if (LIBC_UNLIKELY(!windows::is_page_aligned(addr)))
    return -EINVAL;

  if (size == 0)
    return 0;

  const SIZE_T rounded_size = windows::round_to_page(size);
  if (LIBC_UNLIKELY(rounded_size == 0))
    return -ENOMEM;

  // Guard against pointer overflow before entering the region loop.
  uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
  if (LIBC_UNLIKELY(addr_val > UINTPTR_MAX - rounded_size))
    return -ENOMEM;

  switch (advice) {
  // Purely advisory -- POSIX forbids any semantic effect on memory content.
  // MEM_RESET is intentionally absent (it may zero pages under pressure,
  // violating the POSIX invariant). Priority demotion to VERY_LOW is safe:
  // it only influences eviction order under memory pressure, not data.
  case POSIX_MADV_DONTNEED: {
    ULONG max_prio = windows::wsex_max_priority(addr, rounded_size);
    if (MEMORY_PRIORITY_VERY_LOW < max_prio) {
      MEMORY_PAGE_PRIORITY_INFORMATION info = {MEMORY_PRIORITY_VERY_LOW};
      nt_pal::for_committed_batched(addr, rounded_size,
                                     VmPagePriorityInformation, &info,
                                     sizeof(info));
    }
    return 0;
  }

  // Cancel prior POSIX_MADV_DONTNEED (MEM_RESET), then prefetch.
  // MEM_RESET_UNDO recovers MEM_PRIVATE pages still in RAM before the
  // prefetch warms the working set. Safe on non-reset pages (no-op).
  // Uses bulk VA walk (NtPssCaptureVaSpaceBulk via nt_pal::RegionWalker) —
  // 1.3-3.2x faster than iterative NtQueryVirtualMemory (RA14/Frontier 2).
  case POSIX_MADV_WILLNEED: {
    auto ws = windows::byte_scratch(4096);
    if (!ws) return -ENOMEM;
    nt_pal::RegionWalker walk(addr, rounded_size, ws.data(), ws.size());
    while (walk.next()) {
      if (walk.entry->State == MEM_COMMIT && walk.entry->Type == MEM_PRIVATE)
        nt_pal::vm_reset_undo(walk.chunk, walk.chunk_size);
    }
    nt_pal::prefetch_committed(addr, rounded_size);
    return 0;
  }

  // Access pattern hints via page eviction priority.
  // See madvise.cpp for rationale on priority level choices.
  case POSIX_MADV_SEQUENTIAL: {
    MEMORY_PAGE_PRIORITY_INFORMATION info = {MEMORY_PRIORITY_LOW};
    nt_pal::for_committed_batched(addr, rounded_size,
                                   VmPagePriorityInformation, &info,
                                   sizeof(info));
    return 0;
  }

  case POSIX_MADV_RANDOM: {
    MEMORY_PAGE_PRIORITY_INFORMATION info = {MEMORY_PRIORITY_BELOW_NORMAL};
    nt_pal::for_committed_batched(addr, rounded_size,
                                   VmPagePriorityInformation, &info,
                                   sizeof(info));
    return 0;
  }

  // Restore default eviction priority. No semantic effect on data.
  case POSIX_MADV_NORMAL: {
    MEMORY_PAGE_PRIORITY_INFORMATION info = {MEMORY_PRIORITY_NORMAL};
    nt_pal::for_committed_batched(addr, rounded_size,
                                   VmPagePriorityInformation, &info,
                                   sizeof(info));
    return 0;
  }

  default:
    return -EINVAL;
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
