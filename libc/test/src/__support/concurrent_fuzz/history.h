//===--- concurrent_fuzz history -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Concurrent execution history. Each worker writes its own append-only
// `WorkerHistory` ring during the run; after `stop`, `History::merge`
// builds a flat array sorted by invocation TSC for the linearizability
// checker.
//
// Per-worker rings are single-producer / single-consumer; the producer
// is the worker thread, the consumer is the merge step that runs after
// every worker has joined. No atomics on the hot path; the merge step
// reads after a join-edge ACQUIRE so visibility is guaranteed by the
// thread-join NTSTATUS handshake.
//
// TSC is bracketed by `LFENCE` per Intel's invariant-TSC guidance; on
// the x86-64-v3 floor this codebase enforces (CLAUDE.md), the result is
// monotonic across CPUs and sufficient for real-time-order extraction.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_HISTORY_H
#define LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_HISTORY_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "test/src/__support/concurrent_fuzz/op.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

struct HistoryEntry {
  uint64_t inv_tsc;
  uint64_t res_tsc;
  Op op;
  OpResult result;
};

// Read TSC bracketed by LFENCE so the timestamp orders strictly with
// surrounding loads/stores. `rdtscp` provides the second LFENCE-like
// guarantee on the trailing edge; we add an LFENCE on the leading edge
// for symmetry.
[[nodiscard]] uint64_t rdtsc_lfence_pre();
[[nodiscard]] uint64_t rdtsc_lfence_post();

struct WorkerHistory {
  HistoryEntry *entries{nullptr};
  uint32_t capacity{0};
  uint32_t count{0};

  // Single-producer append. Returns false on capacity exhaustion (the
  // worker treats this as a soft stop — the run continues but the
  // entry is dropped).
  [[nodiscard]] bool push(const HistoryEntry &e) {
    if (count >= capacity)
      return false;
    entries[count] = e;
    ++count;
    return true;
  }
};

struct History {
  static constexpr uint32_t kMaxWorkers = 32;
  // Per-worker rings. capacity = ops_per_worker.
  WorkerHistory per_worker[kMaxWorkers];
  uint32_t worker_count{0};

  // Flat sorted view, populated by `merge()` after all workers stop.
  HistoryEntry *flat{nullptr};
  uint32_t flat_count{0};
  uint32_t flat_capacity{0};

  // Initialize per-worker rings against caller-supplied storage.
  // `entry_storage` points at flat backing memory of size
  // `worker_count * per_worker_capacity` HistoryEntries; this struct
  // partitions it across rings.
  void init_rings(uint32_t worker_count_, uint32_t per_worker_capacity,
                  HistoryEntry *entry_storage, HistoryEntry *flat_storage,
                  uint32_t flat_storage_capacity);

  // Build `flat` from per-worker rings, sorted by `inv_tsc`. Stable on
  // ties (uses (inv_tsc, worker_id, op_index) as the comparison key).
  void merge();

  // Reset per-worker counts and `flat_count` to 0 without reallocating.
  // Used by the shrinker between replay attempts.
  void reset_counts();
};

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_HISTORY_H
