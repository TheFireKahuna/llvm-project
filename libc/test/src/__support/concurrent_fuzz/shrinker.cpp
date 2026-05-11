//===--- concurrent_fuzz shrinker impl -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "test/src/__support/concurrent_fuzz/shrinker.h"

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

namespace {

bool reproduce_with_trials(const Schedule &s, ReproduceFn reproduce,
                           void *user_ctx, uint32_t trials) {
  for (uint32_t t = 0; t < trials; ++t) {
    if (reproduce(s, user_ctx))
      return true;
  }
  return false;
}

// Repartition `op_count` ops across `target_threads` workers,
// preserving op_index order. Updates `out_view.slices` and
// `op.worker_id` per op.
void repartition(Schedule &out_view, ScheduleStore &store,
                 uint16_t target_threads) {
  uint32_t per = out_view.op_count / target_threads;
  uint32_t rem = out_view.op_count - per * target_threads;
  uint32_t pos = 0;
  for (uint16_t w = 0; w < target_threads; ++w) {
    uint32_t cnt = per + (w < rem ? 1u : 0u);
    store.slices_storage[w].start = pos;
    store.slices_storage[w].count = cnt;
    for (uint32_t i = 0; i < cnt; ++i)
      store.ops_storage[pos + i].worker_id = w;
    pos += cnt;
  }
  // Zero the trailing slice slots (so any old leftover entries can't
  // be accidentally read).
  for (uint16_t w = target_threads; w < ScheduleStore::kMaxThreads; ++w)
    store.slices_storage[w] = WorkerSlice{0, 0};
  out_view.thread_count = target_threads;
}

bool drop_op_pass(ScheduleStore &store, Schedule &view, ReproduceFn reproduce,
                  void *user_ctx, uint32_t trials) {
  bool any_dropped = false;
  for (uint32_t i = 0; i < view.op_count; ++i) {
    if (store.ops_storage[i].kind == kOpKindNoop)
      continue;
    Op saved = store.ops_storage[i];
    store.ops_storage[i].kind = kOpKindNoop;
    store.ops_storage[i].arg0 = 0;
    store.ops_storage[i].arg1 = 0;
    store.ops_storage[i].arg2 = 0;
    store.ops_storage[i].arg3 = 0;
    if (reproduce_with_trials(view, reproduce, user_ctx, trials)) {
      any_dropped = true;
    } else {
      store.ops_storage[i] = saved;
    }
  }
  return any_dropped;
}

bool halve_window_pass(ScheduleStore &store, Schedule &view,
                        ReproduceFn reproduce, RebaseOpFn rebase,
                        void *user_ctx, uint32_t trials) {
  if (rebase == nullptr)
    return false;
  uint64_t orig_window = view.params.addr.va_window_bytes;
  uint64_t orig_base = view.params.addr.va_base;
  if (orig_window <= 1)
    return false;
  uint64_t new_window = orig_window / 2;
  if (new_window == 0)
    return false;

  // Backup ops.
  Op backup[ScheduleStore::kMaxOps];
  for (uint32_t i = 0; i < view.op_count; ++i)
    backup[i] = store.ops_storage[i];

  // Try lower half.
  view.params.addr.va_window_bytes = new_window;
  for (uint32_t i = 0; i < view.op_count; ++i)
    rebase(store.ops_storage[i], orig_base, new_window, user_ctx);
  if (reproduce_with_trials(view, reproduce, user_ctx, trials)) {
    return true;
  }
  // Try upper half.
  view.params.addr.va_base = orig_base + new_window;
  for (uint32_t i = 0; i < view.op_count; ++i)
    store.ops_storage[i] = backup[i];
  for (uint32_t i = 0; i < view.op_count; ++i)
    rebase(store.ops_storage[i], view.params.addr.va_base, new_window,
            user_ctx);
  if (reproduce_with_trials(view, reproduce, user_ctx, trials)) {
    return true;
  }
  // Restore.
  view.params.addr.va_base = orig_base;
  view.params.addr.va_window_bytes = orig_window;
  for (uint32_t i = 0; i < view.op_count; ++i)
    store.ops_storage[i] = backup[i];
  return false;
}

bool reduce_threads_pass(ScheduleStore &store, Schedule &view,
                          ReproduceFn reproduce, void *user_ctx,
                          uint32_t trials) {
  bool reduced = false;
  while (view.thread_count > 1) {
    uint16_t orig = view.thread_count;
    repartition(view, store, static_cast<uint16_t>(orig - 1));
    if (reproduce_with_trials(view, reproduce, user_ctx, trials)) {
      reduced = true;
    } else {
      repartition(view, store, orig);
      break;
    }
  }
  return reduced;
}

bool reduce_value_cardinality_pass(ScheduleStore &store, Schedule &view,
                                    ReproduceFn reproduce,
                                    CollapseValueFn collapse, void *user_ctx,
                                    uint32_t trials) {
  if (collapse == nullptr)
    return false;
  if (view.params.value_cardinality <= 2)
    return false;
  uint16_t orig = view.params.value_cardinality;

  Op backup[ScheduleStore::kMaxOps];
  for (uint32_t i = 0; i < view.op_count; ++i)
    backup[i] = store.ops_storage[i];

  for (uint16_t target = 2; target < orig; target *= 2) {
    for (uint32_t i = 0; i < view.op_count; ++i)
      collapse(store.ops_storage[i], target, user_ctx);
    view.params.value_cardinality = target;
    if (reproduce_with_trials(view, reproduce, user_ctx, trials))
      return true;
    // Restore for next attempt.
    for (uint32_t i = 0; i < view.op_count; ++i)
      store.ops_storage[i] = backup[i];
    view.params.value_cardinality = orig;
  }
  return false;
}

} // namespace

bool shrink_schedule(ScheduleStore &store, Schedule &out_view,
                     ReproduceFn reproduce, RebaseOpFn rebase,
                     CollapseValueFn collapse, void *user_ctx,
                     const ShrinkParams &params, ShrinkResult &out_result) {
  out_result.orig_op_count = out_view.op_count;
  out_result.passes_run = 0;

  // Confirm input reproduces.
  if (!reproduce_with_trials(out_view, reproduce, user_ctx,
                              params.reproduce_trials_per_candidate)) {
    out_result.shrunk_op_count = out_view.op_count;
    out_result.shrunk_thread_count = out_view.thread_count;
    out_result.shrunk_value_card = out_view.params.value_cardinality;
    out_result.shrunk_va_window = out_view.params.addr.va_window_bytes;
    return false;
  }

  for (uint32_t pass = 0; pass < params.max_shrink_passes; ++pass) {
    bool any_progress = false;
    if (params.enable_drop_op) {
      if (drop_op_pass(store, out_view, reproduce, user_ctx,
                       params.reproduce_trials_per_candidate))
        any_progress = true;
    }
    if (params.enable_halve_window) {
      if (halve_window_pass(store, out_view, reproduce, rebase, user_ctx,
                             params.reproduce_trials_per_candidate))
        any_progress = true;
    }
    if (params.enable_reduce_threads) {
      if (reduce_threads_pass(store, out_view, reproduce, user_ctx,
                                params.reproduce_trials_per_candidate))
        any_progress = true;
    }
    if (params.enable_reduce_value_cardinality) {
      if (reduce_value_cardinality_pass(store, out_view, reproduce, collapse,
                                          user_ctx,
                                          params.reproduce_trials_per_candidate))
        any_progress = true;
    }
    ++out_result.passes_run;
    if (!any_progress)
      break;
  }

  // Count surviving non-noop ops.
  uint32_t surviving = 0;
  for (uint32_t i = 0; i < out_view.op_count; ++i)
    if (store.ops_storage[i].kind != kOpKindNoop)
      ++surviving;
  out_result.shrunk_op_count = surviving;
  out_result.shrunk_thread_count = out_view.thread_count;
  out_result.shrunk_value_card = out_view.params.value_cardinality;
  out_result.shrunk_va_window = out_view.params.addr.va_window_bytes;
  return true;
}

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL
