//===-- nt_pal::NtPalProcessState — PCB-resident PAL state -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-process Layer 0 PAL state, embedded in `ProcessControlBlock` so
// large-pages availability lives in the same demand-zero PE section as
// every other process-lifetime scalar — no separate file-scope global,
// no `-Wglobal-constructors` concern (the PCB has no destructors), and
// fork inheritance follows the same COW path the rest of the PCB rides
// on. The per-process cookie that used to live here moved to `PcbZone0b`
// when §17.2 #7 was honoured (sealed PAGE_READONLY at runtime).
//
// Leaf header — no PCB include here; this struct is meant to be included
// **by** `process_control_block.h`, and the accessors in `pal_state.h`
// reach state through the PCB instance.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_NT_PAL_PROCESS_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_NT_PAL_PROCESS_STATE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace nt_pal {

} // namespace nt_pal

namespace internal {

// PCB sub-state for the Layer 0 PAL.
//
//   * `large_pages_available` — boolean cached from the one-shot
//     `RtlAdjustPrivilege(SeLockMemoryPrivilege, ...)` probe at libc
//     init. Probe-once; never per allocation. Re-probed after fork as
//     cheap insurance.
//
// Note on `process_cookie`: the per-process cookie probed via
// `NtQueryInformationProcess(ProcessCookie /* 36 */)` lives in
// `PcbZone0b`, not here — the kernel rerolls the cookie across
// `RtlCloneUserProcess`, so the canonical home for it is the same
// page that already takes the Zone 0b unseal/reseal trip on every
// fork (see `nt_pal/pal_init.cpp`). The earlier Zone 1 placement let
// an arbitrary-write primitive poison every encoded freelist pointer
// and shift the partition canary's XOR inputs without leaving the
// page table.
//
// Note on partition state: the Layer 7 partition layer (introduced in
// P1.E) maintains its own coarse pagemap, reserve table, and descriptor
// pool. The reserve table is a file-scope static in `alloc/partition.cpp`
// (CoW-shared on fork like all libc.dll BSS), addressed via the
// sealed Zone 0 pointer `g_pcb.zone0.partition_reserve_table()`. The
// older `partition_base_table` skeleton from P1.A is removed — the
// new partition layer's PCB integration is sealed pointers in Zone 0,
// not mutable Zone 1 storage.
struct NtPalProcessState {
  cpp::Atomic<bool> large_pages_available;
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_NT_PAL_PROCESS_STATE_H
