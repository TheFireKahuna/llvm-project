//===--- concurrent_fuzz linearizability checker impl ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "test/src/__support/concurrent_fuzz/linearizability.h"

#include "src/__support/macros/optimization.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

void MemoScratch::clear(uint32_t log2) {
  uint32_t n = 1u << log2;
  for (uint32_t i = 0; i < n; ++i) {
    entries[i].key0 = 0;
    entries[i].key1 = 0;
  }
}

bool MemoScratch::insert_or_test(uint32_t log2, uint64_t key0, uint64_t key1,
                                  bool &out_overflow) {
  out_overflow = false;
  uint32_t mask = (1u << log2) - 1;
  uint32_t h = static_cast<uint32_t>((key0 ^ (key1 * 0x9E3779B97F4A7C15ULL)) &
                                       mask);
  uint32_t probes = 0;
  for (;;) {
    Entry &e = entries[h];
    if (e.key0 == 0 && e.key1 == 0) {
      e.key0 = key0;
      e.key1 = key1;
      return false;
    }
    if (e.key0 == key0 && e.key1 == key1)
      return true;
    h = (h + 1) & mask;
    if (++probes > (mask + 1) / 2) {
      out_overflow = true;
      return false;
    }
  }
}

namespace {

// Identify quiescent points: indices i in `flat` such that for every
// j < i, flat[j].res_tsc <= flat[i].inv_tsc. The earliest quiescent
// points carve the history into linearizable-independent segments.
//
// Algorithm: maintain `max_res_so_far`. As we advance, an index `i` is
// quiescent iff `flat[i].inv_tsc >= max_res_so_far`. The boundary
// before `i` ends the prior segment.
//
// Returns segment count via `out_segment_count` and writes `[start,
// end)` pairs into `out_starts` (capacity `cap`). The final segment
// covers `[last_start, flat_count)`.
uint32_t find_segments(const HistoryEntry *flat, uint32_t flat_count,
                       uint32_t *out_starts, uint32_t cap) {
  if (flat_count == 0)
    return 0;
  uint32_t count = 0;
  out_starts[count++] = 0;
  uint64_t max_res = flat[0].res_tsc;
  for (uint32_t i = 1; i < flat_count; ++i) {
    if (flat[i].inv_tsc >= max_res) {
      // Boundary before i — i starts a new segment.
      if (count < cap)
        out_starts[count] = i;
      ++count;
    }
    if (flat[i].res_tsc > max_res)
      max_res = flat[i].res_tsc;
  }
  return count;
}

// DFS state for one segment. Entries [seg_start, seg_end).
struct SegState {
  const HistoryEntry *flat;
  uint32_t seg_start;
  uint32_t seg_end;
  uint64_t committed_mask;
  uint32_t committed_count;
  // For each op index local to the segment, precomputed list of
  // segment-local indices that must be committed before it can be
  // linearized (i.e. those whose res_tsc < flat[i].inv_tsc).
  uint64_t deps_mask[64]; // per-op dependency bitmask
};

// A candidate op `i` is enabled iff:
//   * !committed[i]
//   * deps_mask[i] is a subset of committed_mask.
LIBC_INLINE bool is_enabled(const SegState &s, uint32_t local_i) {
  uint64_t bit = 1ull << local_i;
  if ((s.committed_mask & bit) != 0)
    return false;
  return (s.deps_mask[local_i] & ~s.committed_mask) == 0;
}

// Build the dep table for a segment.
void build_deps(SegState &s) {
  uint32_t n = s.seg_end - s.seg_start;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t dep = 0;
    uint64_t inv_i = s.flat[s.seg_start + i].inv_tsc;
    for (uint32_t j = 0; j < n; ++j) {
      if (i == j)
        continue;
      // j must precede i iff res_tsc[j] < inv_tsc[i].
      if (s.flat[s.seg_start + j].res_tsc < inv_i)
        dep |= (1ull << j);
    }
    s.deps_mask[i] = dep;
  }
}

// Recursive DFS. Returns true if a linearization is found from this
// state. `state_buf` is scratch of size `state_size * (n + 1)` (one
// snapshot per stack frame).
bool dfs(SegState &s, Oracle &oracle, MemoScratch &memo, uint32_t memo_log2,
         unsigned char *state_buf, uint32_t depth, uint32_t n,
         FailureReason &out_reason) {
  if (s.committed_count == n)
    return true;

  // Memoize on (committed_mask, oracle_state_hash). Skip slots where
  // both keys are 0 — see MemoScratch contract.
  uint64_t state_h = oracle.state_hash(oracle.ctx);
  uint64_t key0 = s.committed_mask | (state_h << 0); // mix at low bits
  uint64_t key1 = state_h ^ (s.committed_mask * 0xBF58476D1CE4E5B9ULL);
  if (key0 == 0 && key1 == 0)
    key1 = 1; // sentinel-collision avoidance
  bool overflow = false;
  if (memo.insert_or_test(memo_log2, key0, key1, overflow)) {
    return false; // pruned subtree
  }
  if (overflow) {
    out_reason = FailureReason::MemoOverflow;
    // Soft-positive: fall through and continue the search; we just
    // lose memoization for descendants of this node.
  }

  // Save state for restore on backtrack.
  unsigned char *snap = state_buf + depth * oracle.state_size;
  oracle.save_state(oracle.ctx, snap);

  for (uint32_t local_i = 0; local_i < n; ++local_i) {
    if (!is_enabled(s, local_i))
      continue;

    const HistoryEntry &e = s.flat[s.seg_start + local_i];
    OpResult got = oracle.apply(oracle.ctx, e.op);
    if (got.eq(e.result)) {
      uint64_t bit = 1ull << local_i;
      s.committed_mask |= bit;
      ++s.committed_count;

      if (dfs(s, oracle, memo, memo_log2, state_buf, depth + 1, n,
              out_reason)) {
        return true;
      }

      s.committed_mask &= ~bit;
      --s.committed_count;
    }
    oracle.restore_state(oracle.ctx, snap);
  }
  return false;
}

} // namespace

Verdict check_linearizability(const HistoryEntry *flat, uint32_t flat_count,
                              Oracle &oracle, const CheckParams &params,
                              MemoScratch &scratch) {
  Verdict v{};
  v.linearizable = true;
  v.failure_reason = FailureReason::None;
  v.failing_segment_first_op = -1;
  v.failing_segment_op_count = 0;
  v.total_segments = 0;
  v.max_segment_op_count = 0;

  if (flat_count == 0)
    return v;

  // Stack-allocated segment-start array. 4 KiB worst case (1024
  // segments) — fine.
  constexpr uint32_t kSegCap = 1024;
  uint32_t starts[kSegCap];
  uint32_t seg_count = find_segments(flat, flat_count, starts, kSegCap);
  if (seg_count > kSegCap) {
    v.linearizable = false;
    v.failure_reason = FailureReason::SegmentTooBig;
    v.failing_segment_first_op = 0;
    return v;
  }
  v.total_segments = seg_count;

  // Reset oracle once at top.
  oracle.reset(oracle.ctx);

  // Per-segment scratch for state snapshots: depth = seg_size, each
  // frame keeps one snapshot. Allocate once at the cap.
  // 64 ops × state_size bytes is the worst case; with state_size
  // bounded at the SUT level by interval count, this is small (a few
  // KiB).
  // We use a stack-allocated buffer sized at the cap — bounded by
  // segment_op_cap × oracle.state_size. State_size is asserted ≤ 4
  // KiB for safety; the SUT contract is to stay well below that.
  constexpr size_t kStateBytesCap = 64u * 4096u;
  unsigned char state_buf[kStateBytesCap];

  uint32_t state_size = static_cast<uint32_t>(oracle.state_size);
  if (state_size == 0 || state_size > 4096) {
    v.linearizable = false;
    v.failure_reason = FailureReason::OracleDivergence;
    return v;
  }

  uint32_t memo_log2 = params.memo_size_log2;
  if (memo_log2 > MemoScratch::kMaxLog2)
    memo_log2 = MemoScratch::kMaxLog2;

  for (uint32_t seg = 0; seg < seg_count; ++seg) {
    uint32_t seg_start = starts[seg];
    uint32_t seg_end = (seg + 1 < seg_count) ? starts[seg + 1] : flat_count;
    uint32_t n = seg_end - seg_start;

    if (n > v.max_segment_op_count)
      v.max_segment_op_count = n;

    if (n > params.segment_op_cap || n > 64) {
      v.linearizable = false;
      v.failure_reason = FailureReason::SegmentTooBig;
      v.failing_segment_first_op = static_cast<int32_t>(seg_start);
      v.failing_segment_op_count = n;
      return v;
    }

    // Sanity: state_size × n must fit in our scratch.
    if (static_cast<size_t>(state_size) * (n + 1) > kStateBytesCap) {
      v.linearizable = false;
      v.failure_reason = FailureReason::OracleDivergence;
      v.failing_segment_first_op = static_cast<int32_t>(seg_start);
      v.failing_segment_op_count = n;
      return v;
    }

    // Per-segment fresh memo. Quiescent boundary means inter-segment
    // memo entries can never apply.
    scratch.clear(memo_log2);

    SegState s{};
    s.flat = flat;
    s.seg_start = seg_start;
    s.seg_end = seg_end;
    s.committed_mask = 0;
    s.committed_count = 0;
    build_deps(s);

    FailureReason inner_reason = FailureReason::None;
    bool ok = dfs(s, oracle, scratch, memo_log2, state_buf, 0, n,
                  inner_reason);
    if (!ok) {
      v.linearizable = false;
      v.failure_reason = inner_reason == FailureReason::None
                             ? FailureReason::Mismatch
                             : inner_reason;
      v.failing_segment_first_op = static_cast<int32_t>(seg_start);
      v.failing_segment_op_count = n;
      return v;
    }
    // Successful segment leaves the oracle in the post-segment state;
    // the next segment's DFS picks up from there. (No reset between
    // segments — quiescent boundaries fix the inter-segment state.)
  }
  return v;
}

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL
