//===--- concurrent_fuzz Op / OpResult ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Generic op-encoding shared by every layer of the framework. Each SUT
// adapter (e.g. va_tracker, fdtable, mapping_table) defines its own op
// kinds and the meaning of the four `arg*` fields; the framework treats
// the values as opaque.
//
// Why a fixed-shape POD instead of a tagged-union template:
//
//   * Schedule arrays must be flat and trivially serialisable for the
//     shrinker's seed-replay path.
//   * History entries are merged across worker threads without copying
//     SUT-specific payloads.
//   * The linearizability checker compares OpResult structures by value;
//     a fixed shape lets it do that without RTTI.
//
// Sixteen bytes of opaque arg space + a 4-bit kind discriminator covers
// every va_tracker and fdtable op imagined to date. A consumer that
// truly needs more (e.g. an oracle for an interval map keyed by 256-bit
// VA) can encode via interning into a side table.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_OP_H
#define LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_OP_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

// SUT-defined op kind. The framework only requires `0` to be a valid
// "no-op" sentinel that the SUT and Oracle treat identically (used by
// the shrinker's drop-each-op pass).
struct Op {
  uint16_t kind;
  uint16_t worker_id;
  uint32_t op_index; // global index in the schedule
  uint64_t arg0;
  uint64_t arg1;
  uint64_t arg2;
  uint64_t arg3;
};

// SUT-defined result. Status 0 == success; any non-zero is an SUT-level
// errno (positive). The framework only compares (status, payload) pairs
// for equality. Layout chosen so brace-list initializers
// `OpResult{status, payload}` populate the right fields — the natural
// 4-byte tail pad after `status` is implicit.
struct OpResult {
  uint32_t status;
  uint64_t payload;

  [[nodiscard]] LIBC_INLINE bool eq(const OpResult &o) const {
    return status == o.status && payload == o.payload;
  }
};

// Special op kind reserved by the framework. Schedule generators MUST
// NOT emit it; the shrinker emits it to express "drop this op." The
// SUT and Oracle adapters MUST translate it to a no-op that returns
// `OpResult{0, 0}`.
inline constexpr uint16_t kOpKindNoop = 0;

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_OP_H
