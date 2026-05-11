//===-- nt_pal::pal_state — per-process PAL state ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Read-only accessors for the Layer 0 PAL's per-process state.
//
//   * `process_cookie()`  — `NtQueryInformationProcess(ProcessCookie /* 36 */)`
//                           queried once at libc init, cached in
//                           `g_pcb.zone0b.process_cookie_` (sealed
//                           PAGE_READONLY at runtime; rewritten only
//                           during the Zone 0b unseal window after
//                           fork). Non-zero on a healthy process.
//   * `large_pages_available()` — boolean cached from the one-shot
//                           `RtlAdjustPrivilege(SeLockMemoryPrivilege, ...)`
//                           probe at libc init. Probe-once; never per
//                           allocation. Stored in `g_pcb.nt_pal`.
//
// Mutation goes through `pal_init.cpp`; consumers must not write the
// PCB fields directly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_PAL_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_PAL_STATE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal_process_state.h"
#include "src/__support/OSUtil/windows/nt_pal/numa_topology.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace nt_pal {

// Returns the per-process cookie probed once at libc init via
// `NtQueryInformationProcess(ProcessCookie /* 36 */)`. Stable across
// the process lifetime (rerolled by the kernel on `RtlCloneUserProcess`
// and re-cached by `pal_fork_reinit`). Non-zero on a healthy process;
// zero is the "not yet initialised" sentinel callers can fail-safe on.
//
// Plain (non-atomic) load: the storage is in PCB Zone 0b which is
// PAGE_READONLY whenever this function is reachable to readers — the
// hardware seal serialises against the one-shot writer in
// `pal_probe_once()` (Tier A pre-seal) and the fork rewriter (running
// alone inside the unseal window with no peer threads).
[[nodiscard]] LIBC_INLINE uint32_t process_cookie() {
  return ::LIBC_NAMESPACE::g_pcb.zone0b.process_cookie();
}

// True iff `SeLockMemoryPrivilege` was held by the libc init thread at
// init time (probe-once, never per allocation). Drives whether
// `MAP_HUGETLB` returns success (`true`) or `EPERM` (`false`).
[[nodiscard]] LIBC_INLINE bool large_pages_available() {
  return ::LIBC_NAMESPACE::g_pcb.nt_pal.large_pages_available.load(
      cpp::MemoryOrder::ACQUIRE);
}

// Process-lifetime NUMA topology snapshot. Built once at libc init via
// `NtQuerySystemInformationEx(SystemLogicalProcessorInformationEx,
// RelationNumaNode, ...)` and cached inline in sealed Zone 0. Hot
// readers (partition selector, sched_getcpu, numa_ops) reach the
// snapshot through this forwarder rather than touching `g_pcb` directly
// to keep the PCB include-graph dependency minimal.
//
// The reference is hardware-immutable for process lifetime — no atomic
// load needed; the seal serialises against the one-shot writer in
// `pal_probe_once()` (Tier A pre-seal).
[[nodiscard]] LIBC_INLINE const ::LIBC_NAMESPACE::windows::NumaTopology &
numa_topology() {
  return ::LIBC_NAMESPACE::g_pcb.zone0.numa_topology();
}

} // namespace nt_pal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_PAL_STATE_H
