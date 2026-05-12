//===- mlock_policy.h - mlockall flag accessors + lock helpers --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Inline accessors for `g_pcb.mlock.mcl_flags` plus the two helpers
/// the `MAP_LOCKED` / `MCL_FUTURE` post-acquire paths in mmap call:
/// `lock_range` (best-effort lock with working-set quota expansion)
/// and `lock_if_future` (the MCL_FUTURE policy gate).
///
/// **Why a separate header from `mlock_process_state.h`.** The struct
/// definition (`MlockProcessState`) is consumed by
/// `process_control_block.h` to define the field. Reading the field
/// requires the full PCB. Putting the accessors in
/// `mlock_process_state.h` would force it to forward-declare `g_pcb`,
/// or to include `process_control_block.h` (cycle: PCB → struct →
/// PCB). The split puts the struct in one header (PCB include
/// target) and the accessors in this header (consumer include
/// target); each has one job.
///
/// **Layer placement.** This is POSIX-side policy — the MCL_FUTURE /
/// MCL_ONFAULT decision is a POSIX semantic. The kernel ops it
/// composes (`nt_pal::lock_range`, `nt_pal::expand_working_set`) are
/// PAL primitives. The split is intentional: PAL stays op-shaped,
/// POSIX layer carries the policy.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_POLICY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_POLICY_H

#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt_pal/lock.h"
#include "src/__support/OSUtil/windows/nt_pal/working_set.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

/// True if `MCL_FUTURE` is currently set in the process mlock flags.
[[nodiscard]] LIBC_INLINE bool mcl_future_enabled() {
  // RELAXED is sufficient: a stale read in the post-mmap window only
  // determines whether *this* mapping gets locked, not future ones,
  // and the fact that mmap's caller saw the `mlockall` return imposes
  // its own happens-before — anything stronger here would be cosmetic.
  return (::LIBC_NAMESPACE::g_pcb.mlock.mcl_flags.load(
              ::LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED) &
          static_cast<unsigned>(MCL_FUTURE)) != 0u;
}

/// True if `MCL_ONFAULT` is currently set.
[[nodiscard]] LIBC_INLINE bool mcl_onfault_enabled() {
  return (::LIBC_NAMESPACE::g_pcb.mlock.mcl_flags.load(
              ::LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED) &
          static_cast<unsigned>(MCL_ONFAULT)) != 0u;
}

/// Lock a freshly-mapped range, expanding the working-set quota as
/// needed. Best-effort — the lock failure is silently swallowed so
/// `MAP_LOCKED` cannot demote the parent `mmap` from success to
/// failure (Linux contract). Same shape callers expected from the
/// retired `legacy/memory_lock_policy.h::lock_range` so the call
/// sites in mmap_engine survive the substrate swap unchanged.
LIBC_INLINE void lock_range(void *addr, SIZE_T size) {
  HANDLE process = NtCurrentProcess();
  ::LIBC_NAMESPACE::nt_pal::expand_working_set(process, size);
  (void)::LIBC_NAMESPACE::nt_pal::lock_range(addr, size);
}

/// Apply MCL_FUTURE policy after a successful `mmap`. If MCL_FUTURE
/// is not set this is a single RELAXED load. If it is set the range
/// is locked.
///
/// MCL_ONFAULT degrades to immediate lock on this path because the
/// legacy mmap engine has no per-region desc to attach the
/// `region_flag::MLOCK_ONFAULT` bit to — the legacy mapping table
/// model predates the va_tracker. P2's mmap rebuild routes new
/// mappings through `va_tracker::acquire`, where the AcquireMeta
/// can carry MLOCK_ONFAULT under MCL_FUTURE|MCL_ONFAULT and the
/// MLOCK_ONFAULT path returns correctly.
LIBC_INLINE void lock_if_future(void *addr, SIZE_T size) {
  if (LIBC_LIKELY(!mcl_future_enabled()))
    return;
  lock_range(addr, size);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_POLICY_H
