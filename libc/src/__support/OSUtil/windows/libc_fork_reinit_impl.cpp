//===-- Explicit ordered fork child reinit for c.dll -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// libc_fork_reinit() — called in the fork child after NtCreateProcessEx,
// before returning to user code. Replaces the former .FRK$X* section walk.
//
// All reinit ordering is visible in this single function. The child is
// single-threaded when this runs — no lock contention, no races.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

void libc_fork_reinit() {
  // =====================================================================
  // Unseal PCB Zone 0 so PID/parent_pid can be updated.
  // Failure means subsequent Zone 0 writes will AV — fatal.
  // =====================================================================
  if (!pcb_unseal_readonly())
    ::NtTerminateProcess(NtCurrentProcess(), 127);

  // =====================================================================
  // Exception handling — re-register VEH (kernel handles don't survive fork)
  // =====================================================================
  veh_core_fork_reinit();

  // =====================================================================
  // FLS cleanup — deallocate stale Fiber Local Storage without invoking
  // callbacks.  After fork, TEB->FlsData is a CoW copy of the parent's.
  // The FLS callback function pointers are valid (same DLL addresses),
  // but the internal FLS list chains phantom entries for the parent's
  // dead threads.  LdrShutdownProcess/ExitProcess would walk that stale
  // list and process all entries — including non-existent threads.
  //
  // RtlProcessFlsData(FlsData, RTL_FLS_DATA_CLEANUP_DEALLOCATE) frees
  // the FLS data block without invoking per-slot callbacks (flag 2 only,
  // not flag 1).  NULLing TEB->FlsData ensures LdrShutdownProcess finds
  // nothing to process if the child later calls ExitProcess.
  //
  // Must run early — before any reinit that might trigger KERNEL32/
  // KERNELBASE code paths that interact with FLS state.
  // =====================================================================
  {
    void *fls_data = NtCurrentTeb()->FlsData;
    if (fls_data) {
      ::RtlProcessFlsData(fls_data, RTL_FLS_DATA_CLEANUP_DEALLOCATE);
      NtCurrentTeb()->FlsData = nullptr;
    }
  }

  // =====================================================================
  // Identity — refresh uid/gid from child's process token
  // =====================================================================
  identity_fork_reinit();

  // =====================================================================
  // SysV shm — compact the process-local attach table (drop any
  // partially-committed entries from a concurrent shmat at fork time)
  // =====================================================================
  sysv_shm_fork_reinit();

  // =====================================================================
  // Memory subsystem — mapping table recovery, pkey, mmap lock, onfault,
  // VA inventory
  // =====================================================================
  mapping_table_fork_reinit();
  pkey_fork_reinit();
  mmap_lock_fork_reinit();
  mlock_policy_fork_reinit();
  va_inventory_fork_reinit();

  // =====================================================================
  // Environment — lock reset + environ pointer re-sync
  // =====================================================================
  env_fork_reinit();

  // =====================================================================
  // Allocator — reset per-thread caches and locks
  // =====================================================================
  alloc_fork_reinit();

  // =====================================================================
  // Pools and locks — order within this group doesn't matter
  // (all independent lock resets or slab pool resets)
  // =====================================================================
  ofd_pool_fork_reinit();
  brk_fork_reinit();
  file_pool_fork_reinit();
  wait_slot_fork_reinit();

  // =====================================================================
  // Timers — discard stale NT timer handles, reset state
  // =====================================================================
  setitimer_fork_reinit();
  timer_create_fork_reinit();

  // =====================================================================
  // Process subsystems — child table, resource limits, CPU timer
  // =====================================================================
  cpu_limit_timer_fork_reinit();
  child_table_fork_reinit();
  rlimit_fork_reinit();

  // =====================================================================
  // Thread infrastructure pools
  //
  // These pool resets reclaim slab memory from dead (non-forking) threads.
  // They do NOT perform per-object lifecycle cleanup (closing handles,
  // marking robust mutexes OWNER_DIED, etc.) — that work is deferred to
  // registry_fork_reinit() inside signal_fork_reinit() below.
  //
  // This ordering is safe because SlabPool::fork_reinit() only releases
  // slabs where ALL slots have been returned.  Since stale lifecycle
  // objects are still allocated (not freed), their slabs remain committed
  // and accessible when registry_fork_reinit() reads them later.
  //
  // lifecycle_fork_reinit also resets the forking thread's cancellation
  // state and clears stale subsystem pointers (thread_ring, active_syscall).
  //
  // thread_storage_fork_reinit resets the transient ThreadStartStorage
  // pool — no per-object cleanup needed since the objects are short-lived.
  // =====================================================================
  lifecycle_fork_reinit();
  thread_self_fork_reinit();
  robust_pool_fork_reinit();
  thread_ring_fork_reinit();
  thread_storage_fork_reinit();
  named_semaphore_fork_reinit();

  // =====================================================================
  // Fd table — pool reset (after all pool resets above)
  // =====================================================================
  fd_table_fork_reinit();

  // =====================================================================
  // Lock table — release all inherited byte-range locks in the child.
  // POSIX: file locks are NOT inherited across fork(). Handles are still
  // valid at this point (fd_table_fork_reinit only resets pool locks,
  // it does not close handles).
  // =====================================================================
  lock_table_fork_reinit();

  // =====================================================================
  // Console TTY — after fd_table (may touch fd handles), before PTY tree
  // =====================================================================
  console_tty_fork_reinit();

  // =====================================================================
  // Services — reactor (new IOCP + drain thread), then signal (new ALPC port)
  // Must be after fd_table and all pool resets.
  // cpu_limit_timer_fork_restore must run after reactor_fork_reinit
  // because it re-associates the job with the new IOCP.
  //
  // signal_fork_reinit is the thread-trimming checkpoint: it calls
  // registry_fork_reinit(self_lc) which walks all registry slots,
  // closes inherited thread handles, marks robust mutexes OWNER_DIED
  // via cleanup_stale_lifecycle(), and frees dynamically-allocated
  // lifecycles.  After this point, the thread registry contains
  // exactly one entry — the forking thread.
  // =====================================================================
  reactor_fork_reinit();
  cpu_limit_timer_fork_restore();
  signal_fork_reinit();

  // =====================================================================
  // IPC subsystems — must follow reactor (stale WCP/AFD registrations
  // reference the parent IOCP which reactor_fork_reinit() replaced)
  // =====================================================================
  epoll_fork_reinit();
  inotify_fork_reinit();
  dlfcn_fork_reinit();

  pty_tree_fork_reinit();
  vt_pty_fork_reinit();

  // =====================================================================
  // Reseal PCB Zone 0 — canary check + PAGE_READONLY
  // =====================================================================
  if (!pcb_check_canary())
    ::NtTerminateProcess(NtCurrentProcess(), 127);
  // Non-fatal on seal failure (same rationale as init).
  [[maybe_unused]] bool sealed = pcb_seal_readonly();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
