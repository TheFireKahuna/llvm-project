//===-- Fork reinit dispatcher + pthread_atfork integration ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Three-phase fork protocol with pre-fork quiescence:
//
//   fork_prepare()  — Before NtCreateProcessEx.
//                     1. libc_fork_quiesce() — flush FILE* streams and
//                        acquire critical locks for a consistent snapshot.
//                     2. invoke_prepare_callbacks() — user pthread_atfork
//                        prepare handlers (LIFO, per POSIX).
//
//   fork_parent()   — In the parent after fork returns.
//                     1. invoke_parent_callbacks() — user pthread_atfork
//                        parent handlers (FIFO, per POSIX).
//                     2. libc_fork_resume() — release critical locks.
//
//   fork_child()    — In the child (single-threaded).
//                     1. libc_fork_reinit() — reset all internal subsystem
//                        state (resets critical locks, rebuilds kernel
//                        objects, trims thread registry to one thread).
//                     2. invoke_child_callbacks() — user pthread_atfork
//                        child handlers (FIFO, per POSIX).
//
//                     Note: libc_fork_reinit() runs BEFORE user child
//                     callbacks so that user code sees consistent internal
//                     state (fresh reactor, valid ALPC port, reset locks).
//                     This matches glibc's ordering.
//
// fork_reinit_all() — legacy entry point, delegates to fork_child().
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/fork_reinit.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/threads/fork_callbacks.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Weak defaults — overridden by strong definitions in
// libc_fork_reinit_impl.cpp and fork_quiesce.cpp (linked into c.dll).
// Test binaries that transitively pull in fork_reinit but don't link
// the full DLL subsystem graph get these no-ops.
[[gnu::weak]] void libc_fork_reinit() {}
[[gnu::weak]] void libc_fork_quiesce() {}
[[gnu::weak]] void libc_fork_resume() {}

void fork_prepare() {
  // Internal quiescence: flush buffered streams, then acquire critical
  // locks (mmap, environ, signal handler, child table).
  //
  // Streams are flushed and released BEFORE lock acquisition, so user
  // prepare handlers can still use stdio (printf, fclose, etc.).
  //
  // Critical locks ARE held when user prepare handlers execute.  This
  // is intentional: holding them through the snapshot prevents torn
  // invariants.  User code that calls mmap/setenv/sigaction from a
  // prepare handler will deadlock on the same-thread re-acquisition.
  // This is standard practice (glibc acquires internal locks before
  // running user prepare handlers for the same reason).
  libc_fork_quiesce();

  // User prepare handlers (LIFO order per POSIX).
  invoke_prepare_callbacks();
}

void fork_parent() {
  // User parent handlers (FIFO order per POSIX).
  invoke_parent_callbacks();

  // Release critical locks (reverse of acquisition order).
  libc_fork_resume();
}

void fork_child() {
  // Reset all internal subsystem state BEFORE user child callbacks.
  // This ensures user code sees consistent state: fresh reactor,
  // valid ALPC port, reset locks, single-thread registry.
  // Matches glibc's ordering (internal reinit, then user callbacks).
  libc_fork_reinit();

  // User child handlers (FIFO order per POSIX).
  invoke_child_callbacks();
}

void fork_reinit_all() {
  // Legacy entry point — equivalent to fork_child().
  fork_child();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
