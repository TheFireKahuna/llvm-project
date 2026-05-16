//===- mincore.cpp - POSIX mincore on the read-only nt_pal surface --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// NT has no direct mincore. The body walks the requested span via
// `nt_pal::RegionWalker` and, per committed chunk, issues batched
// `MemoryWorkingSetExInformation` queries — a page counts as resident when
// the kernel reports Valid OR Invalid.Location == MemoryLocationResident
// (standby / modified list). That approximates the Linux page-cache
// definition; portable callers test `vec[i] & 1`.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mincore.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_validation.h"
#include "src/__support/OSUtil/windows/nt_pal/query.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t mincore(void *addr, size_t len, unsigned char *vec) {
  namespace mp = ::LIBC_NAMESPACE::windows::memory_posix;

  // EFAULT is reserved for the writable-vec check; null `addr` deliberately
  // falls through so the RegionWalker's MEM_FREE branch surfaces ENOMEM.
  if (LIBC_UNLIKELY(vec == nullptr))
    return -EFAULT;
  if (LIBC_UNLIKELY(addr != nullptr && !mp::is_page_aligned(addr)))
    return -EINVAL;

  if (len == 0)
    return 0;

  const size_t rounded_len = mp::rounded_len_or_zero(len);
  if (LIBC_UNLIKELY(rounded_len == 0))
    return -ENOMEM;
  if (LIBC_UNLIKELY(mp::addr_plus_len_overflows(
          reinterpret_cast<uintptr_t>(addr), rounded_len)))
    return -ENOMEM;

  // 256 * sizeof(MEMORY_WORKING_SET_EX_INFORMATION) = 4 KiB — one stack
  // page, no scratch allocation.
  constexpr SIZE_T kMaxBatch = 256;
  MEMORY_WORKING_SET_EX_INFORMATION ws_info[kMaxBatch];

  ::LIBC_NAMESPACE::nt_pal::RegionWalker walk(addr, rounded_len);
  if (!walk)
    return -ENOMEM;

  const SIZE_T page_size = ::LIBC_NAMESPACE::windows::get_page_size();
  size_t vec_index = 0;

  while (walk.next()) {
    // Linux mincore rejects ranges containing any unmapped VA — partial
    // residency reporting would silently mislead the caller about boundaries.
    if (LIBC_UNLIKELY(walk.entry->State == MEM_FREE))
      return -ENOMEM;

    const SIZE_T chunk_pages = walk.chunk_size / page_size;

    if (walk.entry->State == MEM_RESERVE) {
      for (SIZE_T i = 0; i < chunk_pages; ++i)
        vec[vec_index++] = 0;
      continue;
    }

    SIZE_T pages_done = 0;
    while (pages_done < chunk_pages) {
      SIZE_T batch = chunk_pages - pages_done;
      if (batch > kMaxBatch)
        batch = kMaxBatch;

      for (SIZE_T i = 0; i < batch; ++i) {
        ws_info[i].VirtualAddress =
            walk.chunk + ((pages_done + i) * page_size);
        ws_info[i].VirtualAttributes.Flags = 0;
      }

      // Any kernel-side query failure collapses to ENOMEM: POSIX mincore
      // has no errno more specific than "this range is not queryable as
      // resident memory," and inventing EFAULT/EINVAL here would mislead.
      if (LIBC_UNLIKELY(!::LIBC_NAMESPACE::nt_pal::query_working_set_ex(
              ws_info, batch)))
        return -ENOMEM;

      for (SIZE_T i = 0; i < batch; ++i) {
        const auto &attr = ws_info[i].VirtualAttributes;
        const bool resident =
            attr.Valid ||
            (attr.Invalid.Location == MemoryLocationResident);
        // Plain assignment of 0/1: POSIX reserves bits 1..7, and callers
        // pre-poison `vec` to detect mincore-skipped entries — OR-ing in
        // bit 0 or overwriting the full byte would defeat that diagnostic.
        vec[vec_index++] = resident ? 1 : 0;
      }

      pages_done += batch;
    }
  }

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
