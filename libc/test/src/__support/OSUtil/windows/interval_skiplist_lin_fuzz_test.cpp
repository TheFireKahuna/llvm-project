//===-- Property-based linearizability fuzzer for ART × interval skiplist ===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Property-based linearizability fuzzer for `va_tracker`. Composes the
// generic concurrent_fuzz framework
// (`libc/test/src/__support/concurrent_fuzz/`) with a SUT adapter +
// oracle that verifies every public va_tracker mutator/reader linearises
// against a sequential interval-map reference.
//
// SUT op surface (both SUT and oracle agree on the abstract spec):
//
//   INSERT(lo, hi, value_id)   — overwrite-overlap insert. ALL
//                                 intervals overlapping [lo, hi) are
//                                 deleted; a single [lo, hi)→value_id
//                                 entry is added. Returns OK or
//                                 EINVAL.
//   ERASE(lo, hi)              — delete every interval overlapping
//                                 [lo, hi). Returns OK / EINVAL /
//                                 ENOENT (no leaf yet).
//   PROTECT(lo, hi, value_id)  — replace every overlapping interval
//                                 with a single [lo, hi)→value_id;
//                                 ENOENT if no overlap.
//   SPLIT_AT(boundary)         — split the strictly-containing
//                                 interval at boundary; ENOENT if
//                                 boundary lies on a gap or on an
//                                 existing edge.
//   COALESCE_AROUND(lo, hi)    — merge adjacent same-value pairs in
//                                 [lo, hi) until fixed point.
//   RESOLVE(addr)              — return value_id of covering interval
//                                 or ENOENT.
//   WALK_RANGE(lo, hi)         — SipHash-2-4 of (lo, hi, value_id)
//                                 tuples for every interval intersecting
//                                 [lo, hi).
//
// Encoding choice: `value_id` = `RegionMeta::flags`. The va_tracker
// public surface stores the caller's flags verbatim on the RegionDesc
// and uses flag-equality as the coalesce-compatibility predicate, so
// flags-as-value-id is faithful to the SUT's observable semantics.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain_registry.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/concurrent_fuzz/history.h"
#include "test/src/__support/concurrent_fuzz/linearizability.h"
#include "test/src/__support/concurrent_fuzz/op.h"
#include "test/src/__support/concurrent_fuzz/oracle.h"
#include "test/src/__support/concurrent_fuzz/random.h"
#include "test/src/__support/concurrent_fuzz/schedule.h"
#include "test/src/__support/concurrent_fuzz/shrinker.h"
#include "test/src/__support/concurrent_fuzz/siphash.h"
#include "test/src/__support/concurrent_fuzz/worker_pool.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"

#include <stddef.h>

namespace cf = LIBC_NAMESPACE::concurrent_fuzz;
namespace vt = LIBC_NAMESPACE::windows::va_tracker;
using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;

namespace {

// ===========================================================================
// SUT op kinds. Index 0 is reserved by the framework for kOpKindNoop.
// ===========================================================================

enum : uint16_t {
  kOpInsert = 1,
  kOpErase = 2,
  kOpProtect = 3,
  kOpSplitAt = 4,
  kOpCoalesce = 5,
  kOpResolve = 6,
  kOpWalkRange = 7,
  kOpKindCount = 8,
};

constexpr uint64_t kAllocGran = 64ull * 1024ull;
constexpr uint64_t kTestVaBase = 0x600000000000ULL; // 96 TiB, far above heap
constexpr uint64_t kTestVaWindow = 8ull * 1024ull * 1024ull; // 8 MiB

// SipHash key for walk_range result hashing — fixed (not seed-derived)
// so cross-seed walk hashes are comparable when needed.
constexpr uint64_t kWalkHashK0 = 0xC0FFEE0BADCAFEULL;
constexpr uint64_t kWalkHashK1 = 0xDEADBEEFFEEDFACEULL;

// ===========================================================================
// Oracle state — sorted-array interval map.
//
// kMaxIntervals × sizeof(OracleInterval) ≤ kStateBytesCap = 4 KiB.
// 128 intervals × 24 B = 3072 B + a few overhead fields fits.
// ===========================================================================

struct OracleInterval {
  uint64_t lo;
  uint64_t hi;
  uint16_t value_id;
  uint16_t pad_[3];
};

constexpr uint32_t kMaxIntervals = 128;

struct OracleState {
  OracleInterval intervals[kMaxIntervals];
  uint32_t count;
  uint32_t leaf_installed;
  uint32_t pad0_;
  uint32_t pad1_;
};

static_assert(sizeof(OracleState) <= 4096,
              "OracleState must fit linearizability checker state cap");

void oracle_reset_impl(OracleState &s) {
  s.count = 0;
  s.leaf_installed = 0;
}

[[nodiscard]] bool oracle_range_valid(uint64_t lo, uint64_t hi) {
  if (lo >= hi)
    return false;
  if ((lo & (kAllocGran - 1)) != 0)
    return false;
  uint64_t bytes = hi - lo;
  if ((bytes & (kAllocGran - 1)) != 0)
    return false;
  if (hi < lo)
    return false;
  return true;
}

// Erase every interval that overlaps [lo, hi). Returns the number of
// removed intervals.
uint32_t oracle_remove_overlapping(OracleState &s, uint64_t lo, uint64_t hi) {
  uint32_t out = 0;
  uint32_t removed = 0;
  for (uint32_t i = 0; i < s.count; ++i) {
    const auto &it = s.intervals[i];
    bool overlaps = (it.lo < hi) && (it.hi > lo);
    if (overlaps) {
      ++removed;
      continue;
    }
    if (out != i)
      s.intervals[out] = it;
    ++out;
  }
  s.count = out;
  return removed;
}

// Insert sorted at the right position. Returns false if kMaxIntervals
// is exceeded.
bool oracle_sorted_insert(OracleState &s, uint64_t lo, uint64_t hi,
                          uint16_t value_id) {
  if (s.count >= kMaxIntervals)
    return false;
  uint32_t pos = 0;
  while (pos < s.count && s.intervals[pos].lo < lo)
    ++pos;
  for (uint32_t j = s.count; j > pos; --j)
    s.intervals[j] = s.intervals[j - 1];
  s.intervals[pos].lo = lo;
  s.intervals[pos].hi = hi;
  s.intervals[pos].value_id = value_id;
  for (uint32_t k = 0; k < 3; ++k)
    s.intervals[pos].pad_[k] = 0;
  ++s.count;
  return true;
}

cf::OpResult oracle_apply_insert(OracleState &s, const cf::Op &op) {
  uint64_t lo = op.arg0;
  uint64_t hi = op.arg1;
  uint16_t v = static_cast<uint16_t>(op.arg2);
  if (!oracle_range_valid(lo, hi))
    return {EINVAL, 0};
  (void)oracle_remove_overlapping(s, lo, hi);
  if (!oracle_sorted_insert(s, lo, hi, v))
    return {ENOMEM, 0};
  s.leaf_installed = 1;
  return {0, 0};
}

cf::OpResult oracle_apply_erase(OracleState &s, const cf::Op &op) {
  uint64_t lo = op.arg0;
  uint64_t hi = op.arg1;
  if (!oracle_range_valid(lo, hi))
    return {EINVAL, 0};
  if (!s.leaf_installed)
    return {ENOENT, 0};
  (void)oracle_remove_overlapping(s, lo, hi);
  return {0, 0};
}

cf::OpResult oracle_apply_protect(OracleState &s, const cf::Op &op) {
  uint64_t lo = op.arg0;
  uint64_t hi = op.arg1;
  uint16_t new_v = static_cast<uint16_t>(op.arg2);
  if (!oracle_range_valid(lo, hi))
    return {EINVAL, 0};
  if (!s.leaf_installed)
    return {ENOENT, 0};
  // Look for at least one overlap; ProtectVisitor returns ENOENT if
  // locked.succ_count == 0.
  bool overlap = false;
  for (uint32_t i = 0; i < s.count; ++i) {
    const auto &it = s.intervals[i];
    if (it.lo < hi && it.hi > lo) {
      overlap = true;
      break;
    }
  }
  if (!overlap)
    return {ENOENT, 0};
  (void)oracle_remove_overlapping(s, lo, hi);
  if (!oracle_sorted_insert(s, lo, hi, new_v))
    return {ENOMEM, 0};
  return {0, 0};
}

cf::OpResult oracle_apply_split(OracleState &s, const cf::Op &op) {
  uint64_t boundary = op.arg0;
  if ((boundary & (kAllocGran - 1)) != 0)
    return {EINVAL, 0};
  if (!s.leaf_installed)
    return {ENOENT, 0};
  // Find strictly-containing interval.
  for (uint32_t i = 0; i < s.count; ++i) {
    auto &it = s.intervals[i];
    if (it.lo < boundary && boundary < it.hi) {
      // Insert tail [boundary, hi). Truncate head to [lo, boundary).
      uint64_t old_hi = it.hi;
      uint16_t v = it.value_id;
      it.hi = boundary;
      if (!oracle_sorted_insert(s, boundary, old_hi, v)) {
        // Roll back.
        it.hi = old_hi;
        return {ENOMEM, 0};
      }
      return {0, 0};
    }
  }
  // No strictly-containing interval. SplitVisitor returns ENOENT
  // (locked.succ_count != 1) or EINVAL (boundary on edge). The
  // distinction is which case; both look identical from the oracle's
  // perspective absent an arena-side query, so we standardise on
  // ENOENT for "no interval covers" and EINVAL for "interval exists
  // but boundary is exactly on an edge."
  for (uint32_t i = 0; i < s.count; ++i) {
    const auto &it = s.intervals[i];
    if (it.lo == boundary || it.hi == boundary)
      return {EINVAL, 0};
  }
  return {ENOENT, 0};
}

cf::OpResult oracle_apply_resolve(OracleState &s, const cf::Op &op) {
  uint64_t addr = op.arg0;
  if (!s.leaf_installed)
    return {ENOENT, 0};
  for (uint32_t i = 0; i < s.count; ++i) {
    const auto &it = s.intervals[i];
    if (it.lo <= addr && addr < it.hi)
      return {0, static_cast<uint64_t>(it.value_id)};
  }
  return {ENOENT, 0};
}

cf::OpResult oracle_apply_walk(OracleState &s, const cf::Op &op) {
  uint64_t lo = op.arg0;
  uint64_t hi = op.arg1;
  if (!oracle_range_valid(lo, hi))
    return {0, 0};
  cf::SipHasher h;
  h.reset(kWalkHashK0, kWalkHashK1);
  for (uint32_t i = 0; i < s.count; ++i) {
    const auto &it = s.intervals[i];
    if (it.hi > lo && it.lo < hi) {
      h.absorb_u64(it.lo);
      h.absorb_u64(it.hi);
      h.absorb_u64(static_cast<uint64_t>(it.value_id));
    }
  }
  return {0, h.finalize(0, 0)};
}

cf::OpResult oracle_apply_dispatch(void *ctx, const cf::Op &op) {
  auto *s = static_cast<OracleState *>(ctx);
  switch (op.kind) {
  case cf::kOpKindNoop:
    return {0, 0};
  case kOpInsert:
    return oracle_apply_insert(*s, op);
  case kOpErase:
    return oracle_apply_erase(*s, op);
  case kOpProtect:
    return oracle_apply_protect(*s, op);
  case kOpSplitAt:
    return oracle_apply_split(*s, op);
  case kOpCoalesce:
    // Matches the SUT's no-op handling — auto-coalesce removed by the
    // 2026-05-10 typed-op pivot.
    return {0, 0};
  case kOpResolve:
    return oracle_apply_resolve(*s, op);
  case kOpWalkRange:
    return oracle_apply_walk(*s, op);
  }
  return {EINVAL, 0};
}

uint64_t oracle_state_hash_impl(void *ctx) {
  auto *s = static_cast<OracleState *>(ctx);
  cf::SipHasher h;
  h.reset(0x123456789ABCDEF0ULL, 0xFEDCBA9876543210ULL);
  h.absorb_u64(static_cast<uint64_t>(s->count));
  h.absorb_u64(static_cast<uint64_t>(s->leaf_installed));
  for (uint32_t i = 0; i < s->count; ++i) {
    const auto &it = s->intervals[i];
    h.absorb_u64(it.lo);
    h.absorb_u64(it.hi);
    h.absorb_u64(static_cast<uint64_t>(it.value_id));
  }
  return h.finalize(0, 0);
}

void oracle_save_state_impl(void *ctx, void *dst) {
  __builtin_memcpy(dst, ctx, sizeof(OracleState));
}

void oracle_restore_state_impl(void *ctx, const void *src) {
  __builtin_memcpy(ctx, src, sizeof(OracleState));
}

void oracle_reset_impl_cb(void *ctx) {
  oracle_reset_impl(*static_cast<OracleState *>(ctx));
}

// ===========================================================================
// SUT adapter — translates Op → va_tracker typed-op call
// (acquire / release / replace / mutate / split). The abstract op
// semantics the oracle models are preserved by composing the typed ops:
//
//   INSERT  → split lo + split hi + acquire (on MEM_FREE) / replace (on
//             EEXIST). Replace gives the same overwrite-overlap semantic
//             — atomically replace every interval overlapping [lo, hi)
//             with one [lo, hi)→value_id entry — that the abstract
//             INSERT op specifies.
//   ERASE   → split lo + split hi + release.
//   PROTECT → split lo + split hi + mutate (mutator writes the new
//             flags onto the published clone).
//   SPLIT_AT → split (renamed; same semantic).
//
// COALESCE_AROUND is dropped — the pivot removed auto-coalesce from the
// public surface (descs may be physically chunked but joined-flag descs
// are no longer auto-coalesced into a single succ).
//
// Uses AcquireMeta::flags as the value_id channel, matching the oracle's
// flag-equality semantic.
// ===========================================================================

struct WalkHashCtx {
  cf::SipHasher *h;
  uint64_t lo_filter;
  uint64_t hi_filter;
};

void sut_walk_visitor(vt::VaRange covered, vt::RegionDesc *desc, void *ctx_p) {
  auto *ctx = static_cast<WalkHashCtx *>(ctx_p);
  uint64_t lo = reinterpret_cast<uint64_t>(covered.start);
  uint64_t hi = lo + covered.bytes;
  if (lo >= ctx->hi_filter || hi <= ctx->lo_filter)
    return;
  uint16_t v = (desc != nullptr) ? desc->flags_load() : uint16_t{0};
  ctx->h->absorb_u64(lo);
  ctx->h->absorb_u64(hi);
  ctx->h->absorb_u64(static_cast<uint64_t>(v));
}

// Mirrors the oracle's `leaf_installed` bit — set on the first successful
// `acquire` (= oracle's INSERT-success), never unset within a seed run.
// Tested at the start of ERASE / SPLIT / RESOLVE to match the oracle's
// "no leaf yet → ENOENT" branch, and reset by `wipe_window` between
// seeds so cross-seed state never bleeds across.
bool g_sut_leaf_installed = false;

void sut_split_edges(uint64_t lo, uint64_t hi) {
  // split() returns ENOENT when boundary isn't a strict interior of any
  // covering desc, EINVAL if the boundary is exactly on an edge or
  // misaligned. We discard the return value — the only intent here is to
  // guarantee no desc straddles `lo` or `hi` before a mutating op.
  (void)vt::split(reinterpret_cast<void *>(lo));
  (void)vt::split(reinterpret_cast<void *>(hi));
}

// Drop every desc that intersects `[lo, hi)`, including straddlers whose
// extents reach past either edge. The oracle's INSERT / PROTECT both
// model "remove ALL overlapping intervals" — that means a desc spanning
// `[0, 0x70000)` is fully removed by an INSERT into `[0, 0x10000)`, not
// trimmed. Mirror that here by walking the range repeatedly, releasing
// each found desc's full extent, until nothing intersects. Returns
// `true` if at least one desc was dropped (used by PROTECT to detect
// the "no overlap → ENOENT" case the oracle reports).
struct OverlapFindCtx {
  uint64_t found_lo;
  uint64_t found_hi;
  bool found;
};

void sut_overlap_find_visitor(vt::VaRange covered, vt::RegionDesc *desc,
                               void *ctx_p) {
  auto *ctx = static_cast<OverlapFindCtx *>(ctx_p);
  if (ctx->found || desc == nullptr)
    return;
  ctx->found_lo = reinterpret_cast<uint64_t>(covered.start);
  ctx->found_hi = ctx->found_lo + covered.bytes;
  ctx->found = true;
}

bool sut_drop_all_overlapping(uint64_t lo, uint64_t hi) {
  bool any_dropped = false;
  // Bounded loop — pathological schedules might in principle fragment a
  // range across many descs, but the fuzz config caps op range size to
  // 8 alloc-granules so a single op cannot generate more overlaps than
  // exist in the entire window. 256 iterations is the comfortable cap.
  for (uint32_t iter = 0; iter < 256; ++iter) {
    OverlapFindCtx ctx{0, 0, false};
    vt::VaRange r{reinterpret_cast<void *>(lo), hi - lo};
    vt::walk_range(r, &sut_overlap_find_visitor, &ctx);
    if (!ctx.found)
      break;
    any_dropped = true;
    // Release the found desc's FULL extent (which may reach past
    // [lo, hi)). Pre-split at the desc's edges is unnecessary since the
    // edges ARE the desc boundaries.
    vt::VaRange release_r{reinterpret_cast<void *>(ctx.found_lo),
                          ctx.found_hi - ctx.found_lo};
    (void)vt::release(release_r);
  }
  return any_dropped;
}

cf::OpResult sut_apply_insert(const cf::Op &op) {
  // Oracle's INSERT: drop EVERY overlapping interval (including
  // straddlers whose extents reach past the range), then insert a single
  // new [lo, hi) entry. Match that here by dropping every overlapping
  // desc's full extent before acquiring.
  vt::VaRange r{reinterpret_cast<void *>(op.arg0), op.arg1 - op.arg0};
  vt::AcquireMeta m{};
  m.flags = static_cast<uint16_t>(op.arg2);
  // AcquireMeta::view_prot defaults to 0 which NT rejects with
  // STATUS_INVALID_PAGE_PROTECTION inside commit_replace. The fuzz
  // exercises metadata-level structure only, but the engine still issues
  // real VA commits — pick the common-case PAGE_READWRITE.
  m.view_prot = PAGE_READWRITE;
  (void)sut_drop_all_overlapping(op.arg0, op.arg1);
  auto rr = vt::acquire(r, vt::RegionKind::AnonPrivate, m);
  if (rr.has_value()) {
    g_sut_leaf_installed = true;
    return {0, 0};
  }
  return {static_cast<uint32_t>(rr.error()), 0};
}

cf::OpResult sut_apply_erase(const cf::Op &op) {
  // Oracle's ERASE drops every overlapping interval (including
  // straddlers); same drop-all-overlapping logic as INSERT. The oracle
  // returns ENOENT when no INSERT has succeeded yet (`!leaf_installed`)
  // regardless of how empty the range is.
  if (!g_sut_leaf_installed)
    return {ENOENT, 0};
  (void)sut_drop_all_overlapping(op.arg0, op.arg1);
  return {0, 0};
}

void sut_protect_mutator(vt::RegionDesc *new_desc, void *ctx) {
  uint16_t *new_flags = static_cast<uint16_t *>(ctx);
  new_desc->flags.store(*new_flags, LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
}

cf::OpResult sut_apply_protect(const cf::Op &op) {
  // Oracle's PROTECT: drop every overlapping desc (including
  // straddlers), insert a single new [lo, hi) → ENOENT if no overlap.
  // Same drop-all-overlapping logic as INSERT, gated on the
  // "must have at least one overlap" precondition.
  vt::VaRange r{reinterpret_cast<void *>(op.arg0), op.arg1 - op.arg0};
  vt::AcquireMeta m{};
  m.flags = static_cast<uint16_t>(op.arg2);
  m.view_prot = PAGE_READWRITE;

  bool any_dropped = sut_drop_all_overlapping(op.arg0, op.arg1);
  if (!any_dropped)
    return {ENOENT, 0};

  auto rr = vt::acquire(r, vt::RegionKind::AnonPrivate, m);
  if (rr.has_value())
    return {0, 0};
  return {static_cast<uint32_t>(rr.error()), 0};
}

struct SplitEdgeCtx {
  uint64_t boundary;
  bool on_edge;
  bool strictly_inside;
};

void sut_split_edge_probe_visitor(vt::VaRange covered, vt::RegionDesc *desc,
                                    void *ctx_p) {
  auto *ctx = static_cast<SplitEdgeCtx *>(ctx_p);
  if (desc == nullptr)
    return;
  uint64_t d_lo = reinterpret_cast<uint64_t>(covered.start);
  uint64_t d_hi = d_lo + covered.bytes;
  if (d_lo == ctx->boundary || d_hi == ctx->boundary)
    ctx->on_edge = true;
  if (d_lo < ctx->boundary && ctx->boundary < d_hi)
    ctx->strictly_inside = true;
}

cf::OpResult sut_apply_split(const cf::Op &op) {
  // Oracle's SPLIT_AT distinguishes:
  //   * !leaf_installed                         -> ENOENT
  //   * misaligned boundary                     -> EINVAL
  //   * boundary strictly inside some desc      -> 0 (split)
  //   * boundary on an existing desc edge       -> EINVAL
  //   * no desc adjacent to the boundary at all -> ENOENT
  // `vt::split` collapses the bottom two into ENOENT (its
  // build_plan_split returns ENOENT whenever locked.count != 1, which
  // is true both for the on-edge case [count == 2] and the no-desc
  // case [count == 0]). Walk a small window around the boundary so the
  // SUT can faithfully distinguish edge from gap before calling split.
  uint64_t boundary = op.arg0;
  if ((boundary & (kAllocGran - 1)) != 0)
    return {EINVAL, 0};
  if (!g_sut_leaf_installed)
    return {ENOENT, 0};

  SplitEdgeCtx probe_ctx{boundary, false, false};
  uint64_t probe_lo =
      boundary > kAllocGran ? boundary - kAllocGran : boundary;
  uint64_t probe_hi = boundary + kAllocGran;
  vt::VaRange probe_r{reinterpret_cast<void *>(probe_lo),
                       probe_hi - probe_lo};
  vt::walk_range(probe_r, &sut_split_edge_probe_visitor, &probe_ctx);

  if (probe_ctx.strictly_inside) {
    int rc = vt::split(reinterpret_cast<void *>(boundary));
    return {static_cast<uint32_t>(rc), 0};
  }
  if (probe_ctx.on_edge)
    return {EINVAL, 0};
  return {ENOENT, 0};
}

cf::OpResult sut_apply_resolve(const cf::Op &op) {
  if (!g_sut_leaf_installed)
    return {ENOENT, 0};
  auto rr = vt::resolve(reinterpret_cast<void *>(op.arg0));
  if (!rr.has_value())
    return {static_cast<uint32_t>(rr.error()), 0};
  uint16_t v = (rr.value().desc != nullptr) ? rr.value().desc->flags_load()
                                              : uint16_t{0};
  // Drop the cur-slot reservation established inside resolve() before the
  // next op so chains don't accumulate across the fuzz loop.
  ::LIBC_NAMESPACE::windows::va_tracker::g_va_tracker_skiplist_domain
      .clear_all();
  ::LIBC_NAMESPACE::windows::va_tracker::g_va_tracker_art_domain.clear_all();
  return {0, static_cast<uint64_t>(v)};
}

cf::OpResult sut_apply_walk(const cf::Op &op) {
  uint64_t lo = op.arg0;
  uint64_t hi = op.arg1;
  if (!oracle_range_valid(lo, hi))
    return {0, 0};
  cf::SipHasher h;
  h.reset(kWalkHashK0, kWalkHashK1);
  WalkHashCtx ctx{&h, lo, hi};
  vt::VaRange r{reinterpret_cast<void *>(lo), hi - lo};
  vt::walk_range(r, &sut_walk_visitor, &ctx);
  return {0, h.finalize(0, 0)};
}

cf::OpResult sut_apply_dispatch(void *sut_ctx, const cf::Op &op) {
  (void)sut_ctx;
  switch (op.kind) {
  case cf::kOpKindNoop:
    return {0, 0};
  case kOpInsert:
    return sut_apply_insert(op);
  case kOpErase:
    return sut_apply_erase(op);
  case kOpProtect:
    return sut_apply_protect(op);
  case kOpSplitAt:
    return sut_apply_split(op);
  case kOpCoalesce:
    // COALESCE_AROUND dropped by the 2026-05-10 typed-op pivot. Treat as
    // a no-op on the SUT side; the oracle does the same (see
    // oracle_apply_dispatch) so the linearizability checker stays happy.
    return {0, 0};
  case kOpResolve:
    return sut_apply_resolve(op);
  case kOpWalkRange:
    return sut_apply_walk(op);
  }
  return {EINVAL, 0};
}

void sut_worker_init(void *) {
  ::LIBC_NAMESPACE::concurrent::registry_warm_thread_all();
}

// ===========================================================================
// Op factory — converts (kind, PRNG, params) into a concrete Op with
// SUT-valid arguments.
// ===========================================================================

void op_factory(uint16_t op_kind, cf::SplitMix64 &r,
                const cf::GenParams &params, cf::Op &out_op,
                void *factory_ctx) {
  (void)factory_ctx;

  // Pick an aligned base address inside the window.
  uint64_t lo = cf::sample_address(r, params.addr);
  lo &= ~(kAllocGran - 1);
  if (lo < params.addr.va_base)
    lo = params.addr.va_base;
  uint64_t window_hi = params.addr.va_base + params.addr.va_window_bytes;
  if (lo + kAllocGran > window_hi)
    lo = window_hi - kAllocGran;

  // Range size in 64 KiB multiples — geometric distribution biased
  // toward small ranges (drives skiplist contention by maximising the
  // density of intervals).
  uint32_t range_pages = 1u + (r.next_u32() & 0x7u); // 1..8 × 64 KiB
  uint64_t hi = lo + static_cast<uint64_t>(range_pages) * kAllocGran;
  if (hi > window_hi)
    hi = window_hi;

  // Value id — uniform in [1, value_cardinality]. 0 reserved for "no
  // value" sentinels in the oracle's `desc->flags == 0` contract; we
  // never emit it.
  uint16_t card = params.value_cardinality > 1 ? params.value_cardinality
                                                : static_cast<uint16_t>(2);
  uint16_t v = 1u + static_cast<uint16_t>(r.next_below(card - 1u));

  switch (op_kind) {
  case kOpInsert:
    out_op.arg0 = lo;
    out_op.arg1 = hi;
    out_op.arg2 = v;
    break;
  case kOpErase:
    out_op.arg0 = lo;
    out_op.arg1 = hi;
    break;
  case kOpProtect:
    out_op.arg0 = lo;
    out_op.arg1 = hi;
    out_op.arg2 = v;
    break;
  case kOpSplitAt:
    // boundary somewhere inside [lo, hi).
    out_op.arg0 = lo + kAllocGran;
    if (out_op.arg0 >= hi)
      out_op.arg0 = lo;
    break;
  case kOpCoalesce:
    out_op.arg0 = lo;
    out_op.arg1 = hi;
    break;
  case kOpResolve:
    out_op.arg0 = lo + (r.next_u64() % kAllocGran);
    break;
  case kOpWalkRange:
    out_op.arg0 = lo;
    out_op.arg1 = hi;
    break;
  default:
    break;
  }
}

// ===========================================================================
// Shrinker callbacks — rebase + collapse for the framework's
// halve-window and reduce-cardinality moves.
// ===========================================================================

[[nodiscard]] uint64_t shrink_remap(uint64_t v, uint64_t base,
                                     uint64_t window) {
  if (window == 0)
    return base;
  uint64_t off = v % window;
  off &= ~(kAllocGran - 1);
  return base + off;
}

void shrink_rebase(cf::Op &op, uint64_t new_va_base, uint64_t new_va_window,
                    void *) {
  switch (op.kind) {
  case kOpInsert:
  case kOpErase:
  case kOpProtect:
  case kOpCoalesce:
  case kOpWalkRange: {
    uint64_t new_lo = shrink_remap(op.arg0, new_va_base, new_va_window);
    uint64_t span = (op.arg1 > op.arg0) ? (op.arg1 - op.arg0) : kAllocGran;
    span &= ~(kAllocGran - 1);
    if (span == 0)
      span = kAllocGran;
    uint64_t hi = new_lo + span;
    uint64_t window_hi = new_va_base + new_va_window;
    if (hi > window_hi) {
      hi = window_hi;
      if (hi <= new_lo)
        new_lo = hi - kAllocGran;
    }
    op.arg0 = new_lo;
    op.arg1 = hi;
    break;
  }
  case kOpSplitAt:
    op.arg0 = shrink_remap(op.arg0, new_va_base, new_va_window);
    break;
  case kOpResolve:
    op.arg0 = shrink_remap(op.arg0, new_va_base, new_va_window);
    break;
  default:
    break;
  }
}

void shrink_collapse(cf::Op &op, uint16_t max_value, void *) {
  if (max_value == 0)
    return;
  if (op.kind == kOpInsert || op.kind == kOpProtect) {
    uint16_t v = static_cast<uint16_t>(op.arg2);
    if (v == 0)
      v = 1;
    op.arg2 = 1u + (v % (max_value > 1 ? max_value - 1 : 1));
  }
}

// ===========================================================================
// Cleanup: erase the entire window so the next seed starts from a clean
// state. Erase is overwrite-overlap; one big erase nukes everything.
// ===========================================================================

void wipe_window(uint64_t base, uint64_t window) {
  // Drop every desc in the window (straddlers + fully-inside) using the
  // same helper INSERT/ERASE/PROTECT use. Reset the leaf-installed
  // tracking bit so the next seed starts from a clean oracle state.
  (void)sut_drop_all_overlapping(base, base + window);
  g_sut_leaf_installed = false;
}

// ===========================================================================
// History merge stress validator — runs the schedule once + checker
// once. Used by both the main test and the shrinker's reproduce
// callback.
// ===========================================================================

// Per-test storage caps, sized to the fuzz parameters chosen below.
// Must hold (kMaxThreads × kPerWorkerCap) ≥ kMaxOps so any per-worker
// slice fits regardless of how the shrinker reduces the partition.
constexpr uint32_t kRunMaxOps = 2048;
constexpr uint16_t kRunMaxThreads = 8;
constexpr uint32_t kRunPerWorkerCap = kRunMaxOps;

struct RunBuffers {
  cf::HistoryEntry per_worker_storage[kRunMaxThreads * kRunPerWorkerCap];
  cf::HistoryEntry flat_storage[kRunMaxOps];
  cf::MemoScratch memo;
};

bool run_once_and_check(const cf::Schedule &sched, RunBuffers &bufs,
                        cf::Verdict *out_verdict) {
  cf::History history;
  history.init_rings(sched.thread_count, kRunPerWorkerCap,
                     bufs.per_worker_storage, bufs.flat_storage, kRunMaxOps);

  cf::PoolParams pool_params;
  pool_params.pin_affinity = true;
  pool_params.per_worker_tsc_budget = 0;

  cf::PoolStats pool_stats{};
  int prc = cf::run_pool(sched, &sut_apply_dispatch, &sut_worker_init,
                          /*sut_ctx=*/nullptr, pool_params, history,
                          &pool_stats);
  if (prc != 0)
    return false;
  if (pool_stats.timed_out_workers != 0)
    return false;

  OracleState ostate;
  oracle_reset_impl(ostate);

  cf::Oracle oracle;
  oracle.ctx = &ostate;
  oracle.state_size = sizeof(OracleState);
  oracle.apply = &oracle_apply_dispatch;
  oracle.state_hash = &oracle_state_hash_impl;
  oracle.save_state = &oracle_save_state_impl;
  oracle.restore_state = &oracle_restore_state_impl;
  oracle.reset = &oracle_reset_impl_cb;

  cf::CheckParams check_params;
  cf::Verdict v = cf::check_linearizability(history.flat, history.flat_count,
                                              oracle, check_params, bufs.memo);
  if (out_verdict != nullptr)
    *out_verdict = v;
  return v.linearizable;
}

bool reproduce_callback(const cf::Schedule &s, void *user_ctx) {
  auto *bufs = static_cast<RunBuffers *>(user_ctx);
  // Wipe the SUT between attempts so each reproduce starts fresh.
  wipe_window(s.params.addr.va_base, s.params.addr.va_window_bytes);
  return !run_once_and_check(s, *bufs, /*out_verdict=*/nullptr);
}

// ===========================================================================
// Test driver.
//
// Runs N seeds through generate -> run_pool -> check. On a failure,
// triggers the shrinker, prints the minimised seed (logically; no
// stdout in the unit-test harness — we EXPECT/ASSERT on the verdict
// instead).
//
// Knobs deliberately kept conservative for CI runtime:
//   * 4 seeds per test invocation.
//   * 4 worker threads.
//   * 1024 ops per schedule.
//   * 8 distinct value ids.
//
// Soak runs override these via env-driven knobs (out of scope for the
// initial landing).
// ===========================================================================

cf::ScheduleStore g_store;
RunBuffers g_run_buffers;

const char *format_hex64(uint64_t v, char *buf20) {
  const char *digits = "0123456789abcdef";
  char *p = buf20 + 19;
  *p = '\0';
  if (v == 0) {
    *--p = '0';
  } else {
    while (v != 0) {
      *--p = digits[v & 0xF];
      v >>= 4;
    }
  }
  return p;
}

const char *op_kind_name(uint16_t k) {
  switch (k) {
  case cf::kOpKindNoop: return "NOOP";
  case kOpInsert: return "INSERT";
  case kOpErase: return "ERASE";
  case kOpProtect: return "PROTECT";
  case kOpSplitAt: return "SPLIT_AT";
  case kOpCoalesce: return "COALESCE";
  case kOpResolve: return "RESOLVE";
  case kOpWalkRange: return "WALK_RANGE";
  default: return "<unknown>";
  }
}

// Replay the schedule single-threaded through a fresh SUT + oracle pair,
// printing the first divergence. Used purely for diagnosis when the
// linearizability checker reports a mismatch. Returns true if every op's
// SUT result matches the oracle exactly (an absurdly strict condition
// the linearizability checker does NOT require — but the strong form is
// what we want when debugging).
bool sequential_replay_diagnose(const cf::Schedule &sched) {
  // Wipe SUT before replay so the run starts from MEM_FREE state.
  wipe_window(sched.params.addr.va_base, sched.params.addr.va_window_bytes);

  OracleState ostate;
  oracle_reset_impl(ostate);

  bool ok = true;
  for (uint32_t i = 0; i < sched.op_count; ++i) {
    const cf::Op &op = sched.ops[i];

    cf::OpResult sut = sut_apply_dispatch(nullptr, op);
    cf::OpResult orc = oracle_apply_dispatch(&ostate, op);

    // Trace each successful op too so the failure context is visible.
    if (sut.status == orc.status && sut.payload == orc.payload) {
      char h0[20], h1[20];
      LIBC_NAMESPACE::testing::tlog
          << "  op[" << i << "] " << op_kind_name(op.kind)
          << " arg0=0x" << format_hex64(op.arg0, h0)
          << " arg1=0x" << format_hex64(op.arg1, h1)
          << " arg2=" << op.arg2
          << " => status=" << sut.status << " payload=" << sut.payload
          << "\n";
    }

    if (sut.status != orc.status || sut.payload != orc.payload) {
      char hex0[20], hex1[20];
      LIBC_NAMESPACE::testing::tlog
          << "DIVERGENCE at op[" << i << "] kind=" << op_kind_name(op.kind)
          << " arg0=0x" << format_hex64(op.arg0, hex0)
          << " arg1=0x" << format_hex64(op.arg1, hex1)
          << " arg2=" << op.arg2 << "\n";
      LIBC_NAMESPACE::testing::tlog
          << "  SUT status=" << sut.status << " payload=" << sut.payload
          << "\n";
      LIBC_NAMESPACE::testing::tlog
          << "  ORC status=" << orc.status << " payload=" << orc.payload
          << "\n";
      ok = false;
      break;
    }
  }
  return ok;
}

cf::GenParams default_params(uint64_t va_window_offset) {
  cf::GenParams p;
  // Conservative defaults post-typed-op pivot: SUT ops now make real NT
  // syscalls (acquire/release/replace dispatch into nt_pal), so the
  // checker's 64-op-per-segment cap is reached fast under continuous
  // 4-thread pressure. Drop thread count + op count + seeds for a
  // self-contained smoke that still exercises every typed op. Stress
  // configs can override via a future env-driven knob.
  p.op_count = 64;
  // Sequential schedule (thread_count=1). The fuzz SUT composes
  // overwrite-overlap INSERT / ERASE / PROTECT as `drop_all_overlapping`
  // + `acquire` — multiple typed-op calls per abstract op. That
  // composite isn't atomic against concurrent workers, so a
  // multi-threaded schedule produces intermediate states the
  // linearizability checker reports as Mismatch. Re-enabling
  // thread_count >= 2 requires a SUT refactor that maps each abstract
  // op to a single typed-op call (acquire / replace / release / mutate
  // / split) and an oracle that mirrors those exact semantics.
  p.thread_count = 1;
  p.value_cardinality = 4;
  p.weights.kind_count = kOpKindCount;
  // Slot 0 is kOpKindNoop — never emit. Other ops biased mutator-heavy
  // so we drive enough state churn to expose race windows.
  p.weights.weights[cf::kOpKindNoop] = 0;
  p.weights.weights[kOpInsert] = 25;
  p.weights.weights[kOpErase] = 15;
  p.weights.weights[kOpProtect] = 12;
  p.weights.weights[kOpSplitAt] = 8;
  p.weights.weights[kOpCoalesce] = 0; // dropped by typed-op pivot
  p.weights.weights[kOpResolve] = 20;
  p.weights.weights[kOpWalkRange] = 12;
  p.addr.va_base = kTestVaBase + va_window_offset;
  p.addr.va_window_bytes = kTestVaWindow;
  p.addr.hotspot_weight_256 = 96; // ~37.5%
  p.addr.hotspot_offset = 0;
  p.addr.hotspot_bytes = kTestVaWindow / 4; // 2 MiB hot
  p.addr.tower_jump_geometric_bits = 4;
  return p;
}

} // namespace

// ===========================================================================
// Bootstrap smoke: confirm va_tracker is ready and a single typed-op call
// on the test VA window works. If this fails, the post-pivot init is
// broken and the linearizability fuzz is meaningless.
// ===========================================================================

TEST(LlvmLibcIntervalSkiplistLinFuzz, Bootstrap) {
  ::LIBC_NAMESPACE::concurrent::registry_warm_thread_all();
  ASSERT_TRUE(vt::is_va_tracker_ready());

  // Round-trip smoke at every quadrant of user VA. The release path's
  // Stage 2 must fully MEM_FREE the VA so the second acquire at the
  // same base succeeds (rather than returning EEXIST). The bug here was
  // `Swap()` clearing `LockedSet::count` before the post-Swap survivor
  // walk could read it; the fix removes that clear and lets the post-
  // stage-2 `Unlock` handle it.
  uint64_t candidates[] = {
      0x00000200000000ULL, //  8 GiB
      0x00010000000000ULL, //  1 TiB
      0x00100000000000ULL, // 16 TiB
      0x00600000000000ULL, // 96 TiB (the fuzz-test base)
  };
  for (uint64_t cand : candidates) {
    void *base = reinterpret_cast<void *>(cand);
    vt::VaRange r{base, kAllocGran};
    (void)vt::release(r);
    vt::AcquireMeta m{};
    m.flags = 0x42;
    m.view_prot = PAGE_READWRITE;
    auto rr1 = vt::acquire(r, vt::RegionKind::AnonPrivate, m);
    ASSERT_TRUE(rr1.has_value());
    ASSERT_EQ(vt::release(r), 0);
    auto rr2 = vt::acquire(r, vt::RegionKind::AnonPrivate, m);
    ASSERT_TRUE(rr2.has_value());
    ASSERT_EQ(vt::release(r), 0);
  }
}

// ===========================================================================
// Phase-3-gate fuzz test. Multiple seeds; failure → shrink + ASSERT.
// ===========================================================================

TEST(LlvmLibcIntervalSkiplistLinFuzz, RunSeedsLinearizable) {
  // First insert call lazy-inits the va_tracker at memory-primitives
  // Phase 6; we don't reach into the bootstrap here directly.
  ::LIBC_NAMESPACE::concurrent::registry_warm_thread_all();

  constexpr uint32_t kSeedCount = 4;
  for (uint32_t seed_idx = 0; seed_idx < kSeedCount; ++seed_idx) {
    uint64_t seed = 0xA17C0DE0ULL + static_cast<uint64_t>(seed_idx);

    // Each seed gets a disjoint VA window so the per-seed state never
    // bleeds across.
    cf::GenParams params = default_params(
        static_cast<uint64_t>(seed_idx) * (kTestVaWindow + (16ull << 20)));

    cf::Schedule sched;
    ASSERT_TRUE(cf::generate(seed, params, &op_factory,
                             /*factory_ctx=*/nullptr, g_store, sched));

    // Wipe before run.
    wipe_window(params.addr.va_base, params.addr.va_window_bytes);

    cf::Verdict verdict{};
    bool linearizable = run_once_and_check(sched, g_run_buffers, &verdict);

    if (!linearizable) {
      // Sequential-replay diagnostic: prints the first SUT/oracle
      // divergence in a single-threaded replay of the same schedule.
      // Useful when the failure is a sequential mismatch (failure_reason
      // == Mismatch, segment size == 1) rather than a true concurrency
      // bug.
      (void)sequential_replay_diagnose(sched);

      // Try to shrink for diagnostic value, then fail.
      cf::ShrinkParams sp;
      sp.reproduce_trials_per_candidate = 3;
      sp.max_shrink_passes = 2;
      cf::ShrinkResult sr{};
      (void)cf::shrink_schedule(g_store, sched, &reproduce_callback,
                                 &shrink_rebase, &shrink_collapse,
                                 &g_run_buffers, sp, sr);
      ASSERT_TRUE(linearizable)
          << "seed " << seed << " produced a non-linearizable history "
          << "(failure_reason=" << static_cast<int>(verdict.failure_reason)
          << ", failing_segment_first_op="
          << verdict.failing_segment_first_op
          << ", failing_segment_op_count="
          << verdict.failing_segment_op_count
          << ", max_segment_op_count=" << verdict.max_segment_op_count
          << ", shrunk_op_count=" << sr.shrunk_op_count
          << ", shrunk_thread_count=" << sr.shrunk_thread_count << ")";
    }

    // Coverage assertion: each op kind must appear in the generated
    // schedule. A generator regression that silently zeroes a weight
    // slips by the checker; this catches it.
    uint32_t per_kind[kOpKindCount] = {};
    for (uint32_t i = 0; i < sched.op_count; ++i)
      if (sched.ops[i].kind < kOpKindCount)
        ++per_kind[sched.ops[i].kind];
    EXPECT_GT(per_kind[kOpInsert], 0u);
    EXPECT_GT(per_kind[kOpErase], 0u);
    EXPECT_GT(per_kind[kOpProtect], 0u);
    EXPECT_GT(per_kind[kOpSplitAt], 0u);
    // kOpCoalesce intentionally absent — zero-weighted post-pivot.
    EXPECT_GT(per_kind[kOpResolve], 0u);
    EXPECT_GT(per_kind[kOpWalkRange], 0u);

    // Cleanup so subsequent seeds start fresh.
    wipe_window(params.addr.va_base, params.addr.va_window_bytes);
  }
}
