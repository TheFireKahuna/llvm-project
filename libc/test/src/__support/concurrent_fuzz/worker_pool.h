//===--- concurrent_fuzz worker pool ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Spawns N worker threads, distributes the schedule's worker slices,
// applies each op against the SUT, and records `HistoryEntry`s into a
// supplied `History`. Workers run a tight loop bracketed by TSC/lfence;
// no synchronization between workers beyond the implicit acquire on
// thread-join after every worker's slice completes.
//
// Affinity (best-effort): the pool optionally pins each worker to a
// distinct logical CPU via `NtSetInformationThread(ThreadSelectedCpuSets,
// ...)`, with the CPU set IDs probed once at startup. If the probe
// fails (low-level virtualisation, no CPU set support), the pool runs
// unpinned — schedule-level coverage still applies, only the ART arena
// routing diversity weakens.
//
// Thread entry must be MS-ABI: NT calls the entry point under MS x64.
// The framework's caller-visible `SutApplyFn` signature is SysV (the
// libc-internal default for these targets); the worker pool wraps
// SysV ↔ MS-ABI at the thread-entry boundary.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_WORKER_POOL_H
#define LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_WORKER_POOL_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "test/src/__support/concurrent_fuzz/history.h"
#include "test/src/__support/concurrent_fuzz/op.h"
#include "test/src/__support/concurrent_fuzz/schedule.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

// SUT apply function. Called concurrently from worker threads against
// `sut_ctx`; the SUT is responsible for any required internal locking
// (the data structure under test is normally lock-free, hence "concurrent
// fuzz"). Must be SysV ABI per project convention.
typedef OpResult (*SutApplyFn)(void *sut_ctx, const Op &op);

// Per-thread initialization hook. Called on the worker thread before
// any op is dispatched. SUTs use this to call
// `concurrent::registry_warm_thread_all()` or equivalent once-per-
// thread setup. Optional.
typedef void (*WorkerInitFn)(void *sut_ctx);

struct PoolParams {
  // Pin workers to distinct logical CPUs. Best-effort.
  bool pin_affinity{true};

  // Cumulative slice budget (TSC ticks). The worker bails on its
  // remaining slice if its TSC delta from slice start crosses this. A
  // one-op-stall-forever bug still wedges until the budget elapses;
  // pair with `per_op_tsc_budget` for tighter loops. Sentinel 0 =
  // unbounded.
  uint64_t per_worker_tsc_budget{0};

  // Per-op TSC budget. The worker stamps each op's res_tsc; if the
  // delta `res_tsc - inv_tsc` exceeds this, the HistoryEntry's status
  // is rewritten to `kOpResultStatusTimeoutHint` and the worker breaks
  // out of its slice loop. The first-offender op's TSC delta and kind
  // are surfaced through `PoolStats::slowest_op_*` so CI logs identify
  // the culprit instead of "test timed out". Sentinel 0 = unbounded.
  uint64_t per_op_tsc_budget{0};
};

struct PoolStats {
  uint64_t pinned_workers; // # workers that successfully pinned
  uint64_t total_ops_run;  // sum of HistoryEntry counts across rings
  uint64_t timed_out_workers; // # workers that hit per_worker_tsc_budget
  uint64_t per_op_timeouts;   // # ops that hit per_op_tsc_budget

  // Slowest op observed across the run. Only meaningful when
  // `per_op_timeouts > 0` or when the caller wants p99 telemetry on a
  // clean run.
  uint64_t slowest_op_dt_tsc;   // res_tsc - inv_tsc of the slowest op
  uint64_t slowest_op_inv_tsc;  // its inv_tsc, for cross-worker timeline
  uint16_t slowest_op_kind;     // Op::kind of the slowest op
  uint16_t slowest_op_worker_id;// owning worker
};

// Run `schedule` against `sut`, recording into `history`. Spawns
// `schedule.thread_count` workers, joins them, and merges the history.
//
// Returns 0 on success, -errno on a setup failure (thread spawn,
// affinity probe). A successful run does NOT imply linearizability —
// that's the checker's job. A successful run does imply: every op was
// dispatched at least once and every worker terminated cleanly.
[[nodiscard]] int run_pool(const Schedule &schedule, SutApplyFn sut_apply,
                            WorkerInitFn worker_init, void *sut_ctx,
                            const PoolParams &params, History &history,
                            PoolStats *out_stats);

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_WORKER_POOL_H
