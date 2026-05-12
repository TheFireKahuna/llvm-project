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

#include "src/__support/OSUtil/windows/memory/legacy/mlock_ops.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_lock_policy.h"
#include "src/__support/OSUtil/windows/alloc/placeholder_range.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/memory/legacy/working_set.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {

namespace internal {

// mlock, mlock2, and munlock now live in
// src/__support/OSUtil/windows/memory/posix/mlock.cpp; only the
// process-wide mlockall / munlockall paths remain in the legacy
// translation unit until P9 rebuilds them on the new substrate.

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

    // Bulk-query the entire VA space via nt_pal::RegionWalker. 64KB heap buffer
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
    nt_pal::RegionWalker walk(nullptr, bulk, BUF_SIZE);
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

    nt_pal::free_va(buf);

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

intptr_t munlockall() {
  // Clear MCL_FUTURE and MCL_ONFAULT flags.
  windows::g_mcl_flags.store(0, cpp::MemoryOrder::RELEASE);

  HANDLE process = NtCurrentProcess();

  // Bulk-query entire VA space via nt_pal::RegionWalker with 64KB heap buffer.
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
    nt_pal::RegionWalker walk(nullptr, bulk, BUF_SIZE);
    while (walk.next()) {
      if (!windows::is_lockable(*walk.entry))
        continue;

      PVOID base = walk.entry->BaseAddress;
      SIZE_T size = walk.entry->RegionSize;
      (void)::NtUnlockVirtualMemory(process, &base, &size, MAP_PROCESS);
    }

    nt_pal::free_va(buf);
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
