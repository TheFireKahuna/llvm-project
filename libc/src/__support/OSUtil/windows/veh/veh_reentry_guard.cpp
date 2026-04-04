//===-- VEH reentry guard TLS allocation --------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/veh/veh_reentry_guard.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// TLS index lives in sealed Zone 0 (g_pcb.zone0.veh_sealed().reentry_tls_index).
// Written once during Phase 0a via PcbInitAccess while Zone 0 is still
// unsealed, then read-only for process lifetime.

bool init_veh_reentry_guard() {
  unsigned &idx = PcbInitAccess::veh_sealed_mut().reentry_tls_index;
  // PCB is zero-initialized, so idx starts as 0 (a valid TLS slot index).
  // The old sentinel check (idx != TLS_OUT_OF_INDEXES) doesn't work with
  // zero-init. Since this runs exactly once from veh_reentry_guard_startup_init(), we allocate
  // unconditionally. The allocated index overwrites the zero.
  unsigned new_idx = tls_alloc();
  if (new_idx == TLS_OUT_OF_INDEXES)
    return false;
  idx = new_idx;
  return true;
}

unsigned get_veh_reentry_tls_index() {
  return g_pcb.zone0.veh_sealed().reentry_tls_index;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Phase 1 startup init — allocate TLS slot for VEH reentry guard.
// Called by __libc_dll_init() before veh_core_startup_init().
int LIBC_NAMESPACE::internal::veh_reentry_guard_startup_init() {
  if (!LIBC_NAMESPACE::internal::init_veh_reentry_guard())
    __builtin_trap(); // Cannot proceed without reentry protection.
  return 0;
}
