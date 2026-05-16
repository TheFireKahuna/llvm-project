//===- mlockall.cpp - process-wide mlockall / munlockall ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// MCL_CURRENT walks every kernel VAD via `nt_pal::RegionWalker` —
// broader than the va_tracker's POSIX-only view, matching Linux's
// "lock all currently mapped pages" intent (loader DLLs, libc heap,
// foreign mappings all included). MCL_FUTURE state lives in
// `g_pcb.mlock.mcl_flags`; MCL_ONFAULT layers per-desc
// `region_flag::LOCK_ONFAULT` on top of the VAD walk so the guard-page
// filter in `mem_fault_handler.cpp` can lock on first touch.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mlockall.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"
#include "src/__support/OSUtil/windows/nt_pal/working_set.h"
#include "src/__support/OSUtil/windows/memory/posix/mlock_process_state.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_mutators.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/nt_pal/lock.h"
#include "src/__support/OSUtil/windows/nt_pal/protect.h"
#include "src/__support/OSUtil/windows/nt_pal/query.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {

namespace {

namespace mp = ::LIBC_NAMESPACE::windows::memory_posix;
namespace vt = ::LIBC_NAMESPACE::windows::va_tracker;

// Arm `region_flag::LOCK_ONFAULT` on every tracked desc the VAD region
// intersects, then OR PAGE_GUARD on its committed pages. Untracked
// regions silently skip: no desc to host the bit and no observable
// effect to deliver.
LIBC_INLINE void arm_onfault_for_region(void *region_base, SIZE_T region_size) {
  // Resolve at the base only — `mutate`'s range argument controls the
  // actual overlap set, so one resolve is enough even when the VAD
  // walker splits an NT allocation into protection sub-bands.
  auto ref_or = vt::resolve(region_base);
  if (!ref_or.has_value())
    return;

  vt::VaRange range{region_base, static_cast<size_t>(region_size)};
  int rc = vt::mutate(range, &mp::lock_set_onfault_mutator,
                      /*ctx=*/nullptr, /*prot_change=*/0);
  // Best-effort: a mutate failure just means this region won't lock-
  // on-fault. POSIX permits.
  if (rc != 0 && rc != ENOENT)
    return;

  ::LIBC_NAMESPACE::nt_pal::arm_guard_trap(region_base,
                                            static_cast<size_t>(region_size));
}

LIBC_INLINE void disarm_onfault_for_region(void *region_base,
                                           SIZE_T region_size) {
  auto ref_or = vt::resolve(region_base);
  if (!ref_or.has_value())
    return;
  vt::VaRange range{region_base, static_cast<size_t>(region_size)};
  (void)vt::mutate(range, &mp::lock_clear_mutator, /*ctx=*/nullptr,
                   /*prot_change=*/0);
}

// 3-retry quota-expansion lock for one VAD region. Returns 0 on
// success or on any non-quota NTSTATUS (STATUS_ACCESS_DENIED,
// STATUS_NOT_COMMITTED, etc. — POSIX mlockall is best-effort and
// must not flag these as failures); -1 only on retry-budget
// exhaustion. `process` flows through from the caller because
// `expand_working_set` keys on a process handle.
LIBC_INLINE int lock_region(HANDLE process, void *region_base,
                            SIZE_T region_size) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::lock_range(region_base,
                                                        region_size);
    if (NT_SUCCESS(st))
      return 0;
    if (st == STATUS_WORKING_SET_QUOTA) {
      if (!::LIBC_NAMESPACE::nt_pal::expand_working_set(process, region_size))
        return -1;
      continue;
    }
    return 0;
  }
  return -1;
}

} // namespace

namespace internal {

// PCB-resident `mcl_flags` survives fork via CoW by default; POSIX
// requires reset. Per-desc `region_flag::LOCK_ONFAULT` bits are
// stripped by the va_tracker fork serializer separately.
void mlock_policy_fork_reinit() {
  g_pcb.mlock.mcl_flags.store(0u, ::LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);
}

} // namespace internal

namespace internal {

intptr_t mlockall(int flags) {
  if (flags == 0 ||
      (flags & ~(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT)) != 0)
    return -EINVAL;
  // ONFAULT alone is meaningless — needs a CURRENT or FUTURE target.
  if ((flags & MCL_ONFAULT) && !(flags & (MCL_CURRENT | MCL_FUTURE)))
    return -EINVAL;

  if (flags & MCL_CURRENT) {
    HANDLE process = NtCurrentProcess();
    const bool use_onfault = (flags & MCL_ONFAULT) != 0;

    auto walk = ::LIBC_NAMESPACE::nt_pal::RegionWalker::whole_process();
    if (!walk)
      return -ENOMEM;

    bool failed = false;
    while (walk.next()) {
      if (!::LIBC_NAMESPACE::nt_pal::is_lockable(*walk.entry))
        continue;

      void *region_base = walk.entry->BaseAddress;
      SIZE_T region_size = walk.entry->RegionSize;

      if (use_onfault) {
        arm_onfault_for_region(region_base, region_size);
      } else {
        if (lock_region(process, region_base, region_size) != 0) {
          failed = true;
          break;
        }
      }
    }

    if (failed)
      return -EAGAIN;
  }

  if (flags & MCL_FUTURE) {
    unsigned new_flags = static_cast<unsigned>(MCL_FUTURE);
    if (flags & MCL_ONFAULT)
      new_flags |= static_cast<unsigned>(MCL_ONFAULT);
    // RELEASE on the writer side; readers (`mcl_future_enabled` /
    // `mcl_onfault_enabled`) intentionally load RELAXED — the
    // mlockall-then-mmap ordering already comes from the syscall-
    // return edge in caller code, not from this flag word. The
    // RELEASE store is kept defensively so any future ACQUIRE
    // reader pairs without needing to revisit this site.
    g_pcb.mlock.mcl_flags.fetch_or(
        new_flags, ::LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
  }

  return 0;
}

intptr_t munlockall() {
  // Clear future-arming flags BEFORE the walk — otherwise a concurrent
  // mmap on another thread could race-arm a freshly mapped range that
  // the unlock loop is about to skip past.
  g_pcb.mlock.mcl_flags.store(
      0u, ::LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);

  HANDLE process = NtCurrentProcess();

  // Walk failure does NOT propagate as an error — POSIX requires
  // munlockall to clear its flags even when the walk can't proceed
  // (the clear above already happened).
  auto walk = ::LIBC_NAMESPACE::nt_pal::RegionWalker::whole_process();
  if (walk) {
    while (walk.next()) {
      if (!::LIBC_NAMESPACE::nt_pal::is_lockable(*walk.entry))
        continue;

      void *region_base = walk.entry->BaseAddress;
      SIZE_T region_size = walk.entry->RegionSize;

      // Disarm per-desc LOCK_ONFAULT before the unlock — same
      // re-lock-races-unlock invariant as `munlock`.
      disarm_onfault_for_region(region_base, region_size);

      // STATUS_NOT_LOCKED tolerated — POSIX permits munlock on
      // never-locked memory.
      (void)::LIBC_NAMESPACE::nt_pal::unlock_range(region_base, region_size);
    }
  }

  // Release the inflated hard-min that mlock / mlockall installed via
  // `expand_working_set`, so the OS can trim normally. Symmetric
  // teardown of the quota lift.
  SIZE_T min_ws, max_ws;
  ULONG quota_flags;
  if (::LIBC_NAMESPACE::nt_pal::query_working_set(process, min_ws, max_ws,
                                                    quota_flags)) {
    if (quota_flags & QUOTA_LIMITS_HARDWS_MIN_ENABLE)
      (void)::LIBC_NAMESPACE::nt_pal::set_working_set(
          process, min_ws, max_ws, QUOTA_LIMITS_HARDWS_MIN_DISABLE);
  }

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// `kForkPrioMlockPolicy` (51) runs after the va_tracker fork hooks
// (30..39) and the memory-reconcile slot (50), so any future expansion
// of this hook that walks tracked descs is safe.
LIBC_REGISTER_FORK_REINIT(mlock_policy,
                          ::LIBC_NAMESPACE::internal::kForkPrioMlockPolicy,
                          &::LIBC_NAMESPACE::internal::mlock_policy_fork_reinit)
