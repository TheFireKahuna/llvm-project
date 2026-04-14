//===-- Explicit subsystem init/reinit/fini declarations ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Central declarations for all libc subsystem startup, fork-reinit, and
// shutdown functions. These are called explicitly by __libc_dll_init(),
// __libc_fork_reinit(), and __libc_dll_fini() respectively — replacing
// the former .CRT$XI* / .FRK$X* / .CRT$XP* section-merged dispatch.
//
// This eliminates function-pointer-array dispatch (CFG-friendly), makes
// init ordering source-visible and auditable, and removes fragile section
// letter assignments.
//
// Naming convention:
//   xxx_startup_init()     — process init, returns 0 on success
//   xxx_startup_fini()     ��� process shutdown (reverse of init)
//   xxx_fork_reinit()      — fork child reinit (void, traps on failure)
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LIBC_SUBSYSTEM_INIT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LIBC_SUBSYSTEM_INIT_H

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// =========================================================================
// Process startup init — called by __libc_dll_init() in c.dll
// DLL_PROCESS_ATTACH. Return 0 on success, non-zero on failure.
// =========================================================================

// Phase 0: PCB constants (page_size, cookie, module_handle, canary, pid)
int pcb_startup_init();

// Phase 0.5: Process mitigation policies (DEP permanent, ASLR force-relocate,
// strict handles, extension point disable, CFG strict, image load restrictions,
// side-channel isolation, CET shadow stack). Locks down the process before any
// subsystem infrastructure is created.
int mitigation_policy_startup_init();

// Phase 1: Exception handling infrastructure
int veh_reentry_guard_startup_init();
int veh_core_startup_init();

// Phase 2: Process identity (uid/gid/privilege, parent_pid)
int identity_startup_init();

// Phase 3: Allocator + memory subsystem
// posix_alloc_init() and mmap_subsystem_init() are called directly — they
// have their own declarations in their respective headers.

// Phase 4: Object pools
int lifecycle_startup_init();
int ofd_pool_startup_init();
int file_pool_startup_init();
int wait_slot_startup_init();
int named_semaphore_startup_init();
int robust_pool_startup_init();
int thread_ring_startup_init();
int thread_storage_startup_init();

// Phase 4b: brk subsystem (after pools, uses page_alloc)
int brk_startup_init();

// Phase 5: VEH fault handlers
int mem_fault_startup_init();
int mlock_policy_startup_init();

// Phase 6: Fd table
int fd_table_startup_init();

// Phase 7: Services
int reactor_startup_init();
int signal_startup_init();

// Phase 8: Stdio + console
int fd_table_std_fds_startup_init();
int vt_pty_startup_init();
int console_startup_init();

// =========================================================================
// Pre-fork quiescence / post-fork resume
//
// libc_fork_quiesce() — called from fork_prepare() BEFORE user
//   pthread_atfork prepare handlers.  Flushes FILE* streams and
//   acquires critical locks to ensure a consistent address-space
//   snapshot.
//
// libc_fork_resume()  — called from fork_parent() AFTER user
//   pthread_atfork parent handlers.  Releases the locks acquired
//   by libc_fork_quiesce().  NOT called in the child — locks are
//   reset by libc_fork_reinit().
// =========================================================================

void libc_fork_quiesce();
void libc_fork_resume();

// =========================================================================
// Fork child reinit — called by __libc_fork_reinit() in the child
// after NtCreateProcessEx. Void return; failures call
// NtTerminateProcess(NtCurrentProcess(), 127).
// =========================================================================

void veh_core_fork_reinit();
void identity_fork_reinit();
void mapping_table_fork_reinit();
void pkey_fork_reinit();
void mmap_lock_fork_reinit();
void mlock_policy_fork_reinit();
void va_inventory_fork_reinit();
void env_fork_reinit();
void alloc_fork_reinit();
void alloc_exec_reinit();
void ofd_pool_fork_reinit();
void brk_fork_reinit();
void file_pool_fork_reinit();
void wait_slot_fork_reinit();
void setitimer_fork_reinit();
void timer_create_fork_reinit();
void console_tty_fork_reinit();
void cpu_limit_timer_fork_reinit();
void child_table_fork_reinit();
void rlimit_fork_reinit();
void lifecycle_fork_reinit();
void thread_self_fork_reinit();
void robust_pool_fork_reinit();
void thread_ring_fork_reinit();
void thread_storage_fork_reinit();
void named_semaphore_fork_reinit();
void fd_table_fork_reinit();
void lock_table_fork_reinit();
void reactor_fork_reinit();
void cpu_limit_timer_fork_restore();
void signal_fork_reinit();
void pty_tree_fork_reinit();
void sysv_shm_fork_reinit();
void vt_pty_fork_reinit();
void epoll_fork_reinit();
void inotify_fork_reinit();
void dlfcn_fork_reinit();

// =========================================================================
// Exec teardown — called from exec_self_hollow() Phase 5, after
// exec_quiesce_threads(). Single-threaded: all non-current threads are
// frozen, so lock resets are safe.
//
// Naming convention:
//   xxx_exec_teardown()   — exec-specific destroy (closes live handles)
//   xxx_fork_reinit()     — reused where exec needs identical lock resets
// =========================================================================

// Issue #2: POSIX exec — caught handlers → SIG_DFL; SIG_IGN preserved.
// Also clears process-pending signals (implementation-defined, we choose
// to clear) and resets sigaltstack.
void signal_exec_reset_handlers();

// Issue #3: POSIX exec — timer_create timers invalidated, setitimer
// disarmed. Unlike fork (handles orphaned), exec handles are live —
// must cancel + close + free.
void timer_create_exec_teardown();
void setitimer_exec_teardown();

// =========================================================================
// Process shutdown — called by __libc_dll_fini() on DLL_PROCESS_DETACH
// (FreeLibrary only, not process exit). Reverse of init order.
// =========================================================================

void veh_core_startup_fini();
void signal_startup_fini();
void mlock_policy_startup_fini();
void reactor_startup_fini();
void mapping_table_startup_fini();
void file_pool_startup_fini();
void ofd_pool_startup_fini();
void named_semaphore_startup_fini();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// =========================================================================
// Top-level init/reinit/fini — called from c.dll's _DllMainCRTStartup
// and the fork syscall wrapper. Declared extern "C" for simple linkage
// from the startup .obj entry point.
// =========================================================================

extern "C" {

// Called by c.dll _DllMainCRTStartup on DLL_PROCESS_ATTACH.
// Returns 0 on success, non-zero on failure.
int __libc_dll_init();

// Called by c.dll _DllMainCRTStartup on DLL_PROCESS_DETACH (FreeLibrary).
void __libc_dll_fini();

} // extern "C"

// Called by the fork syscall wrapper in the child process.
// Not extern "C" — internal to c.dll, called from namespaced code.
namespace LIBC_NAMESPACE_DECL {
namespace internal {
void libc_fork_reinit();
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LIBC_SUBSYSTEM_INIT_H
