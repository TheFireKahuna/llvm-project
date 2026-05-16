//===- munmap.cpp - POSIX munmap on the va_tracker -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Entry validation, cordon probe, then a single `va_tracker::release` call.
// The substrate owns edge-straddler split (atomic under its per-arena LOCKED
// envelope), per-desc teardown ordering, placeholder freeing, and
// hole-tolerant iteration — the POSIX layer adds no orchestration of its own.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/munmap.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/pagemap_classifier.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_meta.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_validation.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

namespace mp = ::LIBC_NAMESPACE::windows::memory_posix;
namespace vt = ::LIBC_NAMESPACE::windows::va_tracker;

} // namespace

namespace internal {

intptr_t munmap(void *addr, size_t size) {
  // Zero length is EINVAL, not the success no-op POSIX allows for length-zero
  // mincore/msync. Matches Linux glibc.
  if (LIBC_UNLIKELY(size == 0))
    return -EINVAL;
  if (LIBC_UNLIKELY(addr == nullptr))
    return -EINVAL;
  if (LIBC_UNLIKELY(!mp::is_page_aligned(addr)))
    return -EINVAL;

  const size_t rounded = mp::rounded_len_or_zero(size);
  if (LIBC_UNLIKELY(rounded == 0))
    return -EINVAL;

  const uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
  if (LIBC_UNLIKELY(mp::addr_plus_len_overflows(addr_val, rounded)))
    return -EINVAL;

  // Cordon probe: loaded PE images, kernel-loaned VA (TEB/PEB/stack), and
  // foreign-injected ranges return positive errno here — flipped to negative
  // for the internal `-errno` convention. Wait-free; runs before any
  // destructive substrate call so a rejected range observes no state change.
  if (int e = ::LIBC_NAMESPACE::windows::alloc::pagemap::
          validate_map_fixed_target(addr, rounded);
      e != 0)
    return -e;

  // `va_tracker::release` accepts page-granular ranges and severs
  // edge-straddler descs atomically inside its LOCKED envelope; pre-splitting
  // here would reopen a window where a concurrent peer mutation between the
  // split and the release turns a benign no-op into a phantom EINVAL.
  // Returns positive errno; we flip to negative for the internal convention.
  vt::VaRange range = mp::make_range(addr, rounded);
  int rc = vt::release(range);
  if (LIBC_UNLIKELY(rc != 0))
    return -rc;

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
