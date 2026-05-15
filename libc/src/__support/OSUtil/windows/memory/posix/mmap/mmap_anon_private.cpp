//===- mmap_anon_private.cpp - anonymous-private mmap on the tracker ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Anonymous-private (`mmap(MAP_ANONYMOUS|MAP_PRIVATE, ...)`) lands here
// as one `va_tracker` call after the meta builder converts POSIX flags
// into substrate shape.
//
// Three paths, each a single substrate entry:
//   * Caller-supplied hint, alloc-aligned and likely MEM_FREE:
//     `va_tracker::acquire(VaRange{hint, len}, ...)` honours the hint
//     and fails fast with `EEXIST` on collision so the caller can
//     retry without a hint. Honouring the hint matters for
//     stack-grow-adjacent placement where the caller knows more
//     about the desired layout than the kernel does.
//   * `MAP_32BIT`: `va_tracker::acquire_kernel_chosen_32bit(len, ...)`
//     scouts a low-2-GiB base inside the substrate's LOCKED hold —
//     the placeholder is never observable as MEM_FREE between
//     reserve and commit.
//   * No hint, sub-granularity hint, or hint-collision retry:
//     `va_tracker::acquire_kernel_chosen(len, ...)` reserves at a
//     kernel-chosen MEM_FREE base under the same LOCKED hold.
//
// No `reserve_placeholder` / `free_placeholder` call appears here:
// every scout sits inside the substrate, so there is no
// reserve / release / re-reserve race window for any path.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mmap/mmap.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_meta.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_validation.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

namespace mp = ::LIBC_NAMESPACE::windows::memory_posix;
namespace vt = ::LIBC_NAMESPACE::windows::va_tracker;

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

  const vt::AcquireMeta meta = mp::anon_private_meta(prot, flags);

  if (flags & MAP_32BIT) {
    auto chosen = vt::acquire_kernel_chosen_32bit(
        kernel_bytes, vt::RegionKind::AnonPrivate, meta);
    if (!chosen.has_value())
      return -chosen.error();
    return reinterpret_cast<intptr_t>(chosen.value());
  }

  // Hint path: honour an alloc-granularity-aligned caller hint via the
  // regular `acquire`. A collision returns `EEXIST` and we fall
  // through to the kernel-chosen path. Sub-granularity hints skip
  // the honour attempt because the substrate refuses them and the
  // kernel-chosen path will pick a clean base.
  if (addr != nullptr && mp::is_alloc_aligned(addr)) {
    vt::VaRange range = mp::make_range(addr, kernel_bytes);
    auto ref = vt::acquire(range, vt::RegionKind::AnonPrivate, meta);
    if (ref.has_value())
      return reinterpret_cast<intptr_t>(addr);
    int e = ref.error();
    if (e != EEXIST)
      return -e;
  }

  // No hint (or hint collision): let the substrate scout under its
  // own LOCKED hold so no other POSIX-layer consumer can race the
  // reservation. Single envelope, single substrate-side reserve, no
  // retry loop. The chosen base is returned directly.
  auto chosen =
      vt::acquire_kernel_chosen(kernel_bytes, vt::RegionKind::AnonPrivate,
                                meta);
  if (!chosen.has_value())
    return -chosen.error();
  return reinterpret_cast<intptr_t>(chosen.value());
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
