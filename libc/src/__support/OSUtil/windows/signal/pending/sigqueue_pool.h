//===-- SlabPool allocator for SigqueueEntry ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pool allocator for SigqueueEntry nodes used by the RT signal queues.
//
// Backed by SlabPool — thread-owning slab allocator with guard pages,
// canary hardening, and automatic page decommit for empty slabs. No fixed
// capacity limit (the old SIGQUEUE_MAX = 1024 is removed). Allocation
// returns nullptr only on genuine commit failure (system OOM), which maps
// to EAGAIN per POSIX sigqueue(3).
//
// This is the "no limits" design: capacity scales with available memory.
// Back-pressure is natural — SlabPool grows by committing 4KB pages and
// shrinks by decommitting them. No CAS-loop outstanding_count, no arbitrary
// thresholds.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PENDING_SIGQUEUE_POOL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PENDING_SIGQUEUE_POOL_H

#include "src/__support/OSUtil/windows/signal/pending/rt_queue.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// Allocate a SigqueueEntry from the per-thread slab.
// Returns nullptr only on system OOM (commit failure).
// The returned entry has indeterminate field values — caller must initialize.
SigqueueEntry *sigqueue_alloc();

// Return a SigqueueEntry to the pool. Cross-thread free is safe (SlabPool
// uses an atomic cross-thread freelist with XOR-encoded pointers).
void sigqueue_free(SigqueueEntry *entry);

// Initialize the pool. Called once from signal_subsystem_init().
void sigqueue_pool_init();

// Fork reinit: abandon current thread's slab, reset TLS.
void sigqueue_pool_fork_reinit();

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PENDING_SIGQUEUE_POOL_H
