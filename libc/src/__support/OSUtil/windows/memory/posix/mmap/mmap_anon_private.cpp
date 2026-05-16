//===- mmap_anon_private.cpp - anonymous-private mmap on the tracker ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
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
  // Re-round even though `mmap_entry` already did — direct callers (tests,
  // future shape-private dispatchers) bypass that path.
  const size_t rounded_size = mp::rounded_len_or_zero(size);
  if (LIBC_UNLIKELY(rounded_size == 0))
    return -ENOMEM;

  // All three acquire variants below internally split the rounded-up 64 KiB
  // alloc-granule placeholder at the user boundary and release the pad to
  // MEM_FREE, so a sub-granule request commits only its page-aligned bytes
  // with no lifetime waste.

  const vt::AcquireMeta meta = mp::anon_private_meta(prot, flags);

  if (flags & MAP_32BIT) {
    auto chosen = vt::acquire_kernel_chosen_32bit(
        rounded_size, vt::RegionKind::AnonPrivate, meta);
    if (!chosen.has_value())
      return -chosen.error();
    return reinterpret_cast<intptr_t>(chosen.value());
  }

  // Hint path. `vt::acquire` shaves the prefix off the enclosing alloc
  // granule when the hint is page-aligned but not granule-aligned, so the
  // caller's exact page-aligned address is honoured. A collision returns
  // EEXIST and we fall through to the kernel-chosen path — preserving the
  // hint is best-effort, useful for stack-grow-adjacent placement.
  if (addr != nullptr) {
    vt::VaRange range = mp::make_range(addr, rounded_size);
    auto ref = vt::acquire(range, vt::RegionKind::AnonPrivate, meta);
    if (ref.has_value())
      return reinterpret_cast<intptr_t>(addr);
    int e = ref.error();
    if (e != EEXIST)
      return -e;
  }

  // No hint or hint-collision retry: substrate scouts a placeholder before
  // the commit envelope runs, so the VA stays MEM_RESERVE (never MEM_FREE)
  // between scout and commit — a POSIX-side scout / release / re-reserve
  // loop would race here.
  auto chosen =
      vt::acquire_kernel_chosen(rounded_size, vt::RegionKind::AnonPrivate,
                                meta);
  if (!chosen.has_value())
    return -chosen.error();
  return reinterpret_cast<intptr_t>(chosen.value());
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
