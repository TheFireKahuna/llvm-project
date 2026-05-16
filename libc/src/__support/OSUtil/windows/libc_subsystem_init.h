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
//   xxx_fork_reinit()      — fork child reinit (void, traps on failure)
//
// Process-shutdown finis are not declared here; they are registered into
// `.libcfin$P<phase>` via LIBC_REGISTER_FINI in their own TU. See
// libc_fini_registry.h.
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

// Phase 0c.5 (Tier A): Mapping table + substrate + future memory
// primitives are now brought up declaratively via the `.libcmem`
// walker — see memory/memory_primitives_bootstrap.h. The individual
// subsystem entry points are no longer called directly from
// libc_bootstrap; their init_fn handlers run under the walker.

// Phase 3: Allocator + memory subsystem
// posix_alloc_init() and mmap_subsystem_init() are called directly — they
// have their own declarations in their respective headers.

// Phase 4: Object pools
int lifecycle_startup_init();
int ofd_pool_startup_init();
int file_pool_startup_init();
int wait_slot_startup_init();
// named_semaphore is lazy-init'd via LazyInit on first sem_open() — see
// lazy_init.h and lazy_init_reset.h. Eager Phase 4 init removed.
int robust_pool_startup_init();
int thread_ring_startup_init();
int thread_storage_startup_init();

// Phase 4b: brk subsystem (after pools, uses page_alloc)
int brk_startup_init();

// Phase 5: VEH fault handlers

// Phase 6: Fd table
int fd_table_startup_init();

// Phase 7: Services
int reactor_startup_init();
// Installs the socket-io completion handler. Must run after the reactor's
// IOCP is up so sockets created during Tier B / user code can bind their
// handles immediately. Purely a router registration; no background threads.
int socket_io_startup_init();
int signal_startup_init();
// Registers the OP_LOCK_QUERY handler on the ALPC bus. Must run before
// alpc_bus_startup_init() so the handler is live as soon as the port comes
// up — an inbound query arriving mid-init would otherwise see
// STATUS_NOT_SUPPORTED.
int lock_table_startup_init();
// Brings up the per-process ALPC port. Every `register_handler` call
// from every consumer must precede this so the port opens with a
// complete dispatch table.
int alpc_bus_startup_init();

// Phase 8: Stdio
// vt_pty and console migrated to LazyInit — see vt_pty.cpp (ensure on
// pty_tree::current_attached_pty_id / has_current_attached_pty) and
// console.cpp (ensure on console::current_reference). Both are
// `.libclzr`-registered so exec_self_hollow() clears their gates.
int fd_table_std_fds_startup_init();

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
// after RtlCloneUserProcess. Void return; failures call
// NtTerminateProcess(NtCurrentProcess(), 127).
// =========================================================================

void veh_core_fork_reinit();
void identity_fork_reinit();
// Crystalline SMR fork-reinit. MUST run before va_substrate_fork_reinit
// (and any other CrystallineDomain consumer's fork-reinit) because the
// substrate's pool-state fork-reinit reads `live_count` on surviving
// arenas to rebuild active_/abandoned_head_, and a stale Crystalline
// retire batch (inherited from the parent) referencing one of those
// arenas would leave a dangling retire pointer in a child thread's
// per-domain region. The walker zeroes every surviving thread's
// per-domain slot/batch state in one shot.
void crystalline_fork_reinit();
void va_substrate_fork_reinit();
void mapping_table_fork_reinit();
// ThreadScratch per-thread arenas (primary + overflow). Substrate-aware:
// dead threads' substrate-served slots release through `substrate_release`,
// bootstrap-tier dead threads page_free their raw NT reservation and
// unlink the synthetic ArenaHeader. Must run AFTER va_substrate_fork_reinit
// (substrate_release walks the now-rebuilt pool state) and AFTER
// mapping_table_fork_reinit (substrate's retire path can transitively
// touch the mapping table when an arena drains).
void scratch_fork_reinit();
void pkey_fork_reinit();
void mmap_lock_fork_reinit();
void mlock_policy_fork_reinit();

// Region pool reset + post-fork/exec reconciliation. Both calls must run
// AFTER mmap_lock_fork_reinit so the bulk-VA scan can acquire MmapLock
// writer cleanly. The pool's internal lock state is reset here so the
// sweep is single-threaded as far as our code is concerned.
void memory_reconcile_fork_reinit();
void memory_reconcile_exec_reinit();
void va_inventory_fork_reinit();
void env_fork_reinit();
void alloc_fork_reinit();
void alloc_exec_reinit();
void ofd_pool_fork_reinit();
void brk_fork_reinit();
void file_pool_fork_reinit();
void wait_slot_fork_reinit();
void futex_addr_fork_reinit();
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
// Rebuilds the per-process ALPC port after the parent's port handle
// vanished with the cloned handle table. Runs after reactor_fork_reinit()
// (bus registers with the reactor) and before signal_fork_reinit() /
// anything else that may emit a cross-process ALPC request.
void alpc_bus_fork_reinit();
void signal_fork_reinit();
void pty_tree_fork_reinit();
void sysv_shm_fork_reinit();
void vt_pty_fork_reinit();
void epoll_fork_reinit();
void inotify_fork_reinit();
void dlfcn_fork_reinit();
void rdebug_fork_reinit();

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
// (FreeLibrary only, not process exit).
//
// Per-subsystem finis are not declared here. Each subsystem registers
// its teardown thunk into `.libcfin$P<phase>` via LIBC_REGISTER_FINI in
// its own TU; __libc_dll_fini() walks the section in reverse phase
// order. See libc_fini_registry.h for phase assignments and ordering.
// =========================================================================

// Latches the Tier B init gate so a subsequent LoadLibrary("c.dll") +
// _DllMainCRTStartup → __libc_dll_init() returns failure rather than
// skipping Tier B against half-destroyed pools. Called first from
// __libc_dll_fini(), before the .libcfin sweep.
void mark_dll_init_poisoned();

// Returns true once mark_dll_init_poisoned() has latched. Used by the
// fini walker's debug assertion to enforce the "poison-then-sweep" order
// required so a racing LoadLibrary cannot enter Tier B against a fini
// already in flight.
bool dll_init_is_poisoned();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// =========================================================================
// Top-level init/reinit/fini — called from c.dll's _DllMainCRTStartup
// and the fork syscall wrapper. Declared extern "C" for simple linkage
// from the startup .obj entry point.
// =========================================================================

extern "C" {

// Tier A — runs before any Tier B subsystem init, any user C initializer,
// any C++ constructor, any allocation or signal path. Brings the process
// into a state where code can execute safely:
//   OS floor check, PCB Zone 0, master VEH, identity, seal Zone 0.
// Idempotent (internal CAS gate); safe for both _DllMainCRTStartup and
// EXE-entry paths to call.
void __libc_bootstrap();

// Tier B — called by c.dll _DllMainCRTStartup on DLL_PROCESS_ATTACH after
// __libc_bootstrap() returns. Brings up allocator, thread registry, fd
// table, signal dispatch, reactor, stdio skeleton — everything user code
// expects ready by the time main() runs. Returns 0 on success, non-zero
// on failure.
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
