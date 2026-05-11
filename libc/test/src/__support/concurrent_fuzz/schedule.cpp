//===--- concurrent_fuzz schedule generator impl -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "test/src/__support/concurrent_fuzz/schedule.h"

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

uint16_t pick_op_kind(SplitMix64 &r, const OpKindWeights &w) {
  uint32_t total = 0;
  for (uint32_t i = 0; i < w.kind_count && i < kMaxOpKinds; ++i)
    total += w.weights[i];
  if (total == 0)
    return 0; // degenerate; falls through to no-op
  uint32_t pick = r.next_below(total);
  uint32_t acc = 0;
  for (uint32_t i = 0; i < w.kind_count && i < kMaxOpKinds; ++i) {
    acc += w.weights[i];
    if (pick < acc)
      return static_cast<uint16_t>(i);
  }
  return static_cast<uint16_t>(w.kind_count - 1);
}

uint64_t sample_address(SplitMix64 &r, const AddressBias &bias) {
  uint64_t base;
  if (bias.hotspot_weight_256 > 0 && bias.hotspot_bytes > 0 &&
      r.next_below(256) < bias.hotspot_weight_256) {
    uint64_t off =
        r.next_u64() % (bias.hotspot_bytes == 0 ? 1 : bias.hotspot_bytes);
    base = bias.va_base + bias.hotspot_offset + off;
  } else {
    uint64_t window = bias.va_window_bytes == 0 ? 1 : bias.va_window_bytes;
    uint64_t off = r.next_u64() % window;
    base = bias.va_base + off;
  }

  // Tower-jump bias: occasionally shift the address up to a higher
  // alignment so that `ctz(addr / kAllocGran)` is biased upward.
  if (bias.tower_jump_geometric_bits > 0) {
    uint32_t shift = r.next_geometric(bias.tower_jump_geometric_bits);
    if (shift > 16)
      shift = 16;
    if (shift > 0) {
      uint64_t mask = ~((static_cast<uint64_t>(1) << (16 + shift)) - 1);
      base &= mask;
      // Re-bias into window if the mask pushed us out.
      if (base < bias.va_base)
        base = bias.va_base;
      uint64_t hi =
          bias.va_base +
          (bias.va_window_bytes ? bias.va_window_bytes - 1 : 0);
      if (base > hi)
        base = hi;
    }
  }
  return base;
}

bool ScheduleStore::populate(uint64_t seed, const GenParams &params,
                             Schedule &out_view) {
  if (params.op_count > kMaxOps)
    return false;
  if (params.thread_count == 0 || params.thread_count > kMaxThreads)
    return false;

  out_view.seed = seed;
  out_view.params = params;
  out_view.ops = ops_storage;
  out_view.op_count = params.op_count;
  out_view.slices = slices_storage;
  out_view.thread_count = params.thread_count;

  // Even-split partition: worker `w` owns ops [w*per_worker, ...).
  uint32_t per = params.op_count / params.thread_count;
  uint32_t rem = params.op_count - per * params.thread_count;
  uint32_t pos = 0;
  for (uint16_t w = 0; w < params.thread_count; ++w) {
    uint32_t cnt = per + (w < rem ? 1u : 0u);
    slices_storage[w].start = pos;
    slices_storage[w].count = cnt;
    pos += cnt;
  }
  return true;
}

bool generate(uint64_t seed, const GenParams &params, OpFactoryFn factory,
              void *factory_ctx, ScheduleStore &store, Schedule &out_view) {
  if (factory == nullptr)
    return false;
  if (!store.populate(seed, params, out_view))
    return false;

  // Generate ops sequentially in op_index order; each op pulls from a
  // child PRNG keyed by (seed, op_index). This makes the shrinker's
  // "drop-this-op" pass independent of every other op's args, so an
  // op's arg pattern is invariant under shrinks.
  for (uint32_t i = 0; i < params.op_count; ++i) {
    SplitMix64 sub = derive(seed ^ 0xC0FFEEU, static_cast<uint64_t>(i));
    Op &op = store.ops_storage[i];
    op.kind = pick_op_kind(sub, params.weights);
    if (op.kind == kOpKindNoop && params.weights.kind_count > 1)
      op.kind = 1; // never emit explicit no-ops; shrinker owns those
    op.op_index = i;

    // Worker assignment: lookup which slice contains `i`. Linear scan
    // is fine — `thread_count` is bounded by `kMaxThreads`.
    uint16_t worker = 0;
    for (uint16_t w = 0; w < params.thread_count; ++w) {
      const auto &s = store.slices_storage[w];
      if (i >= s.start && i < s.start + s.count) {
        worker = w;
        break;
      }
    }
    op.worker_id = worker;
    op.arg0 = op.arg1 = op.arg2 = op.arg3 = 0;
    factory(op.kind, sub, params, op, factory_ctx);
  }
  return true;
}

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL
