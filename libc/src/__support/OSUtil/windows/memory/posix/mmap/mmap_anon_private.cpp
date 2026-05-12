//===- mmap_anon_private.cpp - anonymous-private mmap on the tracker ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Anonymous-private (`mmap(MAP_ANONYMOUS|MAP_PRIVATE, ...)`) lands here
// as one `va_tracker::acquire` call after the meta builder converts
// POSIX flags into substrate shape.
//
// The substrate's `acquire` reserves a placeholder at a specific VA
// and refuses any non-MEM_FREE base. POSIX accepts a soft hint, so a
// caller-supplied address may collide with existing VA — the kernel
// is asked to pick a base via `nt_pal::reserve_placeholder` first,
// the placeholder is released to recover MEM_FREE state, and the
// substrate's `acquire` re-reserves at that base. A brief race window
// between the release and the re-reserve can lose to a concurrent
// allocator; the loop retries a bounded number of times before
// surfacing `ENOMEM`.
//
// `MAP_32BIT` keeps the hint in the low 2 GiB by switching the
// scout reservation to `reserve_placeholder_32bit`; the
// `region_flag::LOW_32BIT` bit on the desc carries the constraint
// so a future mremap-grow preserves placement.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mmap/mmap.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_meta.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_validation.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

namespace mp = ::LIBC_NAMESPACE::windows::memory_posix;
namespace vt = ::LIBC_NAMESPACE::windows::va_tracker;

// Bounded retry budget for the scout-reserve / release / acquire
// race. Three attempts is the same budget the legacy mlock retry
// loop uses for transient kernel-quota contention — large enough to
// absorb concurrent allocator chatter, small enough that pathological
// VA pressure surfaces ENOMEM rather than spinning forever.
constexpr int kScoutRetryBudget = 3;

// Pick a MEM_FREE base for an anonymous-private mapping.
//
// The substrate's `acquire` op reserves a placeholder at the address
// it is handed and refuses non-MEM_FREE bases. The caller-supplied
// `hint` is page-aligned per POSIX but may collide with existing VA;
// `nt_pal::reserve_placeholder(hint, size)` lets the kernel choose a
// nearby free base when the hint is occupied. `MAP_32BIT` constrains
// the search to the low 2 GiB. Returns nullptr on exhaustion.
LIBC_INLINE void *scout_anon_base(void *hint, size_t size, int flags) {
  if (flags & MAP_32BIT) {
    // 32-bit reservation ignores the caller hint — the kernel must
    // pick a base below 4 GiB, and an out-of-range hint forces a
    // fallback that would silently widen the placement.
    (void)hint;
    return ::LIBC_NAMESPACE::nt_pal::reserve_placeholder_32bit(size);
  }
  return ::LIBC_NAMESPACE::nt_pal::reserve_placeholder(hint, size);
}

} // namespace

namespace internal {

intptr_t mmap_anon_private(void *addr, size_t size, int prot, int flags) {
  // The caller (mmap_entry) has already validated entry shape and
  // page-rounded `size`. Round again here so direct callers (tests,
  // future shape-private dispatchers) get the same protection.
  const size_t rounded_size = mp::rounded_len_or_zero(size);
  if (LIBC_UNLIKELY(rounded_size == 0))
    return -ENOMEM;

  // The substrate works at NT allocation granularity (64 KiB). A
  // caller request smaller than that consumes a full 64 KiB
  // placeholder; the difference is wasted by NT regardless of which
  // layer rounds.
  const uintptr_t kernel_bytes_raw =
      ::LIBC_NAMESPACE::windows::align_up_to_granularity(rounded_size);
  if (LIBC_UNLIKELY(kernel_bytes_raw == 0))
    return -ENOMEM;
  const size_t kernel_bytes = static_cast<size_t>(kernel_bytes_raw);

  // Caller hint, soft. The substrate refuses anything sub-granularity,
  // so non-64KiB hints align down here. Null in stays null.
  void *aligned_hint = addr;
  if (aligned_hint != nullptr && !mp::is_alloc_aligned(aligned_hint)) {
    uintptr_t down = ::LIBC_NAMESPACE::windows::align_down_to_granularity(
        reinterpret_cast<uintptr_t>(aligned_hint));
    aligned_hint = reinterpret_cast<void *>(down);
  }

  const vt::AcquireMeta meta = mp::anon_private_meta(prot, flags);

  for (int attempt = 0; attempt < kScoutRetryBudget; ++attempt) {
    void *scout = scout_anon_base(aligned_hint, kernel_bytes, flags);
    if (scout == nullptr)
      return -ENOMEM;

    if (LIBC_UNLIKELY(!::LIBC_NAMESPACE::nt_pal::free_placeholder(scout))) {
      // The scout placeholder MUST release cleanly — failure means a
      // kernel-side bug or a different allocator stole the VAD out
      // from under us. Either way the only safe move is to surface
      // ENOMEM; retrying would compound the leak.
      return -ENOMEM;
    }

    vt::VaRange range = mp::make_range(scout, kernel_bytes);
    auto ref = vt::acquire(range, vt::RegionKind::AnonPrivate, meta);
    if (ref.has_value())
      return reinterpret_cast<intptr_t>(scout);

    int e = ref.error();
    if (e != EEXIST)
      return -e;
    // A concurrent allocator claimed the scouted VA between the
    // release and the acquire. Retry; the next scout call will find
    // a different MEM_FREE base.
    aligned_hint = nullptr;
  }

  return -ENOMEM;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
