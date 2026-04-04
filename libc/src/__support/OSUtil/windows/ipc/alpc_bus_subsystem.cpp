//===-- Subsystem hook wrappers for alpc_bus -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin shims that expose alpc_bus lifecycle with the signatures
// expected by libc_subsystem_init.h (int-returning startup, void-returning
// fork hook). Having this translation unit keeps alpc_bus.cpp free of the
// subsystem_init.h dependency and lets the startup file call the bus
// without pulling ipc/alpc_bus.h into every subsystem consumer.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ipc/alpc_bus.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

int alpc_bus_startup_init() {
  // `init()` failure is non-fatal to the process — it disables cross-
  // process IPC but leaves every in-process subsystem fully functional.
  // Returning 0 here mirrors that contract (the startup driver treats
  // non-zero as a hard abort).
  (void)alpc_bus::init();
  return 0;
}

void alpc_bus_fork_reinit() { alpc_bus::fork_reinit(); }

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_FINI(9, alpc_bus, &::LIBC_NAMESPACE::internal::alpc_bus::fini)

LIBC_REGISTER_FORK_REINIT(alpc_bus,
                          ::LIBC_NAMESPACE::internal::kForkPrioAlpcBus,
                          &::LIBC_NAMESPACE::internal::alpc_bus_fork_reinit)
