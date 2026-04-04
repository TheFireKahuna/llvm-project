//===-- Canonical reactor state for Windows ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lightweight process-wide state for the IOCP reactor. The growable slot pool
// and active list remain implementation-private in reactor.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_REACTOR_REACTOR_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_REACTOR_REACTOR_STATE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace reactor {

// Per-thread drain epoch for RCU-style fencing. Cache-line aligned to
// eliminate false sharing — each drain thread writes only its own slot
// on every batch, so no cross-thread contention on the hot path.
struct alignas(64) DrainEpoch {
  cpp::Atomic<uint64_t> value{0};
};

struct ReactorState {
  // Maximum drain pool threads. 4 covers typical core counts while keeping
  // resource use bounded. init() starts min(processor_count, MAX) threads.
  static constexpr uint32_t MAX_DRAIN_THREADS = 4;

  HANDLE iocp;
  HANDLE reserve;
  HANDLE drain_thread;                   // Single drain thread handle.
  HANDLE drain_threads_reserved[MAX_DRAIN_THREADS - 1]; // Future expansion.
  uint32_t drain_thread_count;           // Threads actually started.
  cpp::Atomic<uint32_t> shutdown;        // Non-zero = shutting down.
  cpp::Atomic<uint32_t> drain_exit_count; // Threads that have left the loop.
  cpp::Atomic<uint32_t> drain_cleaned;   // Non-zero = last thread finished cleanup.
  cpp::Atomic<uint64_t> heartbeat;       // Global heartbeat (any-thread progress).
  cpp::Atomic<uint32_t> generation;      // Monotonic ABA counter.
  cpp::Atomic<uintptr_t> router;         // CompletionRouter as uintptr_t.

  // Per-thread epoch array. drain_thread_proc bumps drain_epochs[my_index]
  // after every batch. fence_drain_cycle() snapshots all N, wakes all N,
  // waits for each to advance — guarantees every thread has cycled.
  DrainEpoch drain_epochs[MAX_DRAIN_THREADS];
};

} // namespace reactor
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_REACTOR_REACTOR_STATE_H
