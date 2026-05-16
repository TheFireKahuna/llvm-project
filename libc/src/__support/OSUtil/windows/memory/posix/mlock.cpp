//===- mlock.cpp - POSIX mlock/mlock2/munlock on the read-only nt_pal -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mlock.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/nt_pal/working_set.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_errno.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_mutators.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_validation.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/nt_pal/lock.h"
#include "src/__support/OSUtil/windows/nt_pal/protect.h"
#include "src/__support/OSUtil/windows/nt_pal/query.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {

namespace {

namespace mp = ::LIBC_NAMESPACE::windows::memory_posix;
namespace vt = ::LIBC_NAMESPACE::windows::va_tracker;

// Page-round a POSIX `(addr, len)` pair. Returns positive errno on
// overflow; outputs untouched on failure.
LIBC_INLINE int round_lock_range(const void *addr, size_t len,
                                 uintptr_t *start_out, size_t *rounded_out) {
  const size_t page_size = ::LIBC_NAMESPACE::windows::get_page_size();
  const uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
  const uintptr_t start = addr_val & ~(page_size - 1);
  const uintptr_t original_end = addr_val + len;

  // Two overflow tiers — both load-bearing. The pre-round check fires
  // only when `addr + len` wraps; the post-round check catches the
  // rarer case where the wrap doesn't happen but page-rounding pushes
  // the length past `SIZE_MAX`.
  if (LIBC_UNLIKELY(original_end < addr_val))
    return ENOMEM;
  const size_t adjusted_len = static_cast<size_t>(original_end - start);
  const size_t rounded = mp::rounded_len_or_zero(adjusted_len);
  if (LIBC_UNLIKELY(rounded == 0))
    return ENOMEM;

  *start_out = start;
  *rounded_out = rounded;
  return 0;
}

// Lock one MBI region with 3 quota-expansion retries. Returns 0 or a
// negative errno. STATUS_ACCESS_DENIED maps to EPERM (RLIMIT_MEMLOCK,
// not a filesystem ACL — a portable app testing EACCES would chase the
// wrong cause). STATUS_WORKING_SET_QUOTA only escapes as -EAGAIN once
// the retry budget is exhausted; mid-loop it triggers expansion.
// 3 attempts is the historical Linux budget — raising it papers over
// genuine saturation, lowering it surfaces transient contention.
LIBC_INLINE intptr_t lock_chunk(void *addr, SIZE_T size) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::lock_range(addr, size);
    if (NT_SUCCESS(st))
      return 0;

    if (st == STATUS_WORKING_SET_QUOTA) {
      if (::LIBC_NAMESPACE::nt_pal::expand_working_set(NtCurrentProcess(),
                                                        size))
        continue;
      // Expansion failure means no headroom, not transient contention;
      // ENOMEM rather than EAGAIN.
      return -ENOMEM;
    }

    if (st == STATUS_ACCESS_DENIED)
      return -EPERM;
    return -static_cast<intptr_t>(
        ::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(st));
  }
  return -EAGAIN;
}

// Walk-and-lock. Hard-fails on the first unlockable region (Linux
// `mlock` semantic; distinct from `mlockall`'s best-effort). No
// rollback on partial success — that too matches Linux.
LIBC_INLINE intptr_t walk_and_lock(void *start, SIZE_T size) {
  // Pre-expand the quota for the full range so the per-chunk loop only
  // retries on contention spikes, not on the common fresh-allocation
  // path.
  ::LIBC_NAMESPACE::nt_pal::expand_working_set(NtCurrentProcess(), size);

  ::LIBC_NAMESPACE::nt_pal::RegionWalker walk(start, size);
  if (!walk)
    return -ENOMEM;

  while (walk.next()) {
    if (!::LIBC_NAMESPACE::nt_pal::is_lockable(*walk.entry))
      return -ENOMEM;
    if (intptr_t r = lock_chunk(walk.chunk, walk.chunk_size); r < 0)
      return r;
  }
  return 0;
}

} // namespace

namespace internal {

intptr_t mlock(const void *addr, size_t len) {
  // Glibc semantic: mlock(NULL, n) is ENOMEM, not EINVAL. Linux-ported
  // apps test ENOMEM here to discriminate "address not mapped" from
  // "argument shape wrong".
  if (LIBC_UNLIKELY(addr == nullptr))
    return -ENOMEM;
  if (len == 0)
    return 0;

  uintptr_t start;
  size_t rounded_len;
  if (int e = round_lock_range(addr, len, &start, &rounded_len); e != 0)
    return -e;

  return walk_and_lock(reinterpret_cast<void *>(start),
                       static_cast<SIZE_T>(rounded_len));
}

intptr_t mlock2(const void *addr, size_t len, int flags) {
  // Reject unknown bits — silent acceptance would lock apps into
  // wrong-behaviour assumptions about future kernel additions.
  if (LIBC_UNLIKELY((flags & ~MLOCK_ONFAULT) != 0))
    return -EINVAL;
  if (LIBC_UNLIKELY(addr == nullptr))
    return -ENOMEM;
  if (len == 0)
    return 0;

  uintptr_t start;
  size_t rounded_len;
  if (int e = round_lock_range(addr, len, &start, &rounded_len); e != 0)
    return -e;

  void *range_addr = reinterpret_cast<void *>(start);

  if (flags & MLOCK_ONFAULT) {
    // The VEH guard-page filter (`try_mlock_onfault` in
    // `mem_fault_handler.cpp`) does the actual lock on first touch.
    // Pre-expand here so the filter doesn't bounce on
    // STATUS_WORKING_SET_QUOTA when the first fault lands.
    ::LIBC_NAMESPACE::nt_pal::expand_working_set(
        NtCurrentProcess(), static_cast<SIZE_T>(rounded_len));

    // Set `region_flag::LOCK_ONFAULT` on every tracked desc the range
    // intersects; the substrate's mutate envelope publishes atomically.
    // A range hitting no tracked region (foreign / image / libc-
    // internal VA) returns ENOENT — silent success: there's no desc to
    // arm and no observable effect to deliver anyway.
    int rc = vt::mutate(vt::VaRange{range_addr,
                                    static_cast<size_t>(rounded_len)},
                        &mp::lock_set_onfault_mutator,
                        /*ctx=*/nullptr,
                        /*prot_change=*/0);
    if (rc != 0 && rc != ENOENT)
      return -rc;

    // OR PAGE_GUARD into every committed page so already-resident
    // pages also trip into the filter on next access.
    ::LIBC_NAMESPACE::nt_pal::arm_guard_trap(
        range_addr, static_cast<size_t>(rounded_len));
    return 0;
  }

  return walk_and_lock(range_addr, static_cast<SIZE_T>(rounded_len));
}

intptr_t munlock(const void *addr, size_t len) {
  if (LIBC_UNLIKELY(addr == nullptr))
    return -ENOMEM;
  if (len == 0)
    return 0;

  uintptr_t start;
  size_t rounded_len;
  if (int e = round_lock_range(addr, len, &start, &rounded_len); e != 0)
    return -e;

  void *range_addr = reinterpret_cast<void *>(start);

  // Clear `region_flag::LOCK_ONFAULT` BEFORE the unlock walk —
  // otherwise the guard-page filter would re-lock pages on next access
  // after we just unlocked them. PAGE_GUARD residue is harmless: the
  // kernel auto-clears it on fault dispatch, and with the flag down
  // the filter returns CONTINUE_SEARCH. ENOENT (range tracks no desc)
  // is fine — POSIX permits munlock on never-armed memory.
  (void)vt::mutate(vt::VaRange{range_addr,
                               static_cast<size_t>(rounded_len)},
                   &mp::lock_clear_mutator,
                   /*ctx=*/nullptr,
                   /*prot_change=*/0);

  auto ws = ::LIBC_NAMESPACE::windows::byte_scratch(4096);
  if (!ws)
    return -ENOMEM;
  ::LIBC_NAMESPACE::nt_pal::RegionWalker walk(
      range_addr, static_cast<SIZE_T>(rounded_len), ws.data(), ws.size());

  while (walk.next()) {
    // PAGE_NOACCESS / uncommitted regions can't hold lock state, so
    // skipping them silently is correct.
    if (!::LIBC_NAMESPACE::nt_pal::is_lockable(*walk.entry))
      continue;

    NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::unlock_range(walk.chunk,
                                                          walk.chunk_size);
    // POSIX explicitly permits munlock on memory that isn't locked.
    if (LIBC_UNLIKELY(NT_ERROR(st) && st != STATUS_NOT_LOCKED))
      return -ENOMEM;
  }
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
