//===-- Pre-fork quiescence + post-fork resume ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pre-fork quiescence protocol.  Called from fork_prepare() BEFORE the address
// space snapshot (NtCreateProcessEx) and BEFORE user pthread_atfork prepare
// handlers.
//
// Two concerns:
//
//   1. FILE* stream flushing — POSIX strongly recommends flushing buffered
//      output before fork to prevent the child from inheriting stale write
//      buffers that would be re-flushed on exit (duplicating output).
//
//   2. Critical lock acquisition — acquires locks whose protected data
//      structures cannot be safely rebuilt from scratch by fork_reinit if
//      a non-forking thread was mid-update when the snapshot was taken.
//      Holding these locks guarantees a consistent snapshot.
//
// Stale reactor completions are handled separately by reactor_fork_reinit()
// in the child, which closes the inherited IOCP and creates a fresh one.
//
// Lock acquisition order (must match to avoid ABBA deadlocks):
//
//   1. MmapLock        (exclusive — prevents VA mutations)
//   2. EnvironmentLock (prevents environ array changes)
//   3. SignalHandler   (prevents sigaction changes mid-snapshot)
//   4. ChildTable      (prevents child list mutations)
//
// FILE* streams are flushed and released before lock acquisition, not held
// across the fork.  This avoids re-entrancy hazards: user atfork prepare
// handlers may call stdio functions (printf, fclose, etc.) and must not
// find per-file mutexes already locked by the same thread.
//
// Post-fork parent: release all locks (reverse order).
// Post-fork child:  locks are reset by libc_fork_reinit() — no explicit
//                   release needed.
//
//===----------------------------------------------------------------------===//

#include "src/__support/File/file.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/memory/mmap_lock.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// =========================================================================
// Phase 1: Flush all buffered FILE* streams (best-effort)
// =========================================================================
//
// Uses try_lock_for_flush() per-file to avoid blocking on a file that
// another thread is actively writing — that thread will flush its own
// buffer when it releases the lock or closes the file.
//
// Standard streams (stdin/stdout/stderr) are in the dynamic file list
// because fd_table_std_fds_startup_init() adds them via File::add_file().
// No separate handling is needed.

static void flush_all_streams() {
  File::lock_list();
  for (File *f = File::get_first_file(); f != nullptr; f = f->get_next()) {
    if (f->try_lock_for_flush()) {
      f->flush_unlocked();
      f->unlock();
    }
    // Streams that fail try_lock are being actively used by another
    // thread.  That thread holds the per-file mutex, so it will either
    // flush on its next write/close, or the child inherits the buffered
    // data as-is (acceptable — the thread may still be mid-write).
  }
  File::unlock_list();
}

// =========================================================================
// Phase 2: Acquire critical locks in defined order
// =========================================================================
//
// These locks protect data structures whose internal invariants could
// be violated if a non-forking thread was mid-update during the snapshot.
// fork_reinit resets locks but cannot always repair torn invariants:
//
//   MmapLock     — mapping table + VA layout consistency during remap
//   env lock     — environ array pointer + count + capacity consistency
//   signal lock  — handler table array consistency during sigaction
//   child table  — child entry list + PGID index consistency

static void acquire_critical_locks() {
  windows::g_mmap_lock.acquire_exclusive();
  g_pcb.environment.lock.lock();
  g_pcb.signal_handler.lock.lock();
  g_pcb.child_table.lock.lock();
}

static void release_critical_locks() {
  g_pcb.child_table.lock.unlock();
  g_pcb.signal_handler.lock.unlock();
  g_pcb.environment.lock.unlock();
  windows::g_mmap_lock.release_exclusive();
}

// =========================================================================
// Public API
// =========================================================================

void libc_fork_quiesce() {
  // Flush first (releases all file locks before we acquire critical locks).
  flush_all_streams();

  // Acquire critical locks.  After this point, no other thread can mutate
  // the protected data structures — the snapshot will be consistent.
  acquire_critical_locks();
}

void libc_fork_resume() {
  // Release critical locks in reverse order (parent path only).
  // The child path resets all locks via libc_fork_reinit().
  release_critical_locks();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
