//===-- Explicit ordered subsystem shutdown for c.dll ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// __libc_dll_fini() — called by c.dll's _DllMainCRTStartup during
// DLL_PROCESS_DETACH (FreeLibrary only, not process exit). Replaces
// the former .CRT$XP* section-walked pre-terminator dispatch.
//
// Shutdown order is the reverse of init: services first, then
// infrastructure, then VEH.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

#include "src/__support/macros/config.h"

extern "C" void __libc_dll_fini() {
  using namespace LIBC_NAMESPACE::internal;

  // Reverse of init order: Phase 8 → Phase 7 → ... → Phase 1.
  // Signal must be torn down before reactor (signal's ALPC is reactor-driven).
  // Each fini function must be no-op-safe if called without a matching init
  // (can happen on failed DLL_PROCESS_ATTACH → immediate DLL_PROCESS_DETACH).

  // Phase 7 (services) — signal first, then reactor
  signal_startup_fini();
  reactor_startup_fini();

  // Phase 5 (VEH fault handlers)
  mlock_policy_startup_fini();

  // Phase 4 (pools + memory infrastructure)
  mapping_table_startup_fini();
  named_semaphore_startup_fini();
  file_pool_startup_fini();
  ofd_pool_startup_fini();

  // Phase 1 (VEH core)
  veh_core_startup_fini();
}
