//===- mlock.cpp - POSIX mlock/mlock2/munlock on the read-only nt_pal -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// POSIX `mlock(addr, len)`, `mlock2(addr, len, flags)`, and
/// `munlock(addr, len)` on the read-only `nt_pal::lock_range` /
/// `nt_pal::unlock_range` surface.
///
/// All three return 0 on success and a Linux-flavoured -errno on
/// failure. Page-rounding mirrors Linux: `addr` rounds down, `addr+len`
/// determines the upper bound, and the range is rounded up to the next
/// page. Two distinct overflow checks (pre-round wraparound and
/// post-round zero-result) are both load-bearing — the pre-round catch
/// only sees the simple `addr + len < addr` case.
///
/// `mlock` and `mlock2(0)` walk the requested range region by region
/// (`nt_pal::RegionWalker`), pre-expanding the working-set quota for
/// the full range and then calling the per-chunk `lock_chunk` helper
/// with the 3-retry `STATUS_WORKING_SET_QUOTA` loop. A first
/// unlockable region is hard-failed with `ENOMEM` (Linux semantic — no
/// rollback on partial success).
///
/// `mlock2(MLOCK_ONFAULT)` pre-expands the quota (the VEH handler that
/// will lock pages on fault still hits the quota when faulting in)
/// then arms the onfault range table; no immediate lock. Returns
/// success even when zero pages are currently faulted in.
///
/// `munlock` disarms any onfault tracking BEFORE the unlock walk —
/// otherwise the VEH would keep firing on already-released pages. The
/// per-chunk unlock tolerates `STATUS_NOT_LOCKED` (POSIX permits
/// munlock on never-locked pages); non-lockable regions
/// (`PAGE_NOACCESS`, uncommitted) are skipped silently because they
/// could not have been locked in the first place.
///
/// `mlockall` and `munlockall` remain in the legacy implementation;
/// process-wide bookkeeping (`g_mcl_flags`, the buffer-driven
/// VA walk) is rebuilt in P9.
///
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mlock.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
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

/// Round a POSIX `(addr, len)` pair to a kernel-friendly
/// `(start, rounded_len)`. Encodes the two-tier overflow check the
/// per-op entries share.
///
/// On overflow at either tier `*rounded_out` is left unchanged and the
/// helper returns `ENOMEM`. On success returns 0 and writes the page-
/// rounded start and length into `*start_out` / `*rounded_out`.
LIBC_INLINE int round_lock_range(const void *addr, size_t len,
                                 uintptr_t *start_out, size_t *rounded_out) {
  const size_t page_size = ::LIBC_NAMESPACE::windows::get_page_size();
  const uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
  const uintptr_t start = addr_val & ~(page_size - 1);
  const uintptr_t original_end = addr_val + len;

  // Pre-round overflow: addr + len wrapped past UINTPTR_MAX. The
  // post-round check below catches the much rarer case where `len` is
  // small enough not to wrap but the rounded range still exceeds the
  // address space.
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

/// Lock a single region with the legacy 3-retry quota-expansion loop.
/// Returns 0 on success or a Linux-flavoured -errno value:
///
///   * `-EPERM` from `STATUS_ACCESS_DENIED` — RLIMIT_MEMLOCK-style
///      failure. Generic `EACCES` would misread as a filesystem
///      permission error.
///   * `-ENOMEM` when `expand_working_set` itself fails — distinct
///      from `EAGAIN` because expansion failure means we're out of
///      headroom, not that the lock might succeed on retry.
///   * `-EAGAIN` when expansion succeeded each round but the lock
///      kept hitting `STATUS_WORKING_SET_QUOTA` — POSIX semantic for
///      transient lock failure.
///
/// 3 attempts is the Linux-historical retry budget; raising it papers
/// over genuine quota saturation, lowering it surfaces transient
/// contention as hard failure.
LIBC_INLINE intptr_t lock_chunk(void *addr, SIZE_T size) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::lock_range(addr, size);
    if (NT_SUCCESS(st))
      return 0;

    if (st == STATUS_WORKING_SET_QUOTA) {
      if (::LIBC_NAMESPACE::nt_pal::expand_working_set(NtCurrentProcess(),
                                                        size))
        continue;
      return -ENOMEM;
    }

    if (st == STATUS_ACCESS_DENIED)
      return -EPERM;
    return -static_cast<intptr_t>(
        ::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(st));
  }
  return -EAGAIN;
}

/// Walk-and-lock the range. Hard-fails on the first unlockable region
/// (matches Linux `mlock`, distinct from `mlockall`'s best-effort
/// behaviour). No rollback on per-region success-then-fail; partial
/// progress stays locked, matching Linux.
LIBC_INLINE intptr_t walk_and_lock(void *start, SIZE_T size) {
  // Pre-expand the working-set quota for the full range so that the
  // per-chunk loop only retries on contention spikes, not on the
  // common case of locking a fresh allocation.
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
  // Glibc semantic: mlock(NULL, n) is ENOMEM, not EINVAL. Apps ported
  // from Linux test for ENOMEM here to discriminate "address not in
  // any mapping" from "argument shape wrong".
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
  // Forward-compat: reject any bit outside the documented ONFAULT
  // mask. Silent acceptance breaks future kernel additions that an app
  // may rely on for behaviour switching.
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
    // Pages are locked lazily by the memory subsystem's guard-page
    // filter (`mem_fault_handler.cpp::try_mlock_onfault`) on first
    // touch. Pre-expand the working-set quota so the filter doesn't
    // bounce on STATUS_WORKING_SET_QUOTA when the first fault arrives.
    ::LIBC_NAMESPACE::nt_pal::expand_working_set(
        NtCurrentProcess(), static_cast<SIZE_T>(rounded_len));

    // Set the per-region MLOCK_ONFAULT bit on every tracked desc that
    // intersects the range. The substrate's mutate envelope serialises
    // the publish window; on return every reader of these descs sees
    // the bit set. A range that touches no tracked region (caller
    // armed onfault on a foreign / image / libc-internal VA) finds
    // ENOENT — silently treat as success since the only observable
    // effect would have been a no-op anyway.
    int rc = vt::mutate(vt::VaRange{range_addr,
                                    static_cast<size_t>(rounded_len)},
                        &mp::lock_set_onfault_mutator,
                        /*ctx=*/nullptr,
                        /*prot_change=*/0);
    if (rc != 0 && rc != ENOENT)
      return -rc;

    // Arm one-shot guard traps on every committed page in the range
    // so already-resident pages also enter the filter on next access.
    // Best-effort per chunk.
    ::LIBC_NAMESPACE::nt_pal::arm_guard_trap(
        range_addr, static_cast<size_t>(rounded_len));
    return 0;
  }

  // flags == 0: identical to mlock.
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

  // Clear the per-region MLOCK_ONFAULT bit BEFORE the unlock walk —
  // otherwise the guard-page filter would re-lock pages that the
  // loop below has just unlocked, on the next access. PAGE_GUARD
  // residue on already-armed pages is harmless: the kernel auto-
  // clears PAGE_GUARD on fault dispatch, and with the flag now clear
  // the filter returns CONTINUE_SEARCH instead of re-locking.
  // ENOENT (no tracked desc in the range) is fine — there's nothing
  // to disarm; munlock on a never-armed range is POSIX-permitted.
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
    // Only attempt to unlock memory that could plausibly be locked.
    // PAGE_NOACCESS / uncommitted regions cannot hold lock state, so
    // skipping them silently is correct.
    if (!::LIBC_NAMESPACE::nt_pal::is_lockable(*walk.entry))
      continue;

    NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::unlock_range(walk.chunk,
                                                          walk.chunk_size);
    // POSIX explicitly permits munlock on memory that is not currently
    // locked. STATUS_NOT_LOCKED is the kernel's signal for that case.
    if (LIBC_UNLIKELY(NT_ERROR(st) && st != STATUS_NOT_LOCKED))
      return -ENOMEM;
  }
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
