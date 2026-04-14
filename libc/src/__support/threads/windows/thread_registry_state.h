//===-- Thread registry state for Windows ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Trivially-destructible state embedded in ProcessControlBlock for the
// thread registry. Zero-initialization is a valid empty state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_STATE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {

struct ThreadRegistryState {
  // Reserved VA base for the SlotPage pointer array. Demand-committed.
  // Each committed pointer slot holds a SlotPage* (8 bytes on x64).
  cpp::Atomic<uintptr_t> page_index_base;

  // Number of SlotPages that have been allocated and stored in the index.
  cpp::Atomic<uint32_t> page_count;

  // Hint: lowest page index likely to have a free slot. Avoids scanning
  // from page 0 on every registration.
  cpp::Atomic<uint32_t> free_hint;

  // Pointer to the current TID hash table (open-addressing, power-of-2).
  // Readers load this once per lookup; old tables are epoch-retired on resize.
  cpp::Atomic<uintptr_t> tid_table;

  // Global epoch counter. Incremented by writers (hash resize, synchronize).
  // Readers pin the current epoch in their slot's pinned_epoch field.
  cpp::Atomic<uint64_t> global_epoch;

  // CAS flag for serializing hash table resize. 0 = unlocked, 1 = locked.
  cpp::Atomic<uint32_t> resize_lock;

  // CAS flag for serializing reclamation passes. 0 = unlocked, 1 = locked.
  // Prevents concurrent registry_try_reclaim from double-bumping the epoch
  // and doing redundant work. Losers skip silently — the winner's pass
  // covers all pending items.
  cpp::Atomic<uint32_t> reclaim_lock;

  // Number of currently registered (live) threads.
  cpp::Atomic<uint32_t> live_count;

  // Number of tombstones in the current TID hash table. Included in the
  // resize heuristic so high-churn workloads don't degrade to O(capacity).
  cpp::Atomic<uint32_t> tombstone_count;

  // Singly-linked list of old hash tables pending epoch-safe free.
  cpp::Atomic<uintptr_t> retired_tables;

  // Singly-linked list of empty pages pending epoch-safe free.
  cpp::Atomic<uintptr_t> retired_pages;

  // Singly-linked list of deregistered lifecycles pending epoch-safe free.
  // Each lifecycle is stamped with retired_epoch at the time of deregistration.
  cpp::Atomic<uintptr_t> retired_lifecycles;

  // Monotonic counter of deregistrations since last reclamation attempt.
  // Used to amortize the reclamation scan: try_reclaim runs every N dereg's.
  cpp::Atomic<uint32_t> retire_count;

  // Lazy init state: 0 = uninit, 1 = initializing, 2 = ready.
  cpp::Atomic<uint32_t> init_state;

  // High-water mark: bytes of the page index VA that have been committed.
  // Starts at 4096 (one page committed during init). Avoids redundant
  // page_commit calls on already-committed memory.
  cpp::Atomic<uint32_t> index_committed_bytes;
};

static_assert(__is_trivially_destructible(ThreadRegistryState),
              "Registry state must be trivially destructible for PCB");

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_STATE_H
