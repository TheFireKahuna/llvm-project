//===--- concurrent_fuzz linearizability checker ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Wing-Gibbons (1993) linearizability check with Lowe-style (2017)
// P-compositionality segmentation. Given:
//
//   * A concurrent history H = ⟨entries sorted by inv_tsc⟩.
//   * A sequential reference (`Oracle`) implementing the abstract spec.
//
// We try to construct a linearization L: a topological sort of H
// respecting real-time order (op A whose `res_tsc` precedes op B's
// `inv_tsc` must come before B in L) such that applying L to the oracle
// reproduces every op's observed return value. If such an L exists, H
// is linearizable.
//
// Algorithmic structure:
//
//   1. Segment H at quiescent points (no in-flight ops). Each segment
//      is independently linearizable iff H is. Per Lowe 2017 this
//      is the lever that makes the search tractable: each segment has
//      bounded concurrency (≤ thread_count in-flight ops at any
//      moment), so the bitmask-of-committed-ops fits in u64.
//
//   2. Within each segment, DFS over candidate "next op to linearize."
//      A candidate is an uncommitted op whose `inv_tsc` is ≤ the
//      response time of any committed op AND whose every preceding-by-
//      real-time op is committed. Apply to the oracle, compare with
//      the recorded result; on mismatch, backtrack.
//
//   3. Memoize on `(committed_mask, oracle_state_hash)`. Identical
//      states reached by different paths are pruned.
//
// Returns a `Verdict` carrying linearizability status. On a failure,
// `Verdict::failure_reason` names the failure mode (mismatch /
// segment-too-big / memoization-overflow / oracle-divergence) and
// `failing_segment_first_op` points the caller at the offending op so
// the shrinker can target it.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_LINEARIZABILITY_H
#define LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_LINEARIZABILITY_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "test/src/__support/concurrent_fuzz/history.h"
#include "test/src/__support/concurrent_fuzz/oracle.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

enum class FailureReason : uint8_t {
  None = 0,
  Mismatch,            // No linearization explains the observed returns
  SegmentTooBig,       // > 64 ops between two quiescent points
  MemoOverflow,        // Hash table full; checker bailed soft-positive
  OracleDivergence,    // Oracle apply returned an impossible result
};

struct Verdict {
  bool linearizable;
  FailureReason failure_reason;
  // Index into History::flat of the first op in the failing segment.
  // -1 if no segment failed.
  int32_t failing_segment_first_op;
  uint32_t failing_segment_op_count;
  uint32_t total_segments;
  uint32_t max_segment_op_count; // peak segment size — coverage signal
};

struct CheckParams {
  // Segment size cap. A segment exceeding this aborts with
  // `SegmentTooBig`. 64 is chosen because the committed_mask is a u64;
  // the SUT must produce schedules whose concurrency-window stays
  // bounded for the checker to terminate. Realistic peak: workers ×
  // ~2 ops (one in-flight + tail of a previous one).
  uint32_t segment_op_cap{64};

  // Memoization table size (power of 2). Larger = more pruning, more
  // scratch memory. 1<<14 entries × 16 B/entry = 256 KiB is comfortable
  // for stack alloc.
  uint32_t memo_size_log2{14};
};

// Memoization scratch — caller-supplied so the checker is allocation-
// free at runtime.
struct MemoScratch {
  static constexpr uint32_t kMaxLog2 = 16;
  // Open-addressed hash table, fixed-capacity. Empty slot = key0 == 0
  // && key1 == 0; the checker keys on a 128-bit hash (state, mask), so
  // an all-zero key is impossible by construction.
  struct Entry {
    uint64_t key0;
    uint64_t key1;
  };
  Entry entries[1u << kMaxLog2];

  void clear(uint32_t log2);
  // Returns true iff the key was already present (i.e. this is a
  // pruned subtree). If absent, inserts and returns false.
  [[nodiscard]] bool insert_or_test(uint32_t log2, uint64_t key0,
                                     uint64_t key1, bool &out_overflow);
};

// Run the checker. `flat` and `flat_count` are History::flat output.
// `oracle` is the SUT oracle; the checker calls `oracle.reset()` once
// at the top and at every quiescent boundary.
[[nodiscard]] Verdict check_linearizability(const HistoryEntry *flat,
                                             uint32_t flat_count,
                                             Oracle &oracle,
                                             const CheckParams &params,
                                             MemoScratch &scratch);

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_LINEARIZABILITY_H
