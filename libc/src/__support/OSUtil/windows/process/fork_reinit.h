//===-- Fork infrastructure: quiescence + reinit ----------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Three-phase fork protocol with pre-fork quiescence:
//
//   fork_prepare()  — Quiesce internal state, then run user prepare handlers.
//   fork_parent()   — Run user parent handlers, then release locks.
//   fork_child()    — Reinit internal state, then run user child handlers.
//
// Pre-fork quiescence (libc_fork_quiesce):
//   1. Flush all buffered FILE* streams (best-effort via try_lock)
//   2. Acquire critical locks in defined order:
//      MmapLock → env lock → signal handler lock → child table lock
//
// Post-fork child reinit (libc_fork_reinit):
//   pcb_unseal_readonly_b()       — Zone 0b unseal (Zone 0 stays sealed)
//   pcb_check_canary() + cookie reseed — rotate Zone 0b cookie/canary
//   veh_core_fork_reinit()        — VEH core re-register (writes Zone 0b
//                                   dll_notify_cookie under same window)
//   identity_fork_reinit()        — Process identity reinit + PID write
//   mapping_table_fork_reinit()   — Mapping table reinit (before fd_table)
//   mmap_lock_fork_reinit()       — Mmap lock reset (before mmap consumers)
//   env_fork_reinit()             — Environment lock reset + environ re-sync
//   ofd_pool_fork_reinit()        — OFD pool lock reset
//   file_pool_fork_reinit()       — FILE pool lock reset
//   wait_slot_fork_reinit()       — Wait slot pool reset
//   setitimer_fork_reinit()       — Interval timer state reset
//   child_table_fork_reinit()     — Child table lock reset
//   lifecycle_fork_reinit()       — Lifecycle pool reset + thread trimming
//   robust_pool_fork_reinit()     — Robust record pool reset
//   thread_ring_fork_reinit()     — Thread ring pool reset
//   thread_storage_fork_reinit()  — Thread storage pool reset + trim
//   fd_table_fork_reinit()        — FdTable I/O ring invalidation
//   reactor_fork_reinit()         — Reactor rebuild (new IOCP + drain thread)
//   signal_fork_reinit()          — Signal + registry (trims to one thread)
//   pcb_seal_readonly_b()         — Zone 0b canary validation + reseal
//
// No section-based dispatch, no function-pointer arrays, no indirect calls.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FORK_REINIT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FORK_REINIT_H

#include "include/__llvm-libc-common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Function pointer type for fork reinit callbacks. Returns 0 on success,
// non-zero to abort the child.
using ForkReinitFn = int(*)(void);

// Run all registered fork reinit functions in section order.
// Called once in the fork child before returning to user code.
void fork_reinit_all();

// ---------------------------------------------------------------------------
// Three-phase fork protocol with pre-fork quiescence.
//
// The fork() implementation calls these at the appropriate points:
//
//   fork_prepare()  — Before RtlCloneUserProcess.
//                     1. libc_fork_quiesce (flush streams, acquire locks)
//                     2. pthread_atfork prepare handlers (LIFO)
//
//   fork_parent()   — In the parent after RtlCloneUserProcess.
//                     1. pthread_atfork parent handlers (FIFO)
//                     2. libc_fork_resume (release locks)
//
//   fork_child()    — In the child after RtlCloneUserProcess.
//                     1. libc_fork_reinit (reset all internal state)
//                     2. pthread_atfork child handlers (FIFO)
// ---------------------------------------------------------------------------
void fork_prepare();
void fork_parent();
void fork_child();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Legacy macro — retained for any remaining section-based registrations.
// New subsystems should instead declare an xxx_fork_reinit() function in
// libc_subsystem_init.h and add the call to libc_fork_reinit_impl.cpp.
#define FORK_REINIT(section, func)                                             \
  static int __fork_reinit_##func() {                                  \
    func();                                                                    \
    return 0;                                                                  \
  }                                                                            \
  __attribute__((used)) __LIBC_SECTION_ATTR(section) static                    \
      LIBC_NAMESPACE::internal::ForkReinitFn                                    \
          __frk_entry_##func = __fork_reinit_##func

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FORK_REINIT_H
