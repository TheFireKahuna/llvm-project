//===- mincore.cpp - POSIX mincore on the read-only nt_pal surface --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// POSIX `mincore(addr, len, vec)` — per-page residency probe.
///
/// NT has no direct equivalent. The body issues batched
/// `MemoryWorkingSetExInformation` queries (`nt_pal::query_working_set_ex`)
/// and reports a page as resident when either Valid is set or the page sits
/// on the standby / modified list (Invalid.Location ==
/// MemoryLocationResident). That matches the Linux page-cache definition
/// reasonably closely; programs portable to both systems test `vec[i] & 1`.
///
/// The structural test (POSIX_ROADMAP §2.1) for this file: validation
/// block + `RegionWalker` loop + per-chunk
/// `nt_pal::query_working_set_ex` + errno return. Nothing else.
///
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mincore.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
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

  // POSIX/Linux: vec must be writable. NULL vec is the only path that
  // returns EFAULT; NULL addr falls through to the MEM_FREE check so
  // unmapped addresses surface as ENOMEM regardless of `vec`.
  if (LIBC_UNLIKELY(vec == nullptr))
    return -EFAULT;

  // Unaligned non-null addr is a Linux-defined EINVAL. NULL is allowed
  // here so the MEM_FREE branch below produces ENOMEM.
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

  // 256 entries × MEMORY_WORKING_SET_EX_INFORMATION (16 B) = 4 KiB —
  // one stack page, no scratch allocation.
  constexpr SIZE_T kMaxBatch = 256;
  MEMORY_WORKING_SET_EX_INFORMATION ws_info[kMaxBatch];

  // RegionWalker stitches the kernel's MBI snapshot across the
  // requested span; a single contiguous `vec_index` accumulates output
  // across chunks so multi-VAD ranges produce one continuous vector.
  ::LIBC_NAMESPACE::nt_pal::RegionWalker walk(addr, rounded_len);
  if (!walk)
    return -ENOMEM;

  const SIZE_T page_size = ::LIBC_NAMESPACE::windows::get_page_size();
  size_t vec_index = 0;

  while (walk.next()) {
    // Mid-walk MEM_FREE is a hard error per Linux mincore — partial
    // residency reporting would mislead the caller about the boundary.
    if (LIBC_UNLIKELY(walk.entry->State == MEM_FREE))
      return -ENOMEM;

    const SIZE_T chunk_pages = walk.chunk_size / page_size;

    // MEM_RESERVE cannot have resident pages — uncommitted VA has no
    // backing PTE.
    if (walk.entry->State == MEM_RESERVE) {
      for (SIZE_T i = 0; i < chunk_pages; ++i)
        vec[vec_index++] = 0;
      continue;
    }

    // MEM_COMMIT: batch-query in 256-page slices.
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

      if (LIBC_UNLIKELY(!::LIBC_NAMESPACE::nt_pal::query_working_set_ex(
              ws_info, batch))) {
        // Treat any kernel-side query failure as a generic ENOMEM —
        // mincore has no errno more specific than "the range is not
        // queryable as resident memory."
        return -ENOMEM;
      }

      for (SIZE_T i = 0; i < batch; ++i) {
        const auto &attr = ws_info[i].VirtualAttributes;
        // Bit-0 only assignment. POSIX reserves bits 1..7; OR-ing or
        // overwriting the whole byte breaks programs that pre-poison
        // the buffer to detect mincore-skipped entries.
        const bool resident =
            attr.Valid ||
            (attr.Invalid.Location == MemoryLocationResident);
        vec[vec_index++] = resident ? 1 : 0;
      }

      pages_done += batch;
    }
  }

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
