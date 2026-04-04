//===---------- Windows mlock family engine (kernel functions) -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX mlock, mlock2, mlockall, munlock, munlockall implementation for
// Windows. All functions return 0 on success and -errno on failure.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/mlock_ops.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/memory/memory_lock_policy.h"
#include "src/__support/OSUtil/windows/alloc/placeholder_range.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/memory/working_set.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

/// Lock a single region via NtLockVirtualMemory with quota retry.
/// NtLockVirtualMemory returns STATUS_SUCCESS for already-locked pages.
/// Returns 0 on success, -errno on failure.
long lock_chunk(HANDLE process, void *addr, SIZE_T size) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    // NtLockVirtualMemory modifies these in-place — use local copies.
    PVOID base = addr;
    SIZE_T region_size = size;

    NTSTATUS status =
        ::NtLockVirtualMemory(process, &base, &region_size, MAP_PROCESS);

    if (NT_SUCCESS(status))
      return 0;

    if (status == STATUS_WORKING_SET_QUOTA) {
      if (windows::expand_working_set(process, size))
        continue;
      return -ENOMEM;
    }

    if (status == STATUS_ACCESS_DENIED)
      return -EPERM;
    else
      return -windows_util::ntstatus_to_errno(status);
  }
  // Quota expansion succeeded but locking still failed after retries.
  // POSIX: EAGAIN for transient locking failures.
  return -EAGAIN;
}

} // namespace

namespace internal {

intptr_t mlock(const void *addr, size_t len) {
  // POSIX: ENOMEM for addresses not part of the address space.
  // We also return ENOMEM for null, matching Linux behavior.
  if (LIBC_UNLIKELY(!addr))
    return -ENOMEM;

  if (len == 0)
    return 0;

  // Linux rounds addr down to page boundary and adjusts len upward.
  // Match that behavior so programs ported from Linux work unchanged.
  const SIZE_T page_size = windows::get_page_size();
  uintptr_t start = reinterpret_cast<uintptr_t>(addr) & ~(page_size - 1);
  uintptr_t original_end = reinterpret_cast<uintptr_t>(addr) + len;

  // Overflow check: addr + len must not wrap.
  if (LIBC_UNLIKELY(original_end < reinterpret_cast<uintptr_t>(addr)))
    return -ENOMEM;

  SIZE_T adjusted_len = original_end - start;
  const SIZE_T rounded_len = windows::round_to_page(adjusted_len);
  if (LIBC_UNLIKELY(rounded_len == 0))
    // Overflow in round_to_page.
    return -ENOMEM;

  HANDLE process = NtCurrentProcess();

  // Pre-expand working set for the full range to minimize per-chunk retries.
  windows::expand_working_set(process, rounded_len);

  // Walk the range via bulk VA query (NtPssCaptureVaSpaceBulk) — 1.3-3.2x
  // faster than iterative NtQueryVirtualMemory. NtLockVirtualMemory does not
  // alter VA region boundaries, so the bulk snapshot stays valid.
  auto ws = windows::byte_scratch(4096);
  if (!ws) return -ENOMEM;
  windows::RegionWalker walk(reinterpret_cast<void *>(start), rounded_len,
      reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(ws.data()),
      ws.size());

  while (walk.next()) {
    // Unlike mlockall (best-effort), mlock fails on unlockable regions.
    if (!windows::is_lockable(*walk.entry))
      return -ENOMEM;

    intptr_t ret = lock_chunk(process, walk.chunk, walk.chunk_size);
    if (ret < 0)
      return ret;
  }

  return 0;
}

intptr_t mlock2(const void *addr, size_t len, int flags) {
  // Reject unknown flags.
  if (LIBC_UNLIKELY((flags & ~MLOCK_ONFAULT) != 0))
    return -EINVAL;

  if (LIBC_UNLIKELY(!addr))
    return -ENOMEM;

  if (len == 0)
    return 0;

  // Round addr down and len up to page boundaries (matches Linux behavior).
  const SIZE_T page_size = windows::get_page_size();
  uintptr_t start = reinterpret_cast<uintptr_t>(addr) & ~(page_size - 1);
  uintptr_t original_end = reinterpret_cast<uintptr_t>(addr) + len;

  if (LIBC_UNLIKELY(original_end < reinterpret_cast<uintptr_t>(addr)))
    return -ENOMEM;

  SIZE_T adjusted_len = original_end - start;
  const SIZE_T rounded_len = windows::round_to_page(adjusted_len);
  if (LIBC_UNLIKELY(rounded_len == 0))
    return -ENOMEM;

  void *range_addr = reinterpret_cast<void *>(start);

  // MLOCK_ONFAULT: deferred locking via PAGE_GUARD + VEH.
  if (flags & MLOCK_ONFAULT) {
    // Pre-expand working set — pages will be locked incrementally as they
    // fault in, but the quota must accommodate the full range upfront.
    windows::expand_working_set(NtCurrentProcess(), rounded_len);

    if (!windows::onfault_arm_range(range_addr, rounded_len))
      return -ENOMEM;
    return 0;
  }

  // flags == 0: immediate lock (identical to mlock).
  HANDLE process = NtCurrentProcess();

  // Pre-expand working set for the full range.
  windows::expand_working_set(process, rounded_len);

  // Bulk VA walk — same as mlock().
  auto ws = windows::byte_scratch(4096);
  if (!ws) return -ENOMEM;
  windows::RegionWalker walk(reinterpret_cast<void *>(start), rounded_len,
      reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(ws.data()),
      ws.size());

  while (walk.next()) {
    if (!windows::is_lockable(*walk.entry))
      return -ENOMEM;

    intptr_t ret = lock_chunk(process, walk.chunk, walk.chunk_size);
    if (ret < 0)
      return ret;
  }

  return 0;
}

intptr_t mlockall(int flags) {
  if (flags == 0 ||
      (flags & ~(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT)) != 0)
    return -EINVAL;

  // MCL_ONFAULT without MCL_CURRENT or MCL_FUTURE is meaningless.
  if ((flags & MCL_ONFAULT) && !(flags & (MCL_CURRENT | MCL_FUTURE)))
    return -EINVAL;

  // MCL_CURRENT: lock all currently committed regions.
  if (flags & MCL_CURRENT) {
    HANDLE process = NtCurrentProcess();
    bool use_onfault = (flags & MCL_ONFAULT) != 0;

    // Bulk-query the entire VA space via RegionWalker. 64KB heap buffer
    // holds ~1300 entries per bulk call — covers most processes in one
    // syscall, paginates transparently for larger ones.
    constexpr SIZE_T BUF_SIZE = 0x10000;
    windows::PlaceholderRange ph = windows::PlaceholderRange::reserve(BUF_SIZE);
    if (!ph)
      return -ENOMEM;
    void *buf = ph.base();
    if (NT_ERROR(ph.commit(PAGE_READWRITE)))
      return -ENOMEM; // ~ph releases placeholder on failure.

    auto *bulk = static_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf);
    windows::RegionWalker walk(nullptr, bulk, BUF_SIZE);
    bool failed = false;

    while (walk.next()) {
      if (!windows::is_lockable(*walk.entry))
        continue;

      if (use_onfault) {
        (void)windows::onfault_arm_range(walk.entry->BaseAddress,
                                         walk.entry->RegionSize);
      } else {
        for (int attempt = 0; attempt < 3; ++attempt) {
          PVOID base = walk.entry->BaseAddress;
          SIZE_T size = walk.entry->RegionSize;

          NTSTATUS lock_status =
              ::NtLockVirtualMemory(process, &base, &size, MAP_PROCESS);

          if (NT_SUCCESS(lock_status))
            break;

          if (lock_status == STATUS_WORKING_SET_QUOTA) {
            if (!windows::expand_working_set(process, walk.entry->RegionSize)) {
              failed = true;
              break;
            }
            continue;
          }
          break;
        }
        if (failed)
          break;
      }
    }

    windows::vm_release(buf);

    if (failed)
      return -EAGAIN;
  }

  // MCL_FUTURE: set global flag for future allocations.
  if (flags & MCL_FUTURE) {
    unsigned new_flags = MCL_FUTURE;
    if (flags & MCL_ONFAULT)
      new_flags |= MCL_ONFAULT;

    // Atomically merge flags (don't clear MCL_CURRENT if it was set earlier).
    windows::g_mcl_flags.fetch_or(new_flags, cpp::MemoryOrder::RELEASE);
  }

  return 0;
}

intptr_t munlock(const void *addr, size_t len) {
  // Match mlock's validation: ENOMEM for null (Linux behavior).
  if (LIBC_UNLIKELY(!addr))
    return -ENOMEM;

  if (len == 0)
    return 0;

  // Linux rounds addr down to page boundary and adjusts len upward.
  // Match that behavior so programs ported from Linux work unchanged.
  const SIZE_T page_size = windows::get_page_size();
  uintptr_t start = reinterpret_cast<uintptr_t>(addr) & ~(page_size - 1);
  uintptr_t original_end = reinterpret_cast<uintptr_t>(addr) + len;

  // Overflow check: addr + len must not wrap.
  if (LIBC_UNLIKELY(original_end < reinterpret_cast<uintptr_t>(addr)))
    return -ENOMEM;

  SIZE_T adjusted_len = original_end - start;
  const SIZE_T rounded_len = windows::round_to_page(adjusted_len);
  if (LIBC_UNLIKELY(rounded_len == 0))
    // Overflow in round_to_page.
    return -ENOMEM;

  // Remove any onfault tracking for this range (from mlock2 MLOCK_ONFAULT).
  windows::onfault_disarm_range(reinterpret_cast<void *>(start), rounded_len);

  HANDLE process = NtCurrentProcess();

  // Bulk VA walk — NtUnlockVirtualMemory does not alter VA region boundaries.
  auto ws = windows::byte_scratch(4096);
  if (!ws) return -ENOMEM;
  windows::RegionWalker walk(reinterpret_cast<void *>(start), rounded_len,
      reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(ws.data()),
      ws.size());

  while (walk.next()) {
    // Only unlock committed, accessible memory. Unlocking PAGE_NOACCESS or
    // non-committed memory would fail, and those regions can't be locked
    // anyway, so skipping them is correct.
    if (windows::is_lockable(*walk.entry)) {
      PVOID base = walk.chunk;
      SIZE_T region_size = walk.chunk_size;

      NTSTATUS unlock_status =
          ::NtUnlockVirtualMemory(process, &base, &region_size, MAP_PROCESS);

      // STATUS_NOT_LOCKED: pages weren't locked — not an error per POSIX.
      if (NT_ERROR(unlock_status) && unlock_status != STATUS_NOT_LOCKED)
        return -ENOMEM;
    }
  }

  return 0;
}

intptr_t munlockall() {
  // Clear MCL_FUTURE and MCL_ONFAULT flags.
  windows::g_mcl_flags.store(0, cpp::MemoryOrder::RELEASE);

  HANDLE process = NtCurrentProcess();

  // Bulk-query entire VA space via RegionWalker with 64KB heap buffer.
  constexpr SIZE_T BUF_SIZE = 0x10000;
  windows::PlaceholderRange ph = windows::PlaceholderRange::reserve(BUF_SIZE);
  void *buf = nullptr;
  if (ph) {
    buf = ph.base();
    if (NT_ERROR(ph.commit(PAGE_READWRITE)))
      buf = nullptr; // ~ph releases placeholder on failure.
  }

  if (buf) {
    auto *bulk = static_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf);
    windows::RegionWalker walk(nullptr, bulk, BUF_SIZE);
    while (walk.next()) {
      if (!windows::is_lockable(*walk.entry))
        continue;

      PVOID base = walk.entry->BaseAddress;
      SIZE_T size = walk.entry->RegionSize;
      (void)::NtUnlockVirtualMemory(process, &base, &size, MAP_PROCESS);
    }

    windows::vm_release(buf);
  }

  // Release the inflated hard minimum set by mlock/mlockall's
  // expand_working_set. The OS can now trim the working set normally.
  SIZE_T min_ws, max_ws;
  ULONG flags;
  if (windows::query_working_set(process, min_ws, max_ws, flags)) {
    if (flags & QUOTA_LIMITS_HARDWS_MIN_ENABLE)
      (void)windows::set_working_set(process, min_ws, max_ws,
                                     QUOTA_LIMITS_HARDWS_MIN_DISABLE);
  }

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
