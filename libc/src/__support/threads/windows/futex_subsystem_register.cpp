//===--- Register futex subsystem thread-exit unlinkers -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single-purpose TU that installs the two subsystem-specific unlinker
// callbacks with the wait_slot pool. Split out from both wait_slot.cpp
// (to keep that file free of futex internals) and from futex_addr.h /
// futex_utils.h (both are header-only, and libc forbids global
// constructors, so we can't register at static-init time). Called
// exactly once from wait_slot_startup_init during Phase 4 of libc
// bring-up — before any user code can run, therefore before any thread
// can die with a linked slot.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/wait_slot.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace wait_slot {

void install_subsystem_unlinkers() {
  register_parking_lot_unlinker(
      &futex_addr::parking_lot_unlink_thread_exit);
  register_harris_unlinker(&harris_unlink_futex_trampoline);
}

} // namespace wait_slot
} // namespace LIBC_NAMESPACE_DECL

void LIBC_NAMESPACE::internal::futex_addr_fork_reinit() {
  LIBC_NAMESPACE::futex_addr::fork_reinit();
}

LIBC_REGISTER_FORK_REINIT(futex_addr,
                          ::LIBC_NAMESPACE::internal::kForkPrioFutexAddr,
                          &::LIBC_NAMESPACE::internal::futex_addr_fork_reinit)
