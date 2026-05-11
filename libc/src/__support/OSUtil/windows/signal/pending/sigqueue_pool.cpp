//===-- SlabPool allocator for SigqueueEntry ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// No fixed capacity limit. The old SIGQUEUE_MAX = 1024 hard cap is removed.
// SlabPool::alloc() returns nullptr only on commit failure (system OOM).
// This maps directly to EAGAIN per POSIX sigqueue(3).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/pending/sigqueue_pool.h"

#include "src/__support/OSUtil/windows/alloc/legacy/slab_pool.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

namespace {

using internal::SlabPool;

// Process-global pool instance.
SlabPool pool;

// Per-thread slab pointer. Managed by SlabPool TLS infrastructure.
// thread_local avoids TLS index allocation — the compiler emits it
// directly into the .tls section.
thread_local SlabPool::ThreadSlab my_slab = nullptr;

} // namespace

void sigqueue_pool_init() {
  pool.init(sizeof(SigqueueEntry), alignof(SigqueueEntry),
            /*class_index=*/0);
  pool.init_tls();
}

SigqueueEntry *sigqueue_alloc() {
  // Fast path: thread-local slab has a free slot.
  void *slot = SlabPool::alloc(my_slab);
  if (slot)
    return static_cast<SigqueueEntry *>(slot);

  // Slow path: current slab exhausted or no slab yet.
  // alloc_slow() tries (in order): drain cross-thread frees, reclaim
  // abandoned slabs, allocate a new slab. Returns nullptr only on
  // commit failure.
  slot = pool.alloc_slow(&my_slab);
  if (!slot)
    return nullptr; // OOM → caller returns EAGAIN

  return static_cast<SigqueueEntry *>(slot);
}

void sigqueue_free(SigqueueEntry *entry) {
  // SlabPool::free() is safe for cross-thread frees. It detects whether
  // the freeing thread owns the slab (fast local free) or not (atomic
  // push to the cross-thread freelist with XOR-encoded pointer).
  SlabPool::free(static_cast<void *>(entry));
}

void sigqueue_pool_fork_reinit() {
  // In the fork child, the parent's thread-local slabs are stale.
  // Abandon the current slab (if any) and let the next alloc create
  // a fresh one.
  if (my_slab) {
    pool.abandon(my_slab);
    my_slab = nullptr;
  }
  pool.fork_reinit();
}

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
