//===--- concurrent_fuzz history impl ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "test/src/__support/concurrent_fuzz/history.h"

#include "src/__support/macros/optimization.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

uint64_t rdtsc_lfence_pre() {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_lfence();
  uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#else
  return 0;
#endif
}

uint64_t rdtsc_lfence_post() {
#if defined(__x86_64__) || defined(_M_X64)
  unsigned int aux;
  uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
#else
  return 0;
#endif
}

void History::init_rings(uint32_t worker_count_, uint32_t per_worker_capacity,
                         HistoryEntry *entry_storage,
                         HistoryEntry *flat_storage,
                         uint32_t flat_storage_capacity) {
  worker_count = worker_count_;
  for (uint32_t w = 0; w < worker_count_ && w < kMaxWorkers; ++w) {
    per_worker[w].entries =
        entry_storage + static_cast<size_t>(w) * per_worker_capacity;
    per_worker[w].capacity = per_worker_capacity;
    per_worker[w].count = 0;
  }
  flat = flat_storage;
  flat_capacity = flat_storage_capacity;
  flat_count = 0;
}

void History::reset_counts() {
  for (uint32_t w = 0; w < worker_count && w < kMaxWorkers; ++w)
    per_worker[w].count = 0;
  flat_count = 0;
}

namespace {

// Comparator key — three components for stable ties.
struct SortKey {
  uint64_t inv_tsc;
  uint16_t worker_id;
  uint32_t op_index;
};

LIBC_INLINE bool key_less(const HistoryEntry &a, const HistoryEntry &b) {
  if (a.inv_tsc != b.inv_tsc)
    return a.inv_tsc < b.inv_tsc;
  if (a.op.worker_id != b.op.worker_id)
    return a.op.worker_id < b.op.worker_id;
  return a.op.op_index < b.op.op_index;
}

// In-place insertion-sort partition pass — fast for nearly-sorted input.
// Concurrent-fuzz histories are usually <4k entries, well within the
// quadratic-fast region for cache-friendly insertion sort. Avoids a
// libc-internal qsort dependency.
void insertion_sort(HistoryEntry *a, uint32_t n) {
  for (uint32_t i = 1; i < n; ++i) {
    HistoryEntry tmp = a[i];
    uint32_t j = i;
    while (j > 0 && key_less(tmp, a[j - 1])) {
      a[j] = a[j - 1];
      --j;
    }
    a[j] = tmp;
  }
}

// 4-way merge of nearly-sorted runs. We exploit the fact that each
// per-worker ring is already sorted by inv_tsc (a single thread sees
// monotonic TSC modulo lfence). For larger thread counts a heap-merge
// is asymptotically better, but at <=32 workers the linear scan is
// competitive and avoids dynamic allocation.
void multi_way_merge(WorkerHistory *rings, uint32_t worker_count,
                     HistoryEntry *out, uint32_t &out_count,
                     uint32_t out_capacity) {
  // Indices into each ring.
  uint32_t idx[History::kMaxWorkers] = {};
  out_count = 0;
  for (;;) {
    int best = -1;
    for (uint32_t w = 0; w < worker_count; ++w) {
      if (idx[w] >= rings[w].count)
        continue;
      if (best < 0 ||
          key_less(rings[w].entries[idx[w]],
                   rings[best].entries[idx[best]]))
        best = static_cast<int>(w);
    }
    if (best < 0)
      break;
    if (out_count >= out_capacity) {
      // Capacity exhaustion — surface via truncation; checker treats
      // truncated histories as invalid (linearizability cannot be
      // claimed without the full history).
      return;
    }
    out[out_count++] = rings[best].entries[idx[best]++];
  }
}

} // namespace

void History::merge() {
  // First pass: ensure each per-worker ring is sorted (it already is by
  // construction — single-thread monotonic TSC — but defend against a
  // virtualized environment that violates invariant TSC).
  for (uint32_t w = 0; w < worker_count && w < kMaxWorkers; ++w)
    insertion_sort(per_worker[w].entries, per_worker[w].count);

  multi_way_merge(per_worker, worker_count, flat, flat_count, flat_capacity);
}

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL
