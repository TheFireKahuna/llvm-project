//===-- Canonical VEH state for Windows --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lightweight process-wide state types for the unified VEH dispatcher.
// These are the canonical storage types used directly in the PCB.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_STATE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_utils.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {

struct VehFilter {
  uint32_t exception_mask;                 // Bitmask of VehExceptionBit
  LONG (*handler)(EXCEPTION_POINTERS *ep); // Filter callback
  uint8_t priority;                        // Lower = earlier dispatch
};

inline constexpr uint8_t VEH_PRIORITY_MEMORY = 10;
inline constexpr uint8_t VEH_PRIORITY_MLOCK = 15;
inline constexpr uint8_t VEH_PRIORITY_SIGNAL = 20;

inline constexpr int VEH_MAX_FILTERS = 8;

// Sealed half: this struct lives in PcbZone0 (sealed PAGE_READONLY at end of
// Tier A, never unsealed). The filter table is fully populated by the
// .libcveh sweep before seal; the TLS indices are allocated in Phase 0a/0b
// before seal.
//
// Once sealed:
//   - master_veh_handler reads filters[]/filter_count without synchronization
//   - an attacker with arbitrary write cannot redirect the dispatch table
//   - fork inherits the sealed page via COW; no rewrite needed in the child
struct VehSealedState {
  VehFilter filters[VEH_MAX_FILTERS];
  int filter_count;                       // populated by Tier A sweep
  unsigned reentry_tls_index;             // allocated by Phase 0a
  unsigned fault_guard_tls_index;         // allocated by Phase 0b
};

// Mutable half: stays in Zone 1. handler_handle is the only field that
// changes after init — dll_load_callback re-issues RtlAddVectoredException-
// Handler on every DLL load to maintain front-of-chain position, so the
// returned handle changes.
//
// dll_notify_cookie lives in Zone 0b instead: it's set once at init and
// re-acquired during fork-reinit alongside the PID/cookie writes that
// already require the Zone 0b unseal window.
struct VehMutableState {
  cpp::Atomic<void *> handler_handle; // RtlAddVEH return — updated on DLL load
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_STATE_H
