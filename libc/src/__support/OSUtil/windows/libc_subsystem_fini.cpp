//===-- DLL_PROCESS_DETACH entry point for c.dll ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// __libc_dll_fini() — called by c.dll's _DllMainCRTStartup during
// DLL_PROCESS_DETACH (FreeLibrary only, not process exit).
//
// Subsystems do not appear here individually. Each one registers its
// teardown thunk into `.libcfin$P<phase>` next to its own definition
// via LIBC_REGISTER_FINI; this function walks the section in reverse
// phase order. The phase assignments and ordering rationale live in
// libc_fini_registry.h.
//
// The two operations that must run in `__libc_dll_fini` itself, before
// any subsystem fini, stay here:
//
//   1. Poison the init gate so a racing LoadLibrary("c.dll") cannot
//      re-enter Tier B against half-destroyed pools.
//   2. Walk the registry.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/config.h"

extern "C" void __libc_dll_fini() {
  LIBC_NAMESPACE::internal::mark_dll_init_poisoned();
  // Debug-only check: a future refactor could re-introduce the race the
  // poison gate exists to close. The assert pins the ordering contract
  // (poison precedes sweep) so the regression is caught at first run
  // rather than at first FreeLibrary→LoadLibrary cycle in production.
  LIBC_ASSERT(LIBC_NAMESPACE::internal::dll_init_is_poisoned() &&
              "init gate must be poisoned before .libcfin walker runs");
  LIBC_NAMESPACE::internal::run_all_finis();
}
