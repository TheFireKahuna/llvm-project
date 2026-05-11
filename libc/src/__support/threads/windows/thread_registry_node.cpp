//===-- ThreadRegistryNode — FreeFn dispatch ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Crystalline FreeFn for the thread-registry domain. Routes a fully-
// quiesced retirable node to its underlying allocator based on `kind`.
//
// Invoked by Crystalline's reclamation phase (`free_list`) when the
// node's batch refcount has wrapped to zero — at which point no thread
// holds a reservation pinning the node. Plain (non-atomic) reads of
// `kind` and any subsequent allocator interactions are safe.
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/thread_registry_node.h"

#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/threads/windows/lockfree_hash.h"
#include "src/__support/threads/windows/thread_lifecycle.h"

namespace LIBC_NAMESPACE_DECL {

// Robust-mutex teardown — runs before slab return so the lifecycle's
// held-mutex chain is severed before its memory is reused.
namespace robust_mutex {
void lifecycle_destroy(ThreadLifecycle *lc);
} // namespace robust_mutex

// BucketEntry slab free path (defined in thread_registry.cpp alongside
// the slab pool itself). Forward-declared here to keep the dispatcher
// agnostic to the pool's storage details.
void free_bucket_entry_slab(BucketEntry *entry);

void free_thread_registry_node(ThreadRegistryNode *node) {
  if (!node)
    return;

  switch (node->kind) {
  case ThreadRegistryNodeKind::Lifecycle: {
    auto *lc = static_cast<ThreadLifecycle *>(node);
    robust_mutex::lifecycle_destroy(lc);
    // Close the thread handle here, AFTER Crystalline confirmed no
    // reservation pins lc. Cross-thread borrowers (cancel APC,
    // registry_suspend/resume, signal delivery) reach the handle via
    // registry_borrow_handle which transitively pins through
    // Crystalline; so the handle is observably valid for any borrower
    // whose reservation pins lc, and it is guaranteed unreached by
    // the time we get here. Eliminates the cross-thread
    // closed-handle reuse race (C4).
    HANDLE h = lc->thread_handle.exchange(nullptr,
                                          cpp::MemoryOrder::ACQ_REL);
    if (h)
      ::NtClose(h);
    free_lifecycle(lc);
    return;
  }
  case ThreadRegistryNodeKind::BucketHeadPage: {
    // Pages are page_alloc-sized — return to the OS allocator
    // directly. No slab pool here: pages are large and infrequent
    // (one per ~512 buckets), so per-allocation accounting is fine.
    auto *page = static_cast<BucketHeadPage *>(node);
    internal::page_free(page);
    return;
  }
  case ThreadRegistryNodeKind::BucketEntry: {
    free_bucket_entry_slab(static_cast<BucketEntry *>(node));
    return;
  }
  }
  // Unrecognized kind — corrupted node header. Trap rather than leak.
  __builtin_trap();
}

} // namespace LIBC_NAMESPACE_DECL
