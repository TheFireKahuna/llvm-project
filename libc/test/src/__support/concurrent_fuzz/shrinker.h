//===--- concurrent_fuzz shrinker ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Operational delta-debugging shrinker. Inputs:
//
//   * A schedule that, when run, produces a non-linearizable history.
//   * A `Reproduce` callback (test-defined) that runs a candidate
//     schedule under the SUT and returns true iff the bug reproduces.
//
// Bug reproduction is non-deterministic — concurrent execution. The
// shrinker treats `Reproduce` probabilistically: it runs `K`
// independent trials per candidate; a single positive trial is enough
// to accept the shrink. K trades off shrinker runtime against shrink
// quality.
//
// Shrink moves (in order; cheapest first):
//
//   1. Drop-each-op: replace each op in turn with `kOpKindNoop`. Keep
//      the drop if the bug survives. Linear pass.
//   2. Halve-window: rebase ops to the lower half of the VA window if
//      that half still hosts a reproduction. Halves arg0 / arg1 / arg2
//      VA fields under SUT-supplied transformer.
//   3. Reduce-thread-count: re-partition the schedule onto N-1, N-2,
//      ... threads while preserving op order; accept the smallest
//      that still reproduces.
//   4. Reduce-value-cardinality: collapse all distinct value_ids onto
//      ≤ 2 (under SUT-supplied transformer).
//
// The output is a serialised "minimal seed" — `(seed, GenParams,
// shrink_mask, shrink_thread_count)` — that the regression test can
// re-instantiate and replay.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_SHRINKER_H
#define LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_SHRINKER_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "test/src/__support/concurrent_fuzz/op.h"
#include "test/src/__support/concurrent_fuzz/schedule.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

// Test-supplied bug reproducer. The shrinker hands a derived schedule
// in `s` (its `ops` array may contain `kOpKindNoop` ops to skip);
// caller runs the schedule and returns true iff the linearizability
// checker still reports a failure.
typedef bool (*ReproduceFn)(const Schedule &s, void *user_ctx);

// SUT-defined helper: rebase op `op` to a smaller VA window centered
// at `new_va_base` with `new_va_window`. The factory is responsible
// for keeping the op valid under the SUT's contract (alignment,
// non-empty range, etc.). Called by the halve-window shrink move.
typedef void (*RebaseOpFn)(Op &op, uint64_t new_va_base,
                            uint64_t new_va_window, void *user_ctx);

// SUT-defined helper: collapse the value_id space referenced by `op`
// to be in `[0, max_value)`. Called by the reduce-value-cardinality
// shrink move.
typedef void (*CollapseValueFn)(Op &op, uint16_t max_value, void *user_ctx);

struct ShrinkParams {
  uint32_t reproduce_trials_per_candidate{4};
  uint32_t max_shrink_passes{4};

  // Disable specific shrink moves for SUTs that don't support them
  // (e.g. one that doesn't have a usable rebase transform).
  bool enable_drop_op{true};
  bool enable_halve_window{true};
  bool enable_reduce_threads{true};
  bool enable_reduce_value_cardinality{true};
};

struct ShrinkResult {
  uint32_t orig_op_count;
  uint32_t shrunk_op_count;     // ops that aren't kOpKindNoop after shrink
  uint16_t shrunk_thread_count;
  uint16_t shrunk_value_card;
  uint64_t shrunk_va_window;
  uint32_t passes_run;
};

// Shrink the schedule in place. The caller's `store` is mutated.
// `out_view` is rewritten to reflect the shrunk schedule.
// `reproduce` is invoked many times against derived schedules.
//
// Returns true if the schedule still reproduces the bug after
// shrinking (i.e. the input was actually a positive). Returns false
// if the input itself failed to reproduce — caller has nothing useful.
[[nodiscard]] bool shrink_schedule(ScheduleStore &store, Schedule &out_view,
                                    ReproduceFn reproduce,
                                    RebaseOpFn rebase, CollapseValueFn collapse,
                                    void *user_ctx,
                                    const ShrinkParams &params,
                                    ShrinkResult &out_result);

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_SHRINKER_H
