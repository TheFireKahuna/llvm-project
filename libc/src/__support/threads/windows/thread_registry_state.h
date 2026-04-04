//===-- Thread registry state for Windows ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Process-wide trivially-destructible state for the thread registry.
// Embedded in `ProcessControlBlock` so fork-reinit can zero it as a
// known-good empty state.
//
// All non-pointer fields zero-init to a valid empty registry. Pointer
// fields (`top_level_base`, `iter_head`) are zero on a virgin
// process; the lazy `init_state` machine populates `top_level_base`
// on first use, and `iter_head` is null while no lifecycle is live.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_STATE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

struct ThreadRegistryState {
  // Lazy-init state machine: 0 = uninit, 1 = initializing, 2 = ready.
  // Loser CASes spin/wait on init_state via futex_addr until the
  // winner publishes 2 (or resets to 0 on failure).
  cpp::Atomic<uint32_t> init_state;

  // Packed (L:32, S:31, split_pending:1). See lockfree_hash.h
  // `decode_hash_state` / `encode_hash_state`. Initial value
  // `encode_hash_state(L=initial_log2, S=0, split_pending=false)`.
  cpp::Atomic<uint64_t> ls_state;

  // Live registered lifecycle count — drives the split trigger.
  // RELAXED-bumped on register / decremented on deregister.
  cpp::Atomic<uint32_t> live_count;

  // Global Harris-chain head of live lifecycles. Bit 0 is unused at
  // the head level (Harris marks live on each entry's own
  // next-pointer); see `harris_*_iter` helpers in lockfree_hash.h.
  // Stored as `uintptr_t` so this header can stay forward-decl-only
  // for `ThreadLifecycle`.
  cpp::Atomic<uintptr_t> iter_head;

  // Pointer to the heap-allocated top-level array of
  // `Atomic<BucketHeadPage*>` (length `kTopLevelCapacity`). Allocated
  // once by the lazy-init path and never modified thereafter. Stored
  // as `uintptr_t` so the BucketHeadPage type can stay forward-decl-
  // only here.
  cpp::Atomic<uintptr_t> top_level_base;

  // CAS flag for serializing fallback `try_split` triggers when the
  // primary `split_pending` claim path loses to a concurrent splitter
  // — losers skip silently. Lock-free overall.
  cpp::Atomic<uint32_t> split_advisory;
};

static_assert(__is_trivially_destructible(ThreadRegistryState),
              "Registry state must be trivially destructible for PCB embedding");

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_STATE_H
