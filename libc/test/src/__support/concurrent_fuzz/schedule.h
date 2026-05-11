//===--- concurrent_fuzz schedule generator ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `Schedule` is the canonical input to a fuzzing run: a flat array of
// `Op`s plus the partition that names which worker executes which slice.
// A schedule reproduces deterministically from `(seed, GenParams)`; the
// shrinker stores its minimal failing case as a re-runnable Schedule.
//
// `GenParams` carries every adversarial-coverage knob the framework
// exposes today. Knobs are biases, not constraints: the SUT-supplied
// `OpFactory` is still responsible for realising each op's args under
// the SUT's contract (alignment, validity, etc.). The framework never
// emits an op whose call would be UB at the language level.
//
// Reusable across SUTs by virtue of the `OpFactory` callback: the
// framework picks an op-kind index (with optional weighting) and a
// uniform `SplitMix64` substream; the SUT factory translates that into
// a concrete Op. No SUT-specific knowledge in the framework.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_SCHEDULE_H
#define LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_SCHEDULE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "test/src/__support/concurrent_fuzz/op.h"
#include "test/src/__support/concurrent_fuzz/random.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

// The framework's op-kind discriminator is u16. SUTs that use more than
// 16 distinct op kinds pack additional kinds via arg0; the schedule
// generator weight table is indexed up to this maximum.
inline constexpr uint32_t kMaxOpKinds = 16;

// Per-SUT op-kind weight table. weights[i] = relative weight for op
// kind i; sum is computed by the generator. Slot 0 (kOpKindNoop) is
// usually weight 0 — schedules don't emit explicit no-ops.
struct OpKindWeights {
  uint32_t weights[kMaxOpKinds];
  uint32_t kind_count;
};

// Address-window bias parameters. The generator produces an unaligned
// random offset within `[0, va_window_bytes)`; the SUT factory is
// responsible for any required alignment.
struct AddressBias {
  uint64_t va_base;
  uint64_t va_window_bytes;

  // Hot-spot bias: this fraction (0..256) of generated addresses fall
  // inside `hotspot_bytes` starting at `va_base + hotspot_offset`. The
  // remainder spread across the full window. Tunes the contention
  // shape on the SUT.
  uint32_t hotspot_weight_256;
  uint64_t hotspot_offset;
  uint64_t hotspot_bytes;

  // Tower-jump bias: per-address geometric draw for `ctz(va)` shift,
  // pushing more addresses toward natural skiplist tower boundaries.
  // 0 = uniform; ~6 reproduces the geometric distribution the skiplist
  // uses for tower height. Sat-clamped at 16.
  uint8_t tower_jump_geometric_bits;
  uint8_t pad_[7]{};
};

// The whole knob set. Defaults reproduce a sensible mid-contention
// schedule on an interval-map-shaped SUT.
struct GenParams {
  // Total ops across the whole schedule. Workers split this evenly.
  uint32_t op_count{2048};

  // Concurrent worker count.
  uint16_t thread_count{4};

  // Distinct abstract value identities the SUT must distinguish.
  // Smaller cardinality drives more value-confusion test pressure.
  uint16_t value_cardinality{8};

  uint16_t pad0_{0};
  uint16_t pad1_{0};

  OpKindWeights weights{};
  AddressBias addr{};
};

// Per-worker slice into the schedule's op array.
struct WorkerSlice {
  uint32_t start; // index into Schedule::ops
  uint32_t count;
};

// Flat schedule. Owned by a `ScheduleStore`; this struct is a view.
struct Schedule {
  uint64_t seed;
  GenParams params;
  Op *ops;
  uint32_t op_count;
  WorkerSlice *slices;
  uint16_t thread_count;
  uint16_t pad_{0};
};

// Owning storage for a Schedule. Lives in the test's address space
// (caller-allocated, fixed capacity). Sized to support any GenParams
// the test runner constructs.
struct ScheduleStore {
  static constexpr uint32_t kMaxOps = 16384;
  static constexpr uint16_t kMaxThreads = 32;

  Op ops_storage[kMaxOps];
  WorkerSlice slices_storage[kMaxThreads];

  // Materialise `view` into this store given (seed, params). Returns
  // false on a parameter overflow against the store's caps.
  [[nodiscard]] bool populate(uint64_t seed, const GenParams &params,
                              Schedule &out_view);
};

// SUT-supplied factory. The framework picks the op kind index from the
// weight table and supplies a deterministic SplitMix64 substream
// (`r`). The factory owns translating (kind, r, params) into the four
// arg fields. `op_index` and `worker_id` are filled by the framework
// before the factory sees `out_op`.
typedef void (*OpFactoryFn)(uint16_t op_kind, SplitMix64 &r,
                            const GenParams &params, Op &out_op,
                            void *factory_ctx);

// Generate a schedule into `store`. Returns false on overflow.
[[nodiscard]] bool generate(uint64_t seed, const GenParams &params,
                            OpFactoryFn factory, void *factory_ctx,
                            ScheduleStore &store, Schedule &out_view);

// Helpers: compute a candidate VA inside the address window using the
// hotspot + tower-jump biases. SUT factories call this rather than
// implementing their own address sampling.
[[nodiscard]] uint64_t sample_address(SplitMix64 &r, const AddressBias &bias);

// Pick an op-kind index according to the weight table.
[[nodiscard]] uint16_t pick_op_kind(SplitMix64 &r, const OpKindWeights &w);

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_SCHEDULE_H
