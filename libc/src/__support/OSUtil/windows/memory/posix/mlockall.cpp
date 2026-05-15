//===- mlockall.cpp - process-wide mlockall / munlockall ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `mlockall(flags)` / `munlockall()` on the new substrate.
///
/// MCL_CURRENT walks the entire user VA via `nt_pal::RegionWalker`
/// (the kernel-VAD walker that already powers `va_inventory`'s startup
/// discovery sweep). The broad scope catches POSIX-tracked mappings,
/// loader DLLs (`Image` cordon), libc heap (`LIBC_INTERNAL`), and
/// foreign mappings — matching Linux's "lock all currently mapped
/// pages" intent rather than narrowing to the va_tracker's POSIX-only
/// view.
///
/// MCL_ONFAULT layers per-desc state on top: for every tracked region
/// the walk crosses, `va_tracker::mutate(lock_set_onfault_mutator)`
/// flips `region_flag::MLOCK_ONFAULT`, then `nt_pal::arm_guard_trap`
/// installs PAGE_GUARD on each committed page so the first touch
/// raises `STATUS_GUARD_PAGE_VIOLATION`. The memory subsystem's
/// guard-page filter (`mem_fault_handler.cpp::try_mlock_onfault`)
/// resolves the desc, sees the flag, and locks the page. Untracked
/// regions are skipped silently — there's no desc to host the bit and
/// the loader / heap allocator already manages residency.
///
/// MCL_FUTURE merges its bits into `g_pcb.mlock.mcl_flags` (PCB Zone 1).
/// The P2 mmap rebuild reads the field via `lock_if_future` after every
/// successful map.
///
/// `munlockall` clears `g_pcb.mlock.mcl_flags` BEFORE the walk so a
/// concurrent mmap on another thread cannot apply MCL_FUTURE to a new
/// allocation we are about to unlock from. After unlocking it releases
/// the inflated working-set hard-min via `QUOTA_LIMITS_HARDWS_MIN_DISABLE`
/// so the OS can trim normally — symmetric teardown of the
/// `expand_working_set` inflations mlockall and mlock leave behind.
///
/// Fork: POSIX requires mlock state not to be inherited.
/// `g_pcb.mlock.mcl_flags` is reset by the posix-band fork hook
/// (`mlock_posix_fork_reinit`); per-desc `MLOCK_ONFAULT` bits are
/// stripped by the va_tracker fork serializer so the child replay
/// produces clean descs.
///
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mlockall.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
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

/// Arm MLOCK_ONFAULT on every tracked desc that intersects
/// `(region_base, region_size)`. Untracked regions silently skip — no
/// desc to host the bit. PAGE_GUARD arming proceeds regardless of
/// resolve outcome since the filter rejects unguarded faults via the
/// flag check.
LIBC_INLINE void arm_onfault_for_region(void *region_base, SIZE_T region_size) {
  // Resolve at the region base. The kernel-VAD walker emits
  // protection-band sub-regions of one NT allocation as separate
  // entries; resolve at the base lands us on whichever desc covers
  // this VA — single resolve is enough because mutate's range argument
  // controls the actual overlap set.
  auto ref_or = vt::resolve(region_base);
  if (!ref_or.has_value())
    return;

  vt::VaRange range{region_base, static_cast<size_t>(region_size)};
  int rc = vt::mutate(range, &mp::lock_set_onfault_mutator,
                      /*ctx=*/nullptr, /*prot_change=*/0);
  if (rc != 0 && rc != ENOENT)
    return; // Best-effort: a mutate failure here just means this
            // region won't lock-on-fault. POSIX permits.

  ::LIBC_NAMESPACE::nt_pal::arm_guard_trap(region_base,
                                            static_cast<size_t>(region_size));
}

/// Disarm MLOCK_ONFAULT on every tracked desc intersecting
/// `(region_base, region_size)`. Best-effort; PAGE_GUARD residue is
/// self-clearing.
LIBC_INLINE void disarm_onfault_for_region(void *region_base,
                                           SIZE_T region_size) {
  auto ref_or = vt::resolve(region_base);
  if (!ref_or.has_value())
    return;
  vt::VaRange range{region_base, static_cast<size_t>(region_size)};
  (void)vt::mutate(range, &mp::lock_clear_mutator, /*ctx=*/nullptr,
                   /*prot_change=*/0);
}

/// 3-retry quota-expansion lock for one kernel-VAD region. Returns 0
/// on success, -1 on hard failure (caller maps to `EAGAIN`).
///
/// `expand_working_set` still takes the explicit process handle
/// because it talks to `NtSetInformationProcess(ProcessQuotaLimits)`,
/// not to a per-VA op — keep the signature wide so the caller controls
/// the target process when this gets reused for cross-process locking.
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
    // Other statuses (STATUS_ACCESS_DENIED, STATUS_NOT_COMMITTED, etc.)
    // are silent skip per POSIX best-effort — they don't contribute to
    // the EAGAIN failure flag.
    return 0;
  }
  return -1;
}

} // namespace

namespace internal {

/// Posix-band fork-reinit hook: POSIX requires mlock state to be
/// reset across `fork()`. The PCB-resident `mcl_flags` survives via
/// CoW by default; explicitly storing zero matches the kernel
/// semantic. Per-desc `MLOCK_ONFAULT` bits are stripped by the
/// va_tracker fork serializer separately.
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
    // Atomically merge — preserve any previously-set MCL_CURRENT bit.
    g_pcb.mlock.mcl_flags.fetch_or(
        new_flags, ::LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
  }

  return 0;
}

intptr_t munlockall() {
  // Clear future-arming flags BEFORE the walk so a concurrent mmap
  // on another thread does not race-arm a freshly mapped range that
  // the unlock loop is about to skip past.
  g_pcb.mlock.mcl_flags.store(
      0u, ::LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);

  // `process` is needed only for the working-set quota teardown
  // below; the per-region unlock walk goes through `nt_pal::unlock_range`
  // which targets the current process implicitly.
  HANDLE process = NtCurrentProcess();

  // Walk: best-effort. Scratch-alloc failure does NOT propagate as an
  // error — POSIX requires munlockall to clear its flags even if the
  // walk cannot proceed (the flags clear above already happened).
  auto walk = ::LIBC_NAMESPACE::nt_pal::RegionWalker::whole_process();
  if (walk) {
    while (walk.next()) {
      if (!::LIBC_NAMESPACE::nt_pal::is_lockable(*walk.entry))
        continue;

      void *region_base = walk.entry->BaseAddress;
      SIZE_T region_size = walk.entry->RegionSize;

      // Disarm MLOCK_ONFAULT per tracked desc before the unlock —
      // otherwise the guard-page filter would re-lock pages on next
      // access (matches mlock/munlock disarm-before-unlock invariant).
      disarm_onfault_for_region(region_base, region_size);

      // STATUS_NOT_LOCKED tolerated — POSIX permits munlock on
      // never-locked memory.
      (void)::LIBC_NAMESPACE::nt_pal::unlock_range(region_base, region_size);
    }
  }

  // Release the inflated hard minimum that mlock / mlockall installed
  // via expand_working_set. The OS can now trim the working set
  // normally.
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

// Fork-reinit registration. `kForkPrioMlockPolicy` (51) runs after
// the substrate fork hooks (kForkPrioCrystalline..kForkPrioVaTracker
// at 30..39) and the memory-reconcile slot (50), so the va_tracker is
// fully rebuilt before this hook fires — any future expansion that
// wants to walk tracked descs from here is safe.
LIBC_REGISTER_FORK_REINIT(mlock_policy,
                          ::LIBC_NAMESPACE::internal::kForkPrioMlockPolicy,
                          &::LIBC_NAMESPACE::internal::mlock_policy_fork_reinit)
