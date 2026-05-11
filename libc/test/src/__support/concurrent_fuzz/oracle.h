//===--- concurrent_fuzz oracle interface --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Type-erased SUT oracle interface. The linearizability checker calls
// these function pointers; SUT-specific oracle code (e.g. the
// va_tracker interval-map oracle) supplies them.
//
// State management contract:
//
//   * `apply(ctx, op)` mutates the oracle state and returns the
//     abstract-spec result for `op`.
//   * `state_hash(ctx)` returns a 64-bit hash whose value depends only
//     on the oracle's *observable* state (not allocation pointers,
//     internal cursors, etc.). The checker uses this for memoization
//     keys; collisions degrade memoization to a no-op but never affect
//     correctness.
//   * `save_state(ctx, dst)` writes `state_size` bytes capturing
//     the state. `restore_state(ctx, src)` overwrites the state from
//     a buffer previously written by `save_state`. The buffer format
//     is opaque to the framework; the SUT chooses its layout.
//   * `reset(ctx)` returns the oracle to its empty/initial state.
//
// State_size is a per-oracle compile-time constant. The checker
// allocates `O(history_length) * state_size` of scratch in the worst
// case (one snapshot per DFS frame); the oracle author should keep
// state_size bounded by the maximum live interval count × interval
// size, not the full history.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_ORACLE_H
#define LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_ORACLE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "test/src/__support/concurrent_fuzz/op.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

struct Oracle {
  void *ctx;
  size_t state_size;
  OpResult (*apply)(void *ctx, const Op &op);
  uint64_t (*state_hash)(void *ctx);
  void (*save_state)(void *ctx, void *dst);
  void (*restore_state)(void *ctx, const void *src);
  void (*reset)(void *ctx);
};

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_ORACLE_H
