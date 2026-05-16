//===- va_tracker_transaction.cpp - record-then-commit engine -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Transactional commit engine for the Layer 1 VA tracker. Implements the five
// typed top-level operations the public surface exposes — acquire, release,
// replace, mutate, split — each as a single record-then-commit envelope that
// runs Lock + plan-build + nt_pal kernel program + Swap-publish + post-Swap
// fixups + synchronous Stage 2 backing teardown + Unlock.
//
// The substrate is a single concurrent interval skiplist per arena, per Kim,
// Kwon, Kang, "Scalable Address Spaces using Concurrent Interval Skiplist"
// (SOSP 2025). Multi-entry atomic mutations route here; there is no
// lock-then-mutate primitive on the public surface, and single-entry ops are
// ergonomic shorthands implemented as one-step transactions over this same
// engine.
//
// Pipeline (each envelope, in order)
// ----------------------------------
//   Lock         Acquire the per-node interval-scoped locks (encoded in the
//                `state` byte of each node's `next[0].val`) for the overlap
//                set, in VA order, via `LockedSet::acquire`. Disjoint VA
//                operations proceed in parallel without serializing on any
//                tree-wide lock — the property Kim et al. report at 13.1×
//                mmap-microbench scale.
//   Plan-build   Pure inspection of the locked succ set; fills a stack-
//                resident `CommitPlan` of mode tags plus at most three named
//                commit slots (`inside_commit`, `left_sibling`,
//                `right_sibling`). Nothing in the plan scales with
//                `locked.count`; per-succ work iterates inline in later
//                phases.
//   Demote       Per locked succ, dispatch on `RegionDesc::current_shape()`:
//                section views go through `nt_pal::unmap_view_preserve` (the
//                whole view becomes one placeholder); private commits use
//                `nt_pal::preserve_to_placeholder` and only for descs fully
//                inside `intent.range` (outside survivors must keep their
//                data).
//   Split        For section-view shared backings, `nt_pal::split_placeholder`
//                at each intent edge that has an outside-range survivor —
//                carves the inside slice from the surviving siblings.
//                Private-commit shared backings need no split: their outside
//                slices are still committed and require no placeholder carve.
//   Coalesce     For replace with multiple post-demote placeholders inside
//                the intent range, `nt_pal::coalesce_placeholders` merges
//                them into one before re-commit.
//   Commit       For acquire / replace, per named commit slot: allocate a
//                fresh `DescBacking` (pushed onto the provisional list for
//                rollback safety), optionally `nt_pal::reserve_placeholder`
//                (acquire only), then `commit_replace` (private) or
//                `map_section_replace` (section view), populate kernel state
//                on the backing, build a `RegionDesc`, append the node.
//   Clone        For mutate / split / replace's outside-survivor rebind:
//                clone source descs and append new nodes — no kernel work
//                in this phase.
//   Swap         The linearization point. Atomically detaches the OLD nodes
//                and publishes the NewNodes via the skiplist's overlap-set
//                Swap. If the Swap CAS loses, the entire attempt rolls back
//                (provisional backings retired, unpublished nodes returned)
//                and the bounded-retry loop tries again.
//   Protect      For mutate with `prot_change != 0`, per locked succ issue
//                `nt_pal::protect`. NT's `NtProtectVirtualMemory` is per-VAD
//                atomic, so each demoted-and-split placeholder needs its own
//                call.
//   Ownership    For replace, RELEASE-store nullptr to each OLD backing's
//                `placeholder_base` whose identity was consumed by a new
//                commit; Stage 2's `backing_kill_and_retire` then skips the
//                `free_placeholder` for that backing. Section/file handles
//                stay set and Stage 2 closes them. Skips OLD backings reused
//                by a NEW node (sibling re-map).
//   Stage 2      Synchronous backing teardown. Per locked succ: state-load
//                short-circuit absorbs duplicates and cross-envelope races
//                in O(1); NewNode-survivor check holds mutate-path clones
//                alive (they reference the OLD backing); outside-range
//                survivor scan walks the slivers `(b_lo, range.lo)` and
//                `(range.hi, b_hi)` for any LIVE referencer; finally a
//                Live → Killed CAS, with the winner running
//                `backing_kill_and_retire` (unmap if section view, free
//                placeholder, close handles, retire to Crystalline grace).
//   Unlock       Release the locked nodes, OLD nodes already detached by
//                Swap.
//
// Kernel-state transition envelope
// --------------------------------
// Each typed op moves a VA range between four kernel states — `MEM_FREE`,
// `RESERVE` (placeholder), `COMMIT` (private), `MAPPED` (section view). No
// transition crosses `MEM_FREE` outside of `acquire`'s entry and `release`'s
// exit: the placeholder model (NT's `MEM_REPLACE_PLACEHOLDER`) gives a
// zero-race-window VA ownership transfer for `replace`, so DLL loads or
// external mappers cannot grab the VA between demote and re-commit. No
// freeze bracket needed.
//
// Straddle precondition
// ---------------------
// `release`, `replace`, and `mutate` reject any locked set containing a
// straddler — a desc whose extent extends past the operation range — with
// `-EINVAL`. The caller pre-splits via `split()`, which is pure metadata
// work and runs as a one-step transaction here. This keeps the engine's
// kernel-transition logic linear: a desc is either entirely inside or
// entirely outside `intent.range`. Without this precondition, surviving
// fragments would require per-fragment `NtDuplicateObject` of section
// handles — an O(N) syscall blow-up that violates the budget rule that
// fragmenting ops must run in O(1) NT calls per fragment, regardless of
// fragment count.
//
// Replace topology — structurally bounded at three commit slots
// -------------------------------------------------------------
// `build_plan_replace` accepts every shared-backing topology the substrate
// can produce. Three structurally-named CommitPlan slots cover the entire
// shape:
//
//   inside_commit  — always present for replace; covers `intent.range` with
//                    the caller-supplied kind / handles / offset.
//   left_sibling   — present when the leftmost locked backing's extent
//                    extends past `intent.range.lo`. Carries OLD shape,
//                    OLD section handle, OLD section_offset_at_lo, uniform
//                    per-desc fields for the rebound clones.
//   right_sibling  — symmetric on the right edge.
//
// No array, no count, no `-E2BIG`. Interior succs cannot have backings
// extending past edges by VA contiguity, so the topology is structurally
// bounded at three. The heterogeneous-prot-across-one-backing case returns
// `-ENOTSUP`: multi-prot sibling re-map needs multiple views per edge, a
// shape the current plan layout does not encode.
//
// Locked-range expansion preflight
// --------------------------------
// When a shared backing's extent extends past either edge of `intent.range`,
// `run_envelope` runs a one-shot preflight: lock on `intent.range`, scan
// locked succs for backings extending past edges, widen the lock range to
// the union of those extents, drop and re-acquire on the wider range.
// Convergence in one iteration: after widening to (min over leftmost
// backings' b_lo, max over rightmost backings' b_hi), no further extension
// is possible by VA contiguity.
//
// Per-fragment kernel program
// ---------------------------
// For section-view shared backings:
//   * Demote: whole-view `unmap_view_preserve_transient(view_base)` covers
//     all sharers. Section storage preserves data; sibling re-map below
//     restores the surviving slice's mapping. Subsequent iterations over
//     duplicate succs of the same backing tolerate `STATUS_NOT_MAPPED_VIEW`.
//   * `split_placeholder` at `intent.range.lo` and/or `intent.range.hi`.
//     After splits, the inside slice is its own placeholder; sibling slices
//     are their own placeholders awaiting re-map.
//   * Sibling re-map: `map_section_replace` against the OLD section handle
//     at each surviving slice. The new sibling backing carries
//     `section_handle = nullptr`; the OLD backing keeps the kernel handle.
//     The kernel's view-section internal reference keeps the section alive
//     across OLD-handle close in Stage 2.
//
// For private-commit shared backings:
//   * Demote: ONLY the inside-intent succs. Outside survivors stay committed
//     — `decommit_preserve` would discard their data (private commits have
//     no backing storage).
//   * No split needed. Surviving slices are still committed.
//   * Sibling: pure metadata; no kernel work. The new sibling backing's
//     `placeholder_base` reflects the surviving slice; on eventual kill,
//     `free_placeholder` releases the committed VAD entry directly (NT
//     `MEM_RELEASE` works on committed ranges too).
//
// Per-fragment kernel work is O(1) regardless of survivor count: at most
// one demote per backing (deduped via shared `view_base` for sections,
// gated on inside-intent for private), at most one `split_placeholder` per
// edge, at most one `map_section_replace` per edge.
//
// DescBacking lifecycle
// ---------------------
// Active backings carry kernel state and have `state == Live`. When no
// NewNode references the backing AND no LIVE desc exists in the backing's
// extent outside the locked range, Stage 2 races the `Live → Killed` CAS;
// the winner runs `backing_kill_and_retire` synchronously, settling kernel
// state before the envelope returns. The metadata-only retirement happens
// asynchronously when Crystalline-W grace fires — `desc_backing_free`
// touches no `nt_pal::*` and calls no `NtClose`. This split is mandatory:
// kernel-state lifecycle must live in the synchronous mutator path because
// Crystalline-W is asynchronous (Nikolaev & Ravindran, PLDI 2024) and
// offers no synchronous grace primitive — a discipline that puts kernel
// teardown in a FreeFn would be structurally unachievable.
//
// Provisional rollback
// --------------------
// Every `backing_alloc` in the envelope is pushed onto a 3-slot inline
// `ProvisionalList`. Visitor failure during the commit phase or post-
// execute Swap-CAS loss walks the list, RELAXED-stores `Killed`, runs
// `backing_kill_and_retire`. The capacity equals the maximum number of
// named commit slots in `CommitPlan` (inside + left + right sibling); a
// counter would be a maintainability hazard against future shape changes.
//
// Pagemap integration
// -------------------
// After Swap-success and before Unlock, the engine publishes pagemap
// entries covering each NewNode's per-node range (NOT the wider backing
// extent), then retires each OLD node's per-node range skipping chunks any
// NewNode covers. The single-publisher invariant holds for both passes —
// each fragment owns its own chunks.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/va_tracker.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/memory/desc_backing.h"
#include "src/__support/OSUtil/windows/memory/interval_skiplist.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
#include "src/__support/OSUtil/windows/nt_pal/query.h"
#include "src/__support/OSUtil/windows/nt_pal/section.h"
#include "src/__support/OSUtil/windows/nt_pal/protect.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

namespace {

//===----------------------------------------------------------------------===//
//  Range validation and arena geometry
//===----------------------------------------------------------------------===//

// NT's allocation granularity. Fresh-VA reservation (`NtAllocateVirtualMemoryEx
// (MEM_RESERVE_PLACEHOLDER)`) is the only kernel constraint that requires it;
// interior placeholder operations (split / partial release / replace / mutate)
// accept page granularity per `Placeholders.md` §2.
constexpr uintptr_t kAllocGranularity = 64u * 1024u;

// NT page granularity. The lower bound for interior placeholder operations
// (split / release / replace / mutate). Empirically confirmed against the
// kernel in `Placeholders.md` §2: split offsets at 4 KiB and 8 KiB succeed,
// 1-byte offsets are rejected with `STATUS_INVALID_PARAMETER_1`.
constexpr uintptr_t kPageGranularity = 4u * 1024u;

// Validator for acquire-family ops (`OpKind::Acquire`,
// `OpKind::AcquireAtReserved`). Base must be NT-allocation-granularity
// aligned (the kernel's `MEM_RESERVE_PLACEHOLDER` placement constraint)
// but `bytes` may be any positive non-wrapping page-aligned value: the
// envelope's NT phase rounds the reservation up to alloc granularity
// internally and shrinks the placeholder to exactly `bytes` before the
// commit step (see the `op.reserve_bytes > op.bytes` branch in
// `run_envelope`'s commit loop).
[[nodiscard]] LIBC_INLINE bool range_valid_acquire(VaRange r) {
  if (r.bytes == 0)
    return false;
  uintptr_t lo = r.lo();
  if ((lo & (kAllocGranularity - 1)) != 0)
    return false;
  if ((r.bytes & (kPageGranularity - 1)) != 0)
    return false;
  uintptr_t hi = lo + r.bytes;
  return hi >= lo;
}

[[nodiscard]] LIBC_INLINE bool range_valid_interior(VaRange r) {
  if (r.bytes == 0)
    return false;
  uintptr_t lo = r.lo();
  if ((lo & (kPageGranularity - 1)) != 0)
    return false;
  if ((r.bytes & (kPageGranularity - 1)) != 0)
    return false;
  uintptr_t hi = lo + r.bytes;
  return hi >= lo;
}

// Arena leaves are 4 GiB-aligned 4 GiB windows; the engine commits one
// envelope per arena and the cross-arena dispatcher chops a multi-arena
// range into per-arena sub-envelopes.
[[nodiscard]] LIBC_INLINE bool fits_one_arena(VaRange r) {
  uintptr_t lo = r.lo();
  uintptr_t hi_inclusive = r.hi() - 1;
  return (lo >> 32) == (hi_inclusive >> 32);
}

[[nodiscard]] LIBC_INLINE uintptr_t arena_hi_for(uintptr_t lo) {
  return (lo & ~uintptr_t{0xFFFFFFFF}) + (uintptr_t{1} << 32);
}

//===----------------------------------------------------------------------===//
//  RegionKind ↔ RegionShape mapping
//===----------------------------------------------------------------------===//

// Anonymous kinds collapse to the placeholder shape; file-/shm-backed kinds
// collapse to the section-view shape. The shape drives demote dispatch
// and Stage 2 unmap gating; the kind drives caller-visible metadata only.
[[nodiscard]] LIBC_INLINE RegionShape shape_for_kind(RegionKind k) {
  switch (k) {
  case RegionKind::AnonPrivate:
  case RegionKind::AnonShared:
  case RegionKind::Brk:
  case RegionKind::StackGuard:
    return RegionShape::ANON_PLACEHOLDER;
  case RegionKind::FilePrivate:
  case RegionKind::FileShared:
  case RegionKind::ShmPosix:
  case RegionKind::ShmSysV:
    return RegionShape::FILE_VIEW_MONO;
  }
  __builtin_unreachable();
}

[[nodiscard]] LIBC_INLINE bool kind_is_section_backed(RegionKind k) {
  switch (k) {
  case RegionKind::FilePrivate:
  case RegionKind::FileShared:
  case RegionKind::ShmPosix:
  case RegionKind::ShmSysV:
    return true;
  case RegionKind::AnonPrivate:
  case RegionKind::AnonShared:
  case RegionKind::Brk:
  case RegionKind::StackGuard:
    return false;
  }
  __builtin_unreachable();
}

//===----------------------------------------------------------------------===//
//  BackingRef encode
//===----------------------------------------------------------------------===//

// Encode a freshly allocated DescBacking into the tagged-pointer-free
// `BackingRef` form (chunk_id, slot_idx, generation). `backing_alloc`
// stamps both coords once at slot initialisation and never mutates them,
// so the encode is three independent loads with no synchronisation
// dependency between them — `generation` is ACQUIRE because subsequent
// `deref_backing_raw` consumers compare against it for slot-recycling ABA
// defence.
[[nodiscard]] LIBC_INLINE BackingRef encode_backing_ref(DescBacking *backing) {
  if (backing == nullptr)
    return kBackingRefNull;
  uint32_t gen = backing->generation.load(cpp::MemoryOrder::ACQUIRE);
  return make_backing_ref(static_cast<uint16_t>(backing->cached_chunk_id),
                           static_cast<uint16_t>(backing->cached_slot_idx),
                           gen);
}

} // namespace

//===----------------------------------------------------------------------===//
//  Stage 2 synchronous backing teardown
//===----------------------------------------------------------------------===//

// Tear down a backing's kernel state synchronously, then retire the metadata
// body to Crystalline-W grace.
//
// Caller must have CAS-transitioned `state` to Killed first — that store is
// the linearisation point declaring ownership of teardown. Ordering across
// the body is shape-driven:
//
//   1. For a section view, unmap the view BEFORE freeing the placeholder.
//      `NtFreeVirtualMemory(MEM_RELEASE)` on a still-mapped view returns
//      `STATUS_UNABLE_TO_DELETE_SECTION`. Tolerate `STATUS_NOT_MAPPED_VIEW`
//      because an earlier demote step may have already unmapped the view
//      (replace path's whole-view demote, or a duplicate-shared-backing
//      iteration). The transient priority hint defers the IPI / page-table
//      flush off the critical path — Stage 2 is a synchronous hot path.
//   2. Free the placeholder. Skipped if `placeholder_base` is null, which
//      happens when the replace path's ownership-transfer step has already
//      moved kernel ownership of the placeholder identity to a new backing.
//   3. Close section handle, then file handle. Either may be null — sibling
//      re-map backings carry no handle ownership and the wider OLD backing
//      closes both handles on its own teardown. Section first because the
//      kernel's view-section internal reference keeps the backing file
//      alive on disk only while the section is open.
//   4. Retire to Crystalline-W. The asynchronous metadata reclamation runs
//      `desc_backing_free`, which touches no kernel state.
//
// The unmap gate is on `shape`, not on `section_handle != nullptr`: sibling
// re-map backings are intentionally created with a null section handle (the
// OLD backing retains it) but are still section-view shape and must be
// unmapped. A handle-based gate would misroute those.
void backing_kill_and_retire(DescBacking *backing) {
  if (LIBC_UNLIKELY(backing == nullptr))
    __builtin_trap();

  LIBC_ASSERT(backing->state.load(cpp::MemoryOrder::ACQUIRE) ==
              kBackingStateKilled);

  // `shape` is plain (set once at `backing_set_kernel_state`, never
  // mutated). The handle/base exchanges are ACQ_REL: ACQUIRE so concurrent
  // reader pins observing the new null see this thread's prior writes;
  // RELEASE so the field-zeroing publishes to any post-grace observer.
  BackingShape shape = backing->shape;

  HANDLE saved_section = backing->section_handle.exchange(
      nullptr, cpp::MemoryOrder::ACQ_REL);
  HANDLE saved_file = backing->file_handle.exchange(
      nullptr, cpp::MemoryOrder::ACQ_REL);
  void *saved_base = backing->placeholder_base.exchange(
      nullptr, cpp::MemoryOrder::ACQ_REL);

  // Section view → placeholder. `NtUnmapViewOfSectionEx` with the
  // transient flag.
  if (shape == BackingShape::SectionView && saved_base != nullptr)
    (void)nt_pal::unmap_view_preserve_transient(saved_base);

  // Release the placeholder VAD entry. `NtFreeVirtualMemory(MEM_RELEASE)`
  // works on both placeholder and committed ranges.
  if (saved_base != nullptr)
    (void)nt_pal::free_placeholder(saved_base);

  // `NtClose` the kernel handles.
  if (saved_section != nullptr)
    (void)::NtClose(saved_section);
  if (saved_file != nullptr)
    (void)::NtClose(saved_file);

  g_va_tracker_backing_domain.retire(backing);
}

namespace {

//===----------------------------------------------------------------------===//
//  Provisional rollback list
//===----------------------------------------------------------------------===//

/// Per-envelope rollback safety net for backings allocated this attempt.
///
/// Every `backing_alloc` site pushes onto this list; on visitor failure or
/// Swap-CAS loss, `rollback_provisional` walks it, marks each Killed, and
/// runs `backing_kill_and_retire`. The fixed capacity equals the maximum
/// number of named commit slots in `CommitPlan` — one inside-range commit
/// plus at most one left-sibling re-map plus at most one right-sibling
/// re-map. Interior succs cannot have backings extending past edges by VA
/// contiguity, so the topology is structurally bounded at three; the
/// `LIBC_ASSERT` is a compile-shape regression catcher, not a runtime
/// expectation.
struct ProvisionalList {
  static constexpr uint32_t kCap = 3;
  DescBacking *items[kCap]{};
  uint32_t count{0};

  LIBC_INLINE void push(DescBacking *b) {
    LIBC_ASSERT(count < kCap &&
                "provisional list overflow — capacity must equal the "
                "number of named CommitPlan commit slots");
    items[count++] = b;
  }

  LIBC_INLINE void clear() {
    for (uint32_t i = 0; i < count; ++i)
      items[i] = nullptr;
    count = 0;
  }
};

void rollback_provisional(ProvisionalList &prov) {
  for (uint32_t i = 0; i < prov.count; ++i) {
    DescBacking *b = prov.items[i];
    if (b == nullptr)
      continue;
    // RELAXED: this thread is the unique owner of the provisional backing
    // until rollback completes; no concurrent reader can race the state
    // transition (the backing has not been published to a chain).
    b->state.store(kBackingStateKilled, cpp::MemoryOrder::RELAXED);
    backing_kill_and_retire(b);
  }
  prov.clear();
}

//===----------------------------------------------------------------------===//
//  RegionDesc / SkiplistNode build helpers
//===----------------------------------------------------------------------===//

// Allocate a `RegionDesc` and populate the fields a commit-phase desc needs:
// backing ref, view protection, flags, shape (encoded from `kind`), and
// section offset. Shape and flags are RELEASE-stored so a reader that
// loads the desc via an ACQUIRE on the chain's node value observes them
// fully initialised.
RegionDesc *build_acquire_desc_from_meta(RegionKind kind, DWORD view_prot,
                                         uint16_t flags, uint64_t section_offset,
                                         BackingRef ref) {
  RegionDesc *desc = region_desc_alloc();
  if (desc == nullptr)
    return nullptr;
  desc->backing_ref = ref;
  desc->section_offset.QuadPart = static_cast<int64_t>(section_offset);
  desc->shape.store(static_cast<uint16_t>(shape_for_kind(kind)),
                    cpp::MemoryOrder::RELEASE);
  desc->view_prot = view_prot;
  desc->flags.store(flags, cpp::MemoryOrder::RELEASE);
  desc->numa_interleave_mask = 0;
  return desc;
}

// Allocate a fresh skiplist node, attach the desc as its value, and append
// to the per-envelope `NewNodes` list. Tower height is sampled from the
// geometric distribution per the Kim, Kwon, Kang interval-skiplist design
// (SOSP 2025). On failure at any step, drop the desc's refcount; on
// `NewNodes::push` failure (capacity reached), the node is fully retired
// to the skiplist domain so it cannot leak.
[[nodiscard]] int append_node_for_desc(NewNodes &out, Arena *arena,
                                        uintptr_t lo, uintptr_t hi,
                                        RegionDesc *desc,
                                        bool cleanup_value_on_abort) {
  if (lo >= hi)
    return -EINVAL;
  SkiplistNodeBase *n = bucket_alloc_node(sample_node_height(), arena);
  if (n == nullptr) {
    region_desc_release(desc);
    return -ENOMEM;
  }
  n->lo = lo;
  n->hi = hi;
  // RELEASE: paired with ACQUIRE in any subsequent reader of `value`. The
  // node is not yet reachable from the chain (Swap publishes), but the
  // clone phase, ownership-transfer step, and Stage 2 read `value` off
  // `NewNodes` directly.
  n->value.store(desc, cpp::MemoryOrder::RELEASE);
  if (!out.push(n, cleanup_value_on_abort)) {
    n->value.store(nullptr, cpp::MemoryOrder::RELAXED);
    g_va_tracker_skiplist_domain.retire(n);
    region_desc_release(desc);
    return -ENOMEM;
  }
  return 0;
}

//===----------------------------------------------------------------------===//
//  Straddle precondition checks
//===----------------------------------------------------------------------===//

// Edge-straddler check for the preflight-expanded locked range's outer
// edges. A straddler here is a desc that crosses one of `[edge_lo,
// edge_hi)`'s edges without being fully inside or fully outside. Replace
// uses this to detect cross-chain abutment at the union of all
// preflight-expanded backings: a desc straddling that outer edge breaks
// the one-iteration convergence of locked-range widening (a second
// chain pulled in by widening could itself extend the bound further),
// so we surface `-ENOTSUP` rather than iterate.
//
// Edge-straddlers at the intent edges themselves are handled by the
// plan builders' auto-split paths (clone phase emits survivor clones
// for outside portions, demote phase clips to inside portions); they
// do NOT use this helper.
[[nodiscard]] LIBC_INLINE bool
locked_has_edge_straddler(const LockedSet &locked, uintptr_t edge_lo,
                           uintptr_t edge_hi) {
  for (uint32_t i = 0; i < locked.count; ++i) {
    SkiplistNodeBase *n = locked.at(i);
    if (n == nullptr)
      continue;
    if (n->lo < edge_lo && n->hi > edge_lo)
      return true;
    if (n->lo < edge_hi && n->hi > edge_hi)
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
//  CommitIntent and CommitPlan
//===----------------------------------------------------------------------===//

// Op kind dispatched into the per-arena envelope. Each public typed op
// constructs a `CommitIntent` with the matching `OpKind` and routes through
// `dispatch_per_arena_op`.
enum class OpKind : uint8_t {
  Acquire           = 0,
  Release           = 1,
  Replace           = 2,
  Mutate            = 3,
  Split             = 4,
  // Variant of Acquire whose `range.start` is a placeholder the caller
  // has already reserved via `nt_pal::reserve_placeholder`. The plan
  // builder for this op hardcodes `reserve_placeholder_first = false`;
  // every other code path is shared with `Acquire`. Used by
  // `acquire_kernel_chosen` so the POSIX layer can let the kernel pick
  // a MEM_FREE base without a release / re-reserve race window.
  AcquireAtReserved = 5,
};

// Opaque input bundle forwarded from the public typed op into the per-arena
// dispatcher and the per-attempt envelope. All fields are read-only after
// construction.
struct CommitIntent {
  OpKind        op{OpKind::Acquire};
  VaRange       range{};
  RegionKind    kind{RegionKind::AnonPrivate};
  AcquireMeta   meta{};
  DescMutator   mutator{nullptr};
  void *        mutator_ctx{nullptr};
  DWORD         prot_change{0};
  void *        boundary{nullptr};

  // Mutate-with-commit-on-uncommitted: when true, the envelope walks
  // each locked succ's intersection with `range` via RegionWalker
  // and dispatches per chunk (committed → protect, uncommitted +
  // accessible + MEM_MAPPED → commit_in_reservation, uncommitted +
  // accessible + MEM_PRIVATE → commit_replace[_numa]). Replaces the
  // post_swap_protect per-VAD nt_pal::protect with a per-chunk
  // dispatch. The clone phase does NOT update view_prot — the kernel
  // is the source of truth for current per-page protection, and
  // consumers that need it (fork replay) query MBI directly.
  bool          commit_if_uncommitted_accessible{false};

  // NUMA node hint for commit_replace_numa on the demand-map path.
  // `-1` selects unhinted commit_replace. Honoured only when
  // `commit_if_uncommitted_accessible == true`.
  int           numa_node{-1};
};

/// Kernel-program configuration for one envelope attempt.
///
/// `CommitPlan` is a flat, stack-resident bundle of mode tags plus at most
/// three named commit slots; per-locked-succ work (demote, clone, ownership
/// transfer, Stage 2) is iterated inline over `locked.succ` by the phase
/// drivers rather than materialised as a list on the plan. Nothing in the
/// plan scales with `locked.count`, so there is no `-E2BIG` failure mode
/// for valid POSIX ranges.
///
/// Mode tags drive per-succ iteration:
///   * `demote_mode` — `AutoFromShape` dispatches on
///     `RegionDesc::current_shape()` per locked succ.
///   * `clone_mode` — `AllSuccsWithMutator` (mutate path), `SplitAtBoundary`
///     (split path, one-shot), `OutsideSurvivorRebind` (replace path,
///     rebind outside-intent survivors to new sibling backings).
///   * `ownership_mode` — `AllSuccs` zeroes each OLD backing's
///     `placeholder_base` after Swap (replace path).
///
/// Stage 2's per-succ loop similarly absorbs duplicates: the first iteration
/// on a shared backing wins the `state` CAS and performs synchronous
/// teardown; every subsequent iteration observes `state == Killed` and
/// short-circuits in O(1). No candidates list, no dedup pass, no O(N²).
///
/// The commit phase is the only piece that names kernel-side targets. The
/// replace topology is structural — at most one inside-range commit plus
/// at most one left-edge sibling re-map plus at most one right-edge sibling
/// re-map (interior succs cannot have backings extending past edges by VA
/// contiguity). Every commit slot is a *named* field; there is no array,
/// no count, no `kMaxCommits`.

enum class CommitKind : uint8_t {
  // Anonymous private placeholder → committed via `nt_pal::commit_replace`.
  // No `MEM_WRITE_WATCH` — kernel-native sub-range release relies on the
  // VAD being non-WW (see `nt_pal/placeholder.h` policy comment).
  CommitReplace,
  // Section view of `section` at `section_offset` over the placeholder;
  // `nt_pal::map_section_replace`.
  MapSectionReplace,
};

// Configuration for one commit slot. The plan carries at most three of
// these — `inside_commit`, `left_sibling`, `right_sibling` — each a named
// field, not an array element.
struct CommitOp {
  CommitKind kind;
  // When true, issue `nt_pal::reserve_placeholder` over
  // `(reserve_base, reserve_bytes)` before the commit. Used by `acquire`
  // (entering MEM_FREE → RESERVE → COMMIT); replace skips this because
  // demote+coalesce already established the placeholder.
  bool reserve_placeholder_first;
  void *reserve_base;
  size_t reserve_bytes;
  // Kernel commit target.
  void *base;
  size_t bytes;
  DWORD prot;
  // Section parameters (`kind == MapSectionReplace`).
  HANDLE section;
  LARGE_INTEGER section_offset;
  // New backing's placeholder identity. May be wider than (base, bytes)
  // when the caller pre-reserves more (brk's 256 MiB single-shot,
  // `posix_memalign` headroom).
  void *placeholder_identity_base;
  uint32_t placeholder_identity_pages;
  BackingShape backing_shape;
  HANDLE section_handle_for_backing;
  HANDLE file_handle_for_backing;
  // Per-desc fields written into the freshly built `RegionDesc`.
  RegionKind kind_for_desc;
  DWORD view_prot_for_desc;
  uint16_t flags_for_desc;
  uint64_t section_offset_for_desc;
  // New node VA range bound to this commit's desc. For acquire and the
  // simple inside_commit slot this is (base, base+bytes).
  uintptr_t new_node_lo;
  uintptr_t new_node_hi;
};

/// Replace's outside-range survivor sibling slot.
///
/// When a backing in the locked set extends past either edge of
/// `intent.range` to a node OUTSIDE the original locked range, the
/// envelope's preflight widens the lock to include the survivor and the
/// plan grows a sibling slot capturing OLD identity (shape, handle,
/// section offset at the new sibling's lo, uniform per-desc fields). The
/// commit phase emits a sibling re-map: for section views, a fresh
/// `nt_pal::map_section_replace` against the OLD section handle at the
/// surviving slice; for private commits, pure metadata because the inside-
/// only demote leaves outside slices committed.
///
/// Per-fragment kernel work is O(1) — at most one `split_placeholder` per
/// edge (section view only) plus at most one `map_section_replace` per
/// edge — independent of how many outside-range survivor descs share the
/// wider backing.
struct SiblingReMapSlot {
  bool present{false};
  // Surviving sibling slice in VA. Left: `[lo, intent.range.lo)`. Right:
  // `[intent.range.hi, hi)`.
  uintptr_t lo{0};
  uintptr_t hi{0};
  // OLD backing identity captured under LOCKED hold (stable for the
  // envelope's lifetime). The new sibling backing carries
  // `section_handle = nullptr`; the OLD wider backing keeps both handles
  // and Stage 2 closes them on its own teardown.
  BackingShape old_shape{BackingShape::PrivateCommit};
  HANDLE old_section_handle{nullptr};
  // Section offset at the new sibling slice's `lo`, used as the
  // `SectionOffset` argument to `NtMapViewOfSectionEx`. Computed in
  // `build_plan_replace` from the OLD desc whose `lo` equals the OLD
  // backing's `placeholder_base` (the section view's original base
  // offset, before the now-demoted unmap).
  LARGE_INTEGER old_section_offset_at_lo{};
  DWORD old_prot{0};
  // Per-desc fields for the rebound clones. All survivor descs sharing
  // the OLD backing must carry uniform prot / flags / kind;
  // `build_plan_replace` rejects the heterogeneous case with `-ENOTSUP`
  // because multi-prot sibling re-map needs multiple views per edge.
  RegionKind kind_for_desc{RegionKind::AnonPrivate};
  DWORD view_prot_for_desc{0};
  uint16_t flags_for_desc{0};
};

// Plan struct populated by the per-op builders and consumed by `execute_plan`
// + post-Swap drivers + Stage 2. See the `CommitPlan` docblock above for
// the role of each field group.
struct CommitPlan {
  Arena *arena{nullptr};
  VaRange range{};

  // Demote dispatch tag.
  enum class DemoteMode : uint8_t { None, AutoFromShape };
  DemoteMode demote_mode{DemoteMode::None};

  // Coalesce span (replace path, when ≥2 distinct placeholders survive
  // post-demote inside `range`).
  bool coalesce_needed{false};
  void *coalesce_base{nullptr};
  size_t coalesce_bytes{0};

  // Inside-range commit. Filled by acquire and replace; disabled for
  // mutate / split / release.
  bool has_inside_commit{false};
  CommitOp inside_commit{};

  // Sibling re-map slots. Replace populates these when a backing's extent
  // extends past `intent.range`'s edges; other ops never use them.
  SiblingReMapSlot left_sibling{};
  SiblingReMapSlot right_sibling{};

  // Clone-phase dispatch.
  enum class CloneMode : uint8_t {
    None,
    AllSuccsWithMutator,    // mutate: clone + run mutator + append.
    SplitAtBoundary,        // split: two clones from the single locked succ.
    OutsideSurvivorRebind,  // replace: rebind outside-range survivors to
                            // new sibling backings.
  };
  CloneMode clone_mode{CloneMode::None};
  uintptr_t split_boundary{0};
  DescMutator mutator{nullptr};
  void *mutator_ctx{nullptr};

  // Post-Swap kernel-side protect dispatch.
  bool issue_protect{false};
  DWORD protect_value{0};

  // Per-chunk dispatch instead of post_swap_protect's per-VAD call.
  // When set, the post-Swap phase walks each locked succ's
  // intersection with `range` via `nt_pal::RegionWalker` and dispatches
  // per chunk: committed → `nt_pal::protect(prot_value)`,
  // uncommitted+accessible+MEM_MAPPED →
  // `nt_pal::commit_in_reservation_no_writewatch`, uncommitted+
  // accessible+MEM_PRIVATE → `nt_pal::commit_replace[_numa]` using
  // `numa_node`, uncommitted+PROT_NONE → no-op. `MEM_FREE` aborts
  // with `-ENOMEM` (POSIX mprotect contract). COW translation is
  // applied per-succ using the OLD desc's `region_flag::COW`.
  bool dispatch_per_chunk{false};
  int  numa_node{-1};

  // Post-Swap OLD backing ownership transfer.
  enum class OwnershipMode : uint8_t { None, AllSuccs };
  OwnershipMode ownership_mode{OwnershipMode::None};

  // Release-path kernel program flag. When set, `execute_plan` punches
  // each inside-intent locked desc's slice out of its placeholder-
  // replaced VAD with plain `MEM_RELEASE`. NT's origin-preserving split
  // keeps any outside-range survivor slices at their existing commit
  // and placeholder-origin bit; the released slice becomes MEM_FREE.
  // Outside-range survivor descs are rebound onto fresh sibling
  // backings by `CloneMode::OutsideSurvivorRebind` in the same
  // envelope, so the OLD backing has no LIVE referencer by the time
  // Stage 2 runs.
  bool release_inside_slice{false};
};

//===----------------------------------------------------------------------===//
//  Per-op plan builders
//===----------------------------------------------------------------------===//
//
// Each runs under LOCKED hold, inspects the locked succ set, and fills the
// supplied `CommitPlan`. Returns 0 on success or a negative errno on
// precondition failure — the engine surfaces the error before any kernel
// work runs.

// Acquire over a fresh VA range. No locked succs are required (the range
// was MEM_FREE before this call). The single `inside_commit` slot is
// populated with the caller's kind / handles / offset and (for the
// `ReserveFirst == true` instantiation) a pre-commit
// `nt_pal::reserve_placeholder` to establish the VA in RESERVE state.
//
// The `ReserveFirst` template parameter is the only thing that varies
// between `Acquire` and `AcquireAtReserved`; templating compile-time-
// hardcodes the reserve flag on each specialization so neither public
// path pays a runtime test. The body is otherwise identical, and the
// specialisations are emitted only at the two callsites in
// `build_plan_acquire` / `build_plan_acquire_at_reserved`.
template <bool ReserveFirst>
LIBC_INLINE int build_acquire_plan_body(const CommitIntent &i, Arena *arena,
                                         CommitPlan &plan) {
  plan.arena = arena;
  plan.range = i.range;

  void *ph_base = i.meta.placeholder_base != nullptr
                      ? i.meta.placeholder_base
                      : reinterpret_cast<void *>(i.range.lo());
  // Two cases for the reserve geometry:
  //   * Explicit identity override (brk's 256 MiB single-shot,
  //     `posix_memalign` alignment headroom, mremap grow-into-headroom):
  //     `meta.placeholder_size` is the full reservation extent and must
  //     be alloc-granularity-aligned. The caller's `range.bytes` is the
  //     narrow registered slice inside that extent; the commit step
  //     covers only the slice. No envelope-side shrink runs because
  //     the caller explicitly sized the reservation.
  //   * Default (`meta.placeholder_size == 0`): the reservation matches
  //     `range.bytes`, rounded up to alloc granularity for NT's
  //     `MEM_RESERVE_PLACEHOLDER` placement constraint. The envelope's
  //     NT phase shrinks the rounded-up placeholder down to `bytes`
  //     (split + pad release) so `commit_replace` sees an exact-sized
  //     placeholder.
  size_t identity_bytes;
  size_t reserve_bytes;
  if (i.meta.placeholder_size != 0) {
    if ((i.meta.placeholder_size & (kAllocGranularity - 1)) != 0)
      return -EINVAL;
    identity_bytes = i.meta.placeholder_size;
    reserve_bytes = i.meta.placeholder_size;
  } else {
    identity_bytes = i.range.bytes;
    reserve_bytes = (i.range.bytes + (kAllocGranularity - 1)) &
                    ~(kAllocGranularity - 1);
  }
  uint32_t ph_pages =
      static_cast<uint32_t>(identity_bytes / kPageGranularity);

  CommitOp &op = plan.inside_commit;
  op.kind = kind_is_section_backed(i.kind) ? CommitKind::MapSectionReplace
                                            : CommitKind::CommitReplace;
  op.reserve_placeholder_first = ReserveFirst;
  op.reserve_base = ph_base;
  op.reserve_bytes = reserve_bytes;
  op.base = reinterpret_cast<void *>(i.range.lo());
  op.bytes = i.range.bytes;
  op.prot = i.meta.view_prot;
  op.section = i.meta.section_handle;
  op.section_offset.QuadPart = static_cast<int64_t>(i.meta.section_offset);
  op.placeholder_identity_base = ph_base;
  op.placeholder_identity_pages = ph_pages;
  op.backing_shape = kind_is_section_backed(i.kind)
                         ? BackingShape::SectionView
                         : BackingShape::PrivateCommit;
  op.section_handle_for_backing = i.meta.section_handle;
  op.file_handle_for_backing = i.meta.file_handle;
  op.kind_for_desc = i.kind;
  op.view_prot_for_desc = i.meta.view_prot;
  op.flags_for_desc = i.meta.flags;
  op.section_offset_for_desc = i.meta.section_offset;
  op.new_node_lo = i.range.lo();
  op.new_node_hi = i.range.hi();
  plan.has_inside_commit = true;
  return 0;
}

int build_plan_acquire(const CommitIntent &i, Arena *arena,
                        const LockedSet & /*locked*/, CommitPlan &plan) {
  return build_acquire_plan_body</*ReserveFirst=*/true>(i, arena, plan);
}

// Variant for `OpKind::AcquireAtReserved`: the caller has already
// reserved the placeholder at `range.start`, so the envelope skips
// `nt_pal::reserve_placeholder` and goes straight to commit_replace.
// Failure cleanup is the caller's responsibility; on success the
// placeholder identity transfers to the new backing's lifetime.
int build_plan_acquire_at_reserved(const CommitIntent &i, Arena *arena,
                                    const LockedSet & /*locked*/,
                                    CommitPlan &plan) {
  return build_acquire_plan_body</*ReserveFirst=*/false>(i, arena, plan);
}

// Locate the desc in the locked set whose `lo == target_lo` and return its
// value. Used to recover an OLD desc's section_offset / view_prot / kind /
// flags at a specific VA — the leftmost backing's b_lo for the left-sibling
// slot, the desc-with-extension's b_lo for the right.
[[nodiscard]] LIBC_INLINE RegionDesc *
locked_find_desc_at_lo(const LockedSet &locked, uintptr_t target_lo) {
  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *n = locked.at(k);
    if (n == nullptr || n->lo != target_lo)
      continue;
    return n->value.load(cpp::MemoryOrder::ACQUIRE);
  }
  return nullptr;
}

// Validate that every locked desc sharing `target_b` carries uniform
// view_prot / flags / shape. Heterogeneous (mprotect / mbind / etc. on a
// sub-clone) needs multi-prot sibling re-maps, which the current plan
// layout does not encode — return false and let the caller emit
// `-ENOTSUP`.
[[nodiscard]] LIBC_INLINE bool
locked_uniform_for_backing(const LockedSet &locked, DescBacking *target_b,
                            DWORD &out_prot, uint16_t &out_flags,
                            uint16_t &out_shape_word) {
  bool found = false;
  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *n = locked.at(k);
    if (n == nullptr)
      continue;
    RegionDesc *d = n->value.load(cpp::MemoryOrder::ACQUIRE);
    if (d == nullptr || d->backing_ref == kBackingRefNull)
      continue;
    if (deref_backing_raw(d->backing_ref) != target_b)
      continue;
    DWORD prot = d->view_prot;
    uint16_t flags = d->flags.load(cpp::MemoryOrder::ACQUIRE);
    uint16_t shape_word = d->shape.load(cpp::MemoryOrder::ACQUIRE);
    if (!found) {
      out_prot = prot;
      out_flags = flags;
      out_shape_word = shape_word;
      found = true;
      continue;
    }
    if (prot != out_prot || flags != out_flags || shape_word != out_shape_word)
      return false;
  }
  return found;
}

// Reverse-map a `RegionShape` value to a representative `RegionKind`. Only
// shape is stored on the desc; the original `RegionKind` is lossy under
// section-backed kinds (FILE_VIEW_MONO is shared by FilePrivate,
// FileShared, ShmPosix, ShmSysV), so the sibling rebind reports the most
// likely match. Callers who depend on exact kind preservation should avoid
// partial replace of shared-backing regions across disparate kinds.
[[nodiscard]] LIBC_INLINE RegionKind kind_from_shape(uint16_t shape_word) {
  RegionShape s = static_cast<RegionShape>(shape_word);
  switch (s) {
  case RegionShape::FILE_VIEW_MONO:
  case RegionShape::FILE_VIEW_CHUNKED:
  case RegionShape::FILE_VIEW_RESERVE:
    return RegionKind::FilePrivate;
  case RegionShape::ANON_RESERVE_SECTION:
    return RegionKind::AnonShared;
  case RegionShape::ANON_PLACEHOLDER:
    return RegionKind::AnonPrivate;
  default:
    return RegionKind::AnonPrivate;
  }
}

// Release the descs covering `intent.range`.
//
// Two code paths fold into one plan shape:
//
//   * Non-shared release (no backing extends past either edge of
//     `intent.range`): no sibling slots, no clone phase, just the
//     inside-slice kernel-release flag. Stage 2 then runs the unguarded
//     Live → Killed CAS on each locked OLD backing and tears it down.
//
//   * Shared-backing release: mirrors replace's plan shape exactly —
//     populate `left_sibling` / `right_sibling` from any edge-extending
//     backing's OLD identity, set `clone_mode = OutsideSurvivorRebind`,
//     and let `execute_plan` allocate fresh sibling DescBackings and
//     rebind outside-range survivor descs onto them. The inside-slice
//     kernel release punches the inside slice out of the OLD VAD with
//     plain `MEM_RELEASE`; NT's origin-preserving split keeps the
//     outside slices at their existing commit and placeholder-origin
//     bit while the inside slice becomes MEM_FREE, and the new sibling
//     backings record those outside slices' placeholder identity for
//     their own eventual kill. The OLD backing, having no LIVE
//     referencer post-Swap and a nulled `placeholder_base` from
//     `post_swap_ownership_transfer`, is freed metadata-only by Stage 2.
//
// Auto-split for edge-straddlers: a locked desc whose extent crosses
// an intent edge is no longer rejected. The clone phase emits a
// survivor clone covering only the outside-intent portion, and the
// demote phase clips its kernel work to the inside-intent portion.
// The OLD backing's edge-extending detection (left_b / right_b) is
// driven by backing extent and so captures straddlers automatically:
// the straddler desc's backing extent equals or exceeds the desc
// extent, and crosses the intent edge by construction. Closes the
// posix-compliance race window where a concurrent peer mutation
// between a caller's `split()` and this op turned a benign no-op into
// a phantom EINVAL.
int build_plan_release(const CommitIntent &i, Arena *arena,
                        const LockedSet &locked, CommitPlan &plan) {
  plan.arena = arena;
  plan.range = i.range;
  plan.release_inside_slice = true;

  // Identify the leftmost and rightmost backings whose extent extends
  // past `intent.range`. By VA contiguity there is at most one of each.
  DescBacking *left_b = nullptr;
  DescBacking *right_b = nullptr;
  uintptr_t left_b_lo = 0;
  uintptr_t right_b_hi = 0;

  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *n = locked.at(k);
    if (n == nullptr)
      continue;
    RegionDesc *d = n->value.load(cpp::MemoryOrder::ACQUIRE);
    if (d == nullptr || d->backing_ref == kBackingRefNull)
      continue;
    DescBacking *b = deref_backing_raw(d->backing_ref);
    if (b == nullptr)
      continue;
    void *ph_base = b->placeholder_base.load(cpp::MemoryOrder::ACQUIRE);
    if (ph_base == nullptr)
      continue;
    uintptr_t b_lo = reinterpret_cast<uintptr_t>(ph_base);
    uintptr_t b_hi = b_lo + static_cast<uintptr_t>(b->placeholder_pages) *
                                kPageGranularity;
    if (b_lo < i.range.lo()) {
      if (left_b != nullptr && left_b != b)
        return -ENOTSUP;
      left_b = b;
      left_b_lo = b_lo;
    }
    if (b_hi > i.range.hi()) {
      if (right_b != nullptr && right_b != b)
        return -ENOTSUP;
      right_b = b;
      right_b_hi = b_hi;
    }
  }

  // No shared-backing extension: pure metadata release. Stage 2 will
  // race the Live → Killed CAS on each OLD backing and tear it down.
  if (left_b == nullptr && right_b == nullptr)
    return 0;

  // Sibling slots: capture OLD identity (shape, handle,
  // section_offset_at_lo, prot) for sibling-backing creation in the
  // commit phase and rebound clones in the clone phase. Heterogeneous
  // descs sharing one backing need multi-prot views per edge; refuse
  // with -ENOTSUP because the plan layout doesn't encode that.
  if (left_b != nullptr) {
    DWORD prot = 0;
    uint16_t flags = 0;
    uint16_t shape_word = 0;
    if (!locked_uniform_for_backing(locked, left_b, prot, flags, shape_word))
      return -ENOTSUP;
    RegionDesc *anchor = locked_find_desc_at_lo(locked, left_b_lo);
    if (anchor == nullptr)
      return -ENOTSUP;
    plan.left_sibling.present = true;
    plan.left_sibling.lo = left_b_lo;
    plan.left_sibling.hi = i.range.lo();
    plan.left_sibling.old_shape = left_b->shape;
    plan.left_sibling.old_section_handle =
        left_b->section_handle.load(cpp::MemoryOrder::ACQUIRE);
    plan.left_sibling.old_section_offset_at_lo = anchor->section_offset;
    plan.left_sibling.old_prot = prot;
    plan.left_sibling.kind_for_desc = kind_from_shape(shape_word);
    plan.left_sibling.view_prot_for_desc = prot;
    plan.left_sibling.flags_for_desc = flags;
  }
  if (right_b != nullptr) {
    DWORD prot = 0;
    uint16_t flags = 0;
    uint16_t shape_word = 0;
    if (!locked_uniform_for_backing(locked, right_b, prot, flags, shape_word))
      return -ENOTSUP;
    void *r_ph_base =
        right_b->placeholder_base.load(cpp::MemoryOrder::ACQUIRE);
    uintptr_t right_b_lo = reinterpret_cast<uintptr_t>(r_ph_base);
    RegionDesc *anchor = locked_find_desc_at_lo(locked, right_b_lo);
    if (anchor == nullptr)
      return -ENOTSUP;
    plan.right_sibling.present = true;
    plan.right_sibling.lo = i.range.hi();
    plan.right_sibling.hi = right_b_hi;
    plan.right_sibling.old_shape = right_b->shape;
    plan.right_sibling.old_section_handle =
        right_b->section_handle.load(cpp::MemoryOrder::ACQUIRE);
    LARGE_INTEGER off = anchor->section_offset;
    off.QuadPart += static_cast<int64_t>(i.range.hi() - right_b_lo);
    plan.right_sibling.old_section_offset_at_lo = off;
    plan.right_sibling.old_prot = prot;
    plan.right_sibling.kind_for_desc = kind_from_shape(shape_word);
    plan.right_sibling.view_prot_for_desc = prot;
    plan.right_sibling.flags_for_desc = flags;
  }

  // Rebind outside-range survivors onto the new sibling backings.
  plan.clone_mode = CommitPlan::CloneMode::OutsideSurvivorRebind;

  // Ownership transfer: nullify OLD backings' `placeholder_base` so
  // Stage 2's `backing_kill_and_retire` skips `free_placeholder` for
  // the inside slice's VA — that VA was already released by the
  // inside-slice kernel program above (or, for outside-edge OLD
  // backings, is now owned by the new sibling backings). Section /
  // file handles stay set and Stage 2 closes them.
  plan.ownership_mode = CommitPlan::OwnershipMode::AllSuccs;
  return 0;
}

// Replace a VA range with new kernel state, handling shared backings that
// extend past either edge.
//
// `run_envelope`'s preflight has already widened the locked range to cover
// the full extent of every shared backing (one iteration suffices by VA
// contiguity), so every outside-range survivor is in the locked set when
// this builder runs. The builder identifies the leftmost / rightmost
// edge-extending backings (at most one of each by VA contiguity), captures
// the OLD identity needed for sibling re-map, and populates `inside_commit`
// plus optional sibling slots.
//
// Heterogeneous prot / flags / kind across descs sharing one OLD backing
// returns `-ENOTSUP`: multi-prot sibling re-map needs multiple views per
// edge, a shape the plan layout does not encode.

int build_plan_replace(const CommitIntent &i, Arena *arena,
                        const LockedSet &locked, CommitPlan &plan) {
  // Auto-split for edge-straddlers at the intent edges: see
  // `build_plan_release` for the rationale. The clone phase emits
  // survivor clones for the outside-intent portion of each straddler;
  // the demote phase clips its kernel work to the inside portion.
  //
  // The EXTENDED-lock-edge straddler guard remains: if a desc straddles
  // the union of all preflight-expanded backings' extents, the locked
  // set no longer converges by VA contiguity. That case still requires
  // the caller to pre-split the abutting chain (rare; raised as
  // -ENOTSUP from the substrate).
  if (locked_has_edge_straddler(locked, locked.lo, locked.hi))
    return -ENOTSUP;

  // Identify the leftmost and rightmost backings whose extent extends past
  // `intent.range`. By VA contiguity there is at most one of each.
  DescBacking *left_b = nullptr;
  DescBacking *right_b = nullptr;
  uintptr_t left_b_lo = 0;
  uintptr_t right_b_hi = 0;

  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *n = locked.at(k);
    if (n == nullptr)
      continue;
    RegionDesc *d = n->value.load(cpp::MemoryOrder::ACQUIRE);
    if (d == nullptr || d->backing_ref == kBackingRefNull)
      continue;
    DescBacking *b = deref_backing_raw(d->backing_ref);
    if (b == nullptr)
      continue;
    void *ph_base = b->placeholder_base.load(cpp::MemoryOrder::ACQUIRE);
    if (ph_base == nullptr)
      continue;
    uintptr_t b_lo = reinterpret_cast<uintptr_t>(ph_base);
    uintptr_t b_hi = b_lo + static_cast<uintptr_t>(b->placeholder_pages) *
                                kPageGranularity;
    if (b_lo < i.range.lo()) {
      // Multiple distinct left-edge-extending backings would imply a
      // chain discontinuity. Refuse rather than try to interpret it.
      if (left_b != nullptr && left_b != b)
        return -ENOTSUP;
      left_b = b;
      left_b_lo = b_lo;
    }
    if (b_hi > i.range.hi()) {
      if (right_b != nullptr && right_b != b)
        return -ENOTSUP;
      right_b = b;
      right_b_hi = b_hi;
    }
  }

  plan.arena = arena;
  plan.range = i.range;

  // Demote dispatches per locked succ by shape: section views unmap whole
  // (idempotent across shared-backing succs; data preserved on the
  // section's storage and re-mapped by the sibling slot below); private
  // commits demote only the inside-intent succs (sibling slices stay
  // committed — decommit would lose their data).
  plan.demote_mode = CommitPlan::DemoteMode::AutoFromShape;

  // Coalesce span — required whenever the post-demote VA inside the
  // intent contains more than one placeholder VAD. Two sources of
  // multi-VAD topology:
  //
  //   * Gap-fill: a MEM_FREE region inside the intent is filled with a
  //     fresh placeholder by `execute_plan`, distinct from the
  //     demoted-succ placeholder. Detected here by the "single succ
  //     does not fully tile intent" condition (any inside-VA the
  //     succ doesn't cover is gap-fill territory).
  //
  //   * Multi-desc demote: each locked succ (private-commit) demotes
  //     to its own placeholder VAD, even when the succs tile intent
  //     without MEM_FREE gaps. Section-view succs sharing a wider
  //     backing dedupe to a single whole-view unmap, but private
  //     commits don't — every locked succ contributes its own VAD.
  //
  // Both collapse into one cheap test: skip coalesce ONLY when
  // `locked.count == 1` AND that single succ's extent fully covers
  // `[intent.lo, intent.hi)` (so no gap-fill fires AND no second
  // VAD exists). All other shapes — multiple succs, partial coverage,
  // pure MEM_FREE intent — need coalesce to merge into the single
  // contiguous placeholder that `commit_replace`'s exact-match
  // constraint requires.
  bool needs_coalesce = true;
  if (locked.count == 1) {
    SkiplistNodeBase *only = locked.at(0);
    if (only != nullptr && only->lo <= i.range.lo() &&
        only->hi >= i.range.hi())
      needs_coalesce = false;
  }
  plan.coalesce_needed = needs_coalesce;
  plan.coalesce_base = reinterpret_cast<void *>(i.range.lo());
  plan.coalesce_bytes = i.range.bytes;

  // Inside-range commit: the new backing covers `intent.range` with the
  // caller-supplied kind / handles / offset.
  CommitOp &op = plan.inside_commit;
  op.kind = kind_is_section_backed(i.kind) ? CommitKind::MapSectionReplace
                                            : CommitKind::CommitReplace;
  op.reserve_placeholder_first = false;
  op.base = reinterpret_cast<void *>(i.range.lo());
  op.bytes = i.range.bytes;
  op.prot = i.meta.view_prot;
  op.section = i.meta.section_handle;
  op.section_offset.QuadPart = static_cast<int64_t>(i.meta.section_offset);
  op.placeholder_identity_base = reinterpret_cast<void *>(i.range.lo());
  op.placeholder_identity_pages =
      static_cast<uint32_t>(i.range.bytes / kPageGranularity);
  op.backing_shape = kind_is_section_backed(i.kind)
                         ? BackingShape::SectionView
                         : BackingShape::PrivateCommit;
  op.section_handle_for_backing = i.meta.section_handle;
  op.file_handle_for_backing = i.meta.file_handle;
  op.kind_for_desc = i.kind;
  op.view_prot_for_desc = i.meta.view_prot;
  op.flags_for_desc = i.meta.flags;
  op.section_offset_for_desc = i.meta.section_offset;
  op.new_node_lo = i.range.lo();
  op.new_node_hi = i.range.hi();
  plan.has_inside_commit = true;

  // Sibling slots: for each edge-extending backing, capture OLD identity
  // (shape, handle, section_offset_at_lo, prot) for the commit-phase
  // re-map and the clone-phase rebound clones.
  if (left_b != nullptr) {
    DWORD prot = 0;
    uint16_t flags = 0;
    uint16_t shape_word = 0;
    // Heterogeneous descs sharing one backing need multi-prot views per
    // edge; the plan layout does not encode them.
    if (!locked_uniform_for_backing(locked, left_b, prot, flags, shape_word))
      return -ENOTSUP;
    // Preflight expansion guarantees the leftmost desc of left_b is in
    // the locked set; absence here is structural inconsistency.
    RegionDesc *anchor = locked_find_desc_at_lo(locked, left_b_lo);
    if (anchor == nullptr)
      return -ENOTSUP;
    plan.left_sibling.present = true;
    plan.left_sibling.lo = left_b_lo;
    plan.left_sibling.hi = i.range.lo();
    plan.left_sibling.old_shape = left_b->shape;
    plan.left_sibling.old_section_handle =
        left_b->section_handle.load(cpp::MemoryOrder::ACQUIRE);
    plan.left_sibling.old_section_offset_at_lo = anchor->section_offset;
    plan.left_sibling.old_prot = prot;
    plan.left_sibling.kind_for_desc = kind_from_shape(shape_word);
    plan.left_sibling.view_prot_for_desc = prot;
    plan.left_sibling.flags_for_desc = flags;
  }
  if (right_b != nullptr) {
    DWORD prot = 0;
    uint16_t flags = 0;
    uint16_t shape_word = 0;
    if (!locked_uniform_for_backing(locked, right_b, prot, flags, shape_word))
      return -ENOTSUP;
    void *r_ph_base =
        right_b->placeholder_base.load(cpp::MemoryOrder::ACQUIRE);
    uintptr_t right_b_lo = reinterpret_cast<uintptr_t>(r_ph_base);
    RegionDesc *anchor = locked_find_desc_at_lo(locked, right_b_lo);
    if (anchor == nullptr)
      return -ENOTSUP;
    plan.right_sibling.present = true;
    plan.right_sibling.lo = i.range.hi();
    plan.right_sibling.hi = right_b_hi;
    plan.right_sibling.old_shape = right_b->shape;
    plan.right_sibling.old_section_handle =
        right_b->section_handle.load(cpp::MemoryOrder::ACQUIRE);
    // Section offset at the new sibling's lo (= `intent.range.hi`):
    //   sibling_offset = anchor's section_offset_at_right_b_lo
    //                  + (intent.range.hi - right_b_lo)
    LARGE_INTEGER off = anchor->section_offset;
    off.QuadPart += static_cast<int64_t>(i.range.hi() - right_b_lo);
    plan.right_sibling.old_section_offset_at_lo = off;
    plan.right_sibling.old_prot = prot;
    plan.right_sibling.kind_for_desc = kind_from_shape(shape_word);
    plan.right_sibling.view_prot_for_desc = prot;
    plan.right_sibling.flags_for_desc = flags;
  }

  // Rebind outside-range survivors when any sibling exists.
  if (plan.left_sibling.present || plan.right_sibling.present)
    plan.clone_mode = CommitPlan::CloneMode::OutsideSurvivorRebind;

  // Replace always transfers ownership: every OLD desc's backing has its
  // placeholder identity consumed by either the inside commit or a sibling
  // re-map. Stage 2 closes the OLD section/file handles; the new sibling
  // backings keep their views alive via the kernel's view-section internal
  // reference independent of the OLD handle close.
  plan.ownership_mode = CommitPlan::OwnershipMode::AllSuccs;
  return 0;
}

// Apply a `DescMutator` to clones of every locked desc, optionally with a
// kernel-side `NtProtectVirtualMemory` per VAD scoped to the intent
// range. POSIX `mprotect(addr, len, prot)` only touches `[addr, addr+
// len)` — edge-straddler desc fragmentation happens atomically inside
// the envelope's clone phase: the source desc is split into up to three
// clones (left-outside, inside-mutated, right-outside), with the
// mutator applied only to the inside slice. Per-VAD `NtProtect` in the
// post-Swap phase is clipped to the inside slice for the same reason.
int build_plan_mutate(const CommitIntent &i, Arena *arena,
                       const LockedSet &locked, CommitPlan &plan) {
  if (locked.count == 0)
    return -ENOENT;

  plan.arena = arena;
  plan.range = i.range;

  plan.clone_mode = CommitPlan::CloneMode::AllSuccsWithMutator;
  plan.mutator = i.mutator;
  plan.mutator_ctx = i.mutator_ctx;

  // Per-chunk dispatch path: the slow-path mprotect equivalent. The
  // post-Swap phase walks the kernel VAD chain per locked succ and
  // dispatches each chunk (committed → `nt_pal::protect`;
  // uncommitted + accessible → commit_in_reservation or
  // commit_replace[_numa]). The clone phase intentionally does NOT
  // write `view_prot` on the new clones — the kernel is the source
  // of truth for current per-page protection, so the clone carries
  // forward the acquire-time intent unchanged. Replaces the standard
  // post_swap_protect — `issue_protect` stays false.
  if (i.commit_if_uncommitted_accessible) {
    plan.dispatch_per_chunk = true;
    plan.numa_node = i.numa_node;
    plan.protect_value = i.prot_change;
    return 0;
  }

  // `NtProtectVirtualMemory` is per-VAD atomic; a range spanning multiple
  // split placeholders needs per-VAD calls iterated in the post-Swap
  // protect phase.
  if (i.prot_change != 0) {
    plan.issue_protect = true;
    plan.protect_value = i.prot_change;
  }
  return 0;
}

// Split the single locked desc at `boundary` into two clones. The single-
// succ precondition is the contract that lets the public `split()` op
// guarantee its boundary lands strictly inside one captured desc.
int build_plan_split(const CommitIntent &i, Arena *arena,
                      const LockedSet &locked, CommitPlan &plan) {
  if (locked.count != 1)
    return -ENOENT;
  uintptr_t boundary = reinterpret_cast<uintptr_t>(i.boundary);
  SkiplistNodeBase *old_node = locked.at(0);
  if (boundary <= old_node->lo || boundary >= old_node->hi)
    return -EINVAL;

  plan.arena = arena;
  plan.range = i.range;
  plan.clone_mode = CommitPlan::CloneMode::SplitAtBoundary;
  plan.split_boundary = boundary;
  return 0;
}

//===----------------------------------------------------------------------===//
//  Kernel commit phase — execute_plan
//===----------------------------------------------------------------------===//

// Issue the kernel commit named by `op`. `nt_pal::commit_replace` performs
// `NtAllocateVirtualMemoryEx(MEM_COMMIT | MEM_REPLACE_PLACEHOLDER |
// MEM_WRITE_WATCH)`; `nt_pal::map_section_replace` performs
// `NtMapViewOfSectionEx(..., MEM_REPLACE_PLACEHOLDER)`. Either is atomic at
// the VAD level and fails on size mismatch — no partial state on failure.
NTSTATUS run_commit(const CommitOp &op) {
  switch (op.kind) {
  case CommitKind::CommitReplace:
    return nt_pal::commit_replace(op.base, op.bytes, op.prot);
  case CommitKind::MapSectionReplace:
    return nt_pal::map_section_replace(op.section, op.base, op.bytes,
                                        op.section_offset, op.prot);
  }
  __builtin_unreachable();
}

// True if `n` falls fully inside `intent`. The no-straddler precondition
// makes a single edge comparison sufficient — every locked succ is either
// fully inside or fully outside.
[[nodiscard]] LIBC_INLINE bool succ_is_inside_intent(const SkiplistNodeBase *n,
                                                      VaRange intent) {
  return n->lo >= intent.lo() && n->hi <= intent.hi();
}

/// Run the kernel program defined by `plan` under the LOCKED hold.
///
/// Walks the demote → split → coalesce → commit → clone sequence,
/// iterating `locked.succ` inline for per-succ phases and consulting the
/// plan's named commit slots for the structurally-bounded commit phase.
/// Returns 0 on success or a negative errno; on failure the caller
/// (`run_envelope`) walks `prov` for backing rollback and
/// `retire_unpublished_nodes` for desc rollback.
int execute_plan(CommitPlan &plan, const LockedSet &locked,
                  NewNodes &new_nodes, ProvisionalList &prov) {
  // Demote phase, shape-driven (replace path only).
  //
  // Section views: target `backing.placeholder_base` (= view base). The
  // unmap covers the WHOLE view including any sibling slices — safe
  // because the section's storage preserves the data; the sibling re-map
  // below restores the surviving slice's mapping. For shared backings,
  // every iteration targets the same view base; the first call demotes,
  // subsequent calls return `STATUS_NOT_MAPPED_VIEW`, which is benign
  // (the view is already placeholder) and tolerated.
  //
  // Private commits: ONLY the inside-intent succs demote. Outside-range
  // survivors stay committed — `decommit_preserve` would discard their
  // data (private commits have no backing storage). The clone phase's
  // `OutsideSurvivorRebind` picks up the survivor descs and remaps them
  // onto fresh sibling backings whose placeholder identity reflects the
  // surviving slice.
  if (plan.demote_mode == CommitPlan::DemoteMode::AutoFromShape) {
    for (uint32_t k = 0; k < locked.count; ++k) {
      SkiplistNodeBase *old_node = locked.at(k);
      if (old_node == nullptr)
        continue;
      RegionDesc *d = old_node->value.load(cpp::MemoryOrder::ACQUIRE);
      if (d == nullptr)
        continue;
      RegionShape s = d->current_shape();
      NTSTATUS st;
      switch (s) {
      case RegionShape::FILE_VIEW_MONO:
      case RegionShape::FILE_VIEW_CHUNKED:
      case RegionShape::FILE_VIEW_RESERVE:
      case RegionShape::ANON_RESERVE_SECTION: {
        DescBacking *ob = d->backing_ref == kBackingRefNull
                              ? nullptr
                              : deref_backing_raw(d->backing_ref);
        if (ob == nullptr) {
          st = STATUS_SUCCESS;
          break;
        }
        void *view_base =
            ob->placeholder_base.load(cpp::MemoryOrder::ACQUIRE);
        if (view_base == nullptr) {
          st = STATUS_SUCCESS;
          break;
        }
        st = nt_pal::unmap_view_preserve_transient(view_base);
        if (st == STATUS_NOT_MAPPED_VIEW)
          st = STATUS_SUCCESS;
        break;
      }
      case RegionShape::ANON_PLACEHOLDER: {
        // Demote only the intersection of the locked node with the
        // intent range. Three cases collapse into one clip:
        //   * Fully outside (demote window empty) — leave commit alone
        //     so the rebound clone observes preserved data.
        //   * Fully inside — demote the entire node extent (matches the
        //     previous behaviour exactly when `succ_is_inside_intent`).
        //   * Edge-straddler — demote only the inside portion; the
        //     outside portion stays committed and is published as a
        //     fresh survivor desc in the clone phase.
        // Decommit-preserve is page-granular on NT and auto-splits the
        // backing VAD into committed-left, decommitted-middle,
        // committed-right pieces — the survivor sees its data
        // preserved across the envelope.
        const uintptr_t demote_lo =
            old_node->lo > plan.range.lo() ? old_node->lo : plan.range.lo();
        const uintptr_t demote_hi =
            old_node->hi < plan.range.hi() ? old_node->hi : plan.range.hi();
        if (demote_lo >= demote_hi) {
          st = STATUS_SUCCESS;
          break;
        }
        void *base = reinterpret_cast<void *>(demote_lo);
        size_t bytes = static_cast<size_t>(demote_hi - demote_lo);
        st = nt_pal::preserve_to_placeholder(base, bytes);
        break;
      }
      default:
        // Unrecognised shape — the plan-build precondition rejects the
        // foreign-shape case, so anything leftover here is a mapping the
        // engine isn't authorised to touch.
        return -ENOTSUP;
      }
      if (!NT_SUCCESS(st))
        return -EFAULT;
    }
  }

  // Fill MEM_FREE gaps inside `range` with fresh placeholders (replace
  // path only — acquire builds with demote_mode == None and skips this).
  //
  // The replace pipeline assumes `range` is fully covered by placeholder-
  // state VAs entering the coalesce step. Locked-set descs are demoted
  // above, but MEM_FREE sub-spans inside `range` (e.g. a prior partial
  // release that left a kernel-side hole) reach coalesce unchanged.
  // `coalesce_placeholders` itself tolerates MEM_FREE gaps in its input
  // range, but the subsequent `commit_replace` then fails with
  // STATUS_CONFLICTING_ADDRESSES because the FREE pages are not in
  // placeholder state when the coalesced VAD is replaced. Reserving a
  // fresh placeholder over each MEM_FREE gap turns the whole `range`
  // into a uniform placeholder span that coalesce can merge into a
  // single VAD that `commit_replace` consumes cleanly.
  //
  // Gap placeholders are tracked locally so a kernel failure downstream
  // can roll them back. On the success path the subsequent coalesce
  // consumes them all into a single VAD; no per-gap cleanup is needed.
  // No DescBacking is allocated for these — they are short-lived kernel
  // state owned by this stack frame until the coalesce subsumes them.
  if (plan.demote_mode == CommitPlan::DemoteMode::AutoFromShape) {
    struct GapResv {
      void *base;
      size_t bytes;
    };
    // Pathological cap; one envelope should not span more distinct
    // FREE gaps than this. Sustained breach signals a callsite that
    // ought to fragment the request rather than pile every hole into
    // a single replace.
    constexpr uint32_t kMaxGapResv = 16;
    GapResv gap_resvs[kMaxGapResv];
    uint32_t gap_count = 0;

    uintptr_t plan_lo = plan.range.lo();
    uintptr_t plan_end = plan.range.hi();
    nt_pal::RegionWalker walk(reinterpret_cast<void *>(plan_lo),
                              static_cast<SIZE_T>(plan_end - plan_lo));
    if (!walk) {
      for (uint32_t k = 0; k < gap_count; ++k)
        (void)nt_pal::free_placeholder(gap_resvs[k].base);
      return -EFAULT;
    }
    while (walk.next()) {
      if (walk.entry->State != MEM_FREE)
        continue;
      // RegionWalker clips `chunk` / `chunk_size` to the plan range,
      // so an MBI that extends below plan.range.lo() or above
      // plan.range.hi() is automatically narrowed to the FREE gap
      // inside our authority.
      void *resv =
          nt_pal::reserve_placeholder(walk.chunk, walk.chunk_size);
      if (resv == nullptr) {
        for (uint32_t k = 0; k < gap_count; ++k)
          (void)nt_pal::free_placeholder(gap_resvs[k].base);
        return -EFAULT;
      }
      if (gap_count >= kMaxGapResv) {
        // Pathological fragmentation — bail with rollback.
        (void)nt_pal::free_placeholder(resv);
        for (uint32_t k = 0; k < gap_count; ++k)
          (void)nt_pal::free_placeholder(gap_resvs[k].base);
        return -ENOMEM;
      }
      gap_resvs[gap_count++] = {resv, walk.chunk_size};
    }
  }

  // Release-path kernel program (no demote, no commit). For each
  // locked desc fully inside `intent.range`, punch the desc's slice
  // out of the placeholder-replaced VAD with plain `MEM_RELEASE` on
  // exactly the inside slice. NT's origin-preserving split keeps the
  // head and tail (the outside-range survivor slices) at their
  // existing commit state with full page content intact and
  // placeholder-origin bit propagated; the inside slice transitions
  // to MEM_FREE. One syscall per locked-inside desc; no MBI query,
  // no scratch allocation, no memcpy. Outside-range survivor descs
  // are rebound onto fresh sibling backings by the
  // `CloneMode::OutsideSurvivorRebind` clone phase below (which
  // `build_plan_release` configures whenever a sibling slot is
  // present), so the OLD backing has no LIVE referencer by the time
  // Stage 2 runs and the kill path is metadata-only.
  //
  // This relies on the inside slice's VAD being commit-replaced
  // without `MEM_WRITE_WATCH` — the kernel rejects every sub-range
  // form of `NtFreeVirtualMemory` on a WW-armed VAD with
  // `STATUS_FREE_VM_NOT_AT_BASE`. The libc's commit path therefore
  // routes through `nt_pal::commit_replace` (no WW) by default;
  // callers that explicitly need the dirty bitmap opt in via
  // `commit_replace_writewatch` and accept the whole-VAD-lifecycle
  // constraint that entails. See `nt_pal/placeholder.h` for the
  // policy statement.
  //
  // Section-view shapes need a more elaborate unmap + split + free
  // dance for the inside slice and are not yet wired here — those
  // shapes return `-ENOTSUP`. The fuzz test's current SUT only uses
  // AnonPrivate.
  if (plan.release_inside_slice) {
    for (uint32_t k = 0; k < locked.count; ++k) {
      SkiplistNodeBase *old_node = locked.at(k);
      if (old_node == nullptr)
        continue;
      if (!succ_is_inside_intent(old_node, plan.range))
        continue;
      RegionDesc *d = old_node->value.load(cpp::MemoryOrder::ACQUIRE);
      if (d == nullptr)
        continue;
      RegionShape s = d->current_shape();
      if (s != RegionShape::ANON_PLACEHOLDER)
        return -ENOTSUP;
      void *rel_base = reinterpret_cast<void *>(old_node->lo);
      size_t rel_size = static_cast<size_t>(old_node->hi - old_node->lo);
      if (!nt_pal::interior_release(rel_base, rel_size))
        return -EFAULT;
    }
  }

  // Split phase for section-view siblings. After the whole-view unmap,
  // the section-view backing's extent is one big placeholder spanning
  // `(b_lo, b_hi)`. `nt_pal::split_placeholder` carves out the surviving
  // sibling slices at each `intent.range` edge that has a sibling.
  //
  // Private-commit siblings need no split — the inside-only demote
  // leaves the surviving slice still committed, not in placeholder
  // state, so there's no placeholder to split.
  //
  // The right-edge split's source address depends on whether the left
  // and right backings are the same:
  //
  //   * Same backing (one wide view spans both edges): after the first
  //     split, the second placeholder begins at `intent.range.lo` and
  //     spans `(intent.range.lo, b_hi)`. The right-edge split targets
  //     that placeholder.
  //   * Different backings: each is its own placeholder. The right-edge
  //     split targets the right backing's `placeholder_base` = the lo
  //     of the rightmost OLD desc-with-extension's backing.
  if (plan.left_sibling.present &&
      plan.left_sibling.old_shape == BackingShape::SectionView) {
    // Source placeholder: (left_sibling.lo, ...). Split at offset
    // (intent.range.lo - left_sibling.lo) carves out the left
    // surviving slice.
    if (!nt_pal::split_placeholder(
            reinterpret_cast<void *>(plan.left_sibling.lo),
            static_cast<size_t>(plan.range.lo() - plan.left_sibling.lo)))
      return -EFAULT;
  }
  if (plan.right_sibling.present &&
      plan.right_sibling.old_shape == BackingShape::SectionView) {
    // Determine the source placeholder's start. If the right backing
    // is the same kernel placeholder that left was carved out of,
    // its remaining piece begins at intent.range.lo. Otherwise we
    // start at the right backing's own (b_lo).
    uintptr_t src_start;
    bool same_backing_both_edges =
        plan.left_sibling.present &&
        plan.left_sibling.old_section_handle ==
            plan.right_sibling.old_section_handle &&
        plan.left_sibling.old_section_handle != nullptr;
    if (same_backing_both_edges) {
      src_start = plan.range.lo();
    } else {
      // Right backing's own placeholder. Walk the locked set for the
      // OLD desc whose hi crosses the right edge — its backing's
      // placeholder_base is the right backing's b_lo.
      DescBacking *right_b = nullptr;
      for (uint32_t k = 0; k < locked.count; ++k) {
        SkiplistNodeBase *n = locked.at(k);
        if (n == nullptr || n->lo < plan.range.hi())
          continue;
        RegionDesc *d = n->value.load(cpp::MemoryOrder::ACQUIRE);
        if (d == nullptr || d->backing_ref == kBackingRefNull)
          continue;
        DescBacking *b = deref_backing_raw(d->backing_ref);
        if (b == nullptr)
          continue;
        right_b = b;
        break;
      }
      if (right_b == nullptr)
        return -EFAULT;
      void *r_ph = right_b->placeholder_base.load(cpp::MemoryOrder::ACQUIRE);
      if (r_ph == nullptr)
        return -EFAULT;
      src_start = reinterpret_cast<uintptr_t>(r_ph);
    }
    // Carve out the inside slice ending at intent.range.hi: split
    // offset = (intent.range.hi - src_start).
    if (!nt_pal::split_placeholder(
            reinterpret_cast<void *>(src_start),
            static_cast<size_t>(plan.range.hi() - src_start)))
      return -EFAULT;
  }

  // Coalesce demoted placeholders inside `range`.
  // `nt_pal::coalesce_placeholders` issues
  // `NtFreeVirtualMemory(MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)`.
  if (plan.coalesce_needed) {
    NTSTATUS st = nt_pal::coalesce_placeholders(plan.coalesce_base,
                                                 plan.coalesce_bytes);
    if (!NT_SUCCESS(st))
      return -EFAULT;
  }

  // Inside-range commit.
  if (plan.has_inside_commit) {
    CommitOp &op = plan.inside_commit;

    DescBacking *backing = backing_alloc();
    if (backing == nullptr)
      return -ENOMEM;
    prov.push(backing);

    // Optional pre-commit reserve (acquire path). MEM_FREE → RESERVE.
    if (op.reserve_placeholder_first) {
      void *reserved =
          nt_pal::reserve_placeholder(op.reserve_base, op.reserve_bytes);
      if (reserved == nullptr) {
        // STATUS_CONFLICTING_ADDRESSES — VA not MEM_FREE. The provisional
        // backing has nullptr kernel-state fields, so rollback skips
        // free / close cleanly.
        return -EEXIST;
      }
      op.placeholder_identity_base = reserved;
    }

    // Sub-granularity shrink. NT reserves placeholders at allocation
    // granularity, so a page-granular `bytes` produces an over-reserve
    // of up to `kAllocGranularity - kPageGranularity` bytes. The
    // backing's placeholder identity is `placeholder_identity_pages *
    // kPageGranularity` — the size the caller meant to keep — which
    // can be less than `reserve_bytes` only in the over-reserve case.
    // When `meta.placeholder_size` is explicitly set (brk's 256 MiB
    // single-shot, `posix_memalign` headroom, `mremap` grow-into-
    // headroom), `reserve_bytes == identity_bytes` and no shrink
    // runs: the caller-sized placeholder is preserved intact.
    //
    // The split is on `placeholder_identity_base` whether the
    // envelope just reserved (`reserve_placeholder_first == true`) or
    // the caller pre-reserved via the scout pattern in
    // `acquire_kernel_chosen[_32bit]`. The pad is a bare placeholder
    // by construction — no MRI_Ex probe needed before MEM_RELEASE.
    const size_t identity_bytes =
        static_cast<size_t>(op.placeholder_identity_pages) *
        kPageGranularity;
    if (op.reserve_bytes > identity_bytes) {
      if (LIBC_UNLIKELY(!nt_pal::split_placeholder(
              op.placeholder_identity_base, identity_bytes))) {
        if (op.reserve_placeholder_first)
          (void)nt_pal::free_placeholder(op.placeholder_identity_base);
        return -ENOMEM;
      }
      PVOID pad = static_cast<char *>(op.placeholder_identity_base) +
                  identity_bytes;
      SIZE_T pad_size = op.reserve_bytes - identity_bytes;
      (void)::NtFreeVirtualMemory(NtCurrentProcess(), &pad, &pad_size,
                                  MEM_RELEASE);
    }

    NTSTATUS st = run_commit(op);
    if (!NT_SUCCESS(st)) {
      if (op.reserve_placeholder_first &&
          op.placeholder_identity_base != nullptr)
        (void)nt_pal::free_placeholder(op.placeholder_identity_base);
      return -ENOMEM;
    }

    backing_set_kernel_state(backing, op.placeholder_identity_base,
                              op.placeholder_identity_pages,
                              op.backing_shape,
                              op.section_handle_for_backing,
                              op.file_handle_for_backing);

    BackingRef ref = encode_backing_ref(backing);
    RegionDesc *desc = build_acquire_desc_from_meta(
        op.kind_for_desc, op.view_prot_for_desc, op.flags_for_desc,
        op.section_offset_for_desc, ref);
    if (desc == nullptr)
      return -ENOMEM;

    int rc = append_node_for_desc(new_nodes, plan.arena, op.new_node_lo,
                                   op.new_node_hi, desc,
                                   /*cleanup_value_on_abort=*/true);
    if (rc != 0)
      return rc;
  }

  // Sibling re-map slots. For each present slot: allocate a fresh
  // DescBacking, run the appropriate kernel re-map (section view:
  // `map_section_replace` against the OLD section handle; private
  // commit: no kernel work because the surviving slice is still
  // committed), populate kernel state on the new backing. Sibling
  // backings carry no handle ownership; the OLD wider backing keeps
  // section / file handles and Stage 2 closes them. The kernel's view-
  // section internal reference keeps the section alive across OLD-handle
  // close.
  DescBacking *new_left_sibling_backing = nullptr;
  DescBacking *new_right_sibling_backing = nullptr;

  if (plan.left_sibling.present) {
    DescBacking *backing = backing_alloc();
    if (backing == nullptr)
      return -ENOMEM;
    prov.push(backing);
    new_left_sibling_backing = backing;

    void *sibling_base = reinterpret_cast<void *>(plan.left_sibling.lo);
    size_t sibling_bytes =
        static_cast<size_t>(plan.range.lo() - plan.left_sibling.lo);
    uint32_t sibling_pages =
        static_cast<uint32_t>(sibling_bytes / kPageGranularity);

    if (plan.left_sibling.old_shape == BackingShape::SectionView) {
      // Placeholder → MAPPED at the surviving slice.
      NTSTATUS st = nt_pal::map_section_replace(
          plan.left_sibling.old_section_handle, sibling_base, sibling_bytes,
          plan.left_sibling.old_section_offset_at_lo,
          plan.left_sibling.old_prot);
      if (!NT_SUCCESS(st))
        return -EFAULT;
    }
    // Private commit shape: no kernel work. The surviving slice stayed
    // committed across demote (the demote loop gated on
    // `succ_is_inside_intent`).

    backing_set_kernel_state(backing, sibling_base, sibling_pages,
                              plan.left_sibling.old_shape,
                              /*section_handle=*/nullptr,
                              /*file_handle=*/nullptr);
    // No new node is appended for sibling backings here — the clone
    // phase's `OutsideSurvivorRebind` clones each surviving OLD desc and
    // binds it to this sibling backing.
  }

  if (plan.right_sibling.present) {
    DescBacking *backing = backing_alloc();
    if (backing == nullptr)
      return -ENOMEM;
    prov.push(backing);
    new_right_sibling_backing = backing;

    void *sibling_base = reinterpret_cast<void *>(plan.right_sibling.lo);
    size_t sibling_bytes =
        static_cast<size_t>(plan.right_sibling.hi - plan.right_sibling.lo);
    uint32_t sibling_pages =
        static_cast<uint32_t>(sibling_bytes / kPageGranularity);

    if (plan.right_sibling.old_shape == BackingShape::SectionView) {
      NTSTATUS st = nt_pal::map_section_replace(
          plan.right_sibling.old_section_handle, sibling_base, sibling_bytes,
          plan.right_sibling.old_section_offset_at_lo,
          plan.right_sibling.old_prot);
      if (!NT_SUCCESS(st))
        return -EFAULT;
    }

    backing_set_kernel_state(backing, sibling_base, sibling_pages,
                              plan.right_sibling.old_shape,
                              /*section_handle=*/nullptr,
                              /*file_handle=*/nullptr);
  }
  // The two sibling-backing locals are read by the clone phase below.

  // Clone phase: produce new nodes by cloning existing descs (mutate,
  // split, outside-survivor rebind). No kernel work in this phase.
  switch (plan.clone_mode) {
  case CommitPlan::CloneMode::None:
    break;

  case CommitPlan::CloneMode::AllSuccsWithMutator: {
    // Emit up to three clones per locked succ:
    //   * left-outside slice  [src.lo, intent.lo) — non-mutated; emitted
    //                          only when src.lo < intent.lo.
    //   * inside slice        [max(src.lo, intent.lo),
    //                          min(src.hi, intent.hi)) — mutator
    //                          applied; emitted whenever the
    //                          intersection is non-empty.
    //   * right-outside slice [intent.hi, src.hi) — non-mutated;
    //                          emitted only when src.hi > intent.hi.
    //
    // All clones inherit the source's `backing_ref` — mutate does not
    // change backings. Section_offset shifts via
    // `clone_region_desc_for_fragment(src, src.lo, frag_lo)` for any
    // slice whose `frag_lo > src.lo` (the inside slice of a left-edge
    // straddler and the right-outside slice of any right-edge
    // straddler).
    //
    // The clone phase does NOT update view_prot — acquire-time intent
    // is preserved. The kernel-side protection write in the post-Swap
    // phase is clipped to the inside slice and is the only visible
    // side effect.
    for (uint32_t k = 0; k < locked.count; ++k) {
      SkiplistNodeBase *src_node = locked.at(k);
      if (src_node == nullptr)
        continue;
      RegionDesc *src = src_node->value.load(cpp::MemoryOrder::ACQUIRE);
      if (src == nullptr)
        continue;

      const uintptr_t inside_lo =
          src_node->lo > plan.range.lo() ? src_node->lo : plan.range.lo();
      const uintptr_t inside_hi =
          src_node->hi < plan.range.hi() ? src_node->hi : plan.range.hi();
      const bool has_left_outside = src_node->lo < plan.range.lo();
      const bool has_right_outside = src_node->hi > plan.range.hi();

      if (has_left_outside) {
        RegionDesc *clone = clone_region_desc_for_fragment(src, src_node->lo,
                                                            src_node->lo);
        if (clone == nullptr)
          return -ENOMEM;
        int rc = append_node_for_desc(new_nodes, plan.arena, src_node->lo,
                                       inside_lo, clone,
                                       /*cleanup_value_on_abort=*/true);
        if (rc != 0)
          return rc;
      }

      if (inside_lo < inside_hi) {
        RegionDesc *clone =
            clone_region_desc_for_fragment(src, src_node->lo, inside_lo);
        if (clone == nullptr)
          return -ENOMEM;
        if (plan.mutator != nullptr)
          plan.mutator(clone, plan.mutator_ctx);
        int rc = append_node_for_desc(new_nodes, plan.arena, inside_lo,
                                       inside_hi, clone,
                                       /*cleanup_value_on_abort=*/true);
        if (rc != 0)
          return rc;
      }

      if (has_right_outside) {
        RegionDesc *clone =
            clone_region_desc_for_fragment(src, src_node->lo, inside_hi);
        if (clone == nullptr)
          return -ENOMEM;
        int rc = append_node_for_desc(new_nodes, plan.arena, inside_hi,
                                       src_node->hi, clone,
                                       /*cleanup_value_on_abort=*/true);
        if (rc != 0)
          return rc;
      }
    }
    break;
  }

  case CommitPlan::CloneMode::SplitAtBoundary: {
    // Single locked succ guaranteed by build_plan_split's precondition.
    SkiplistNodeBase *src_node = locked.at(0);
    if (src_node == nullptr)
      return -EINVAL;
    RegionDesc *src = src_node->value.load(cpp::MemoryOrder::ACQUIRE);
    if (src == nullptr)
      return -EINVAL;
    // Left half [src_lo, boundary) — no offset shift.
    RegionDesc *left =
        clone_region_desc_for_fragment(src, src_node->lo, src_node->lo);
    if (left == nullptr)
      return -ENOMEM;
    int rc = append_node_for_desc(new_nodes, plan.arena, src_node->lo,
                                   plan.split_boundary, left,
                                   /*cleanup_value_on_abort=*/true);
    if (rc != 0)
      return rc;
    // Right half [boundary, src_hi) — section_offset shifts by
    // (boundary - src_lo).
    RegionDesc *right = clone_region_desc_for_fragment(src, src_node->lo,
                                                        plan.split_boundary);
    if (right == nullptr)
      return -ENOMEM;
    return append_node_for_desc(new_nodes, plan.arena, plan.split_boundary,
                                 src_node->hi, right,
                                 /*cleanup_value_on_abort=*/true);
  }

  case CommitPlan::CloneMode::OutsideSurvivorRebind: {
    // Emit a survivor clone for the outside-intent portion of each
    // locked succ. The unified geometry handles three input shapes
    // with one walk:
    //
    //   * Fully-outside-left  : src.hi <= intent.lo. Emit one clone
    //                            covering the full source extent
    //                            against the left sibling backing.
    //   * Fully-outside-right : src.lo >= intent.hi. Symmetric.
    //   * Edge-straddler      : src extent crosses intent.lo and/or
    //                            intent.hi. Emit one or two clones
    //                            covering only the outside slices;
    //                            the inside slice is consumed by
    //                            `inside_commit`.
    //   * Fully-inside        : src.lo >= intent.lo and src.hi <=
    //                            intent.hi. Skipped — no outside
    //                            portion, inside is consumed by
    //                            `inside_commit`.
    //
    // The clone's section_offset shifts when the outside slice does
    // not start at the source's lo (right-edge straddler case): the
    // right slice begins at `intent.hi`, so the section byte at the
    // slice's start is `src.section_offset + (intent.hi - src.lo)`.
    // `clone_region_desc_for_fragment(src, src.lo, frag_lo)` does the
    // shift when `frag_lo > src.lo`.
    BackingRef left_sibling_ref = kBackingRefNull;
    BackingRef right_sibling_ref = kBackingRefNull;
    if (new_left_sibling_backing != nullptr)
      left_sibling_ref = encode_backing_ref(new_left_sibling_backing);
    if (new_right_sibling_backing != nullptr)
      right_sibling_ref = encode_backing_ref(new_right_sibling_backing);

    for (uint32_t k = 0; k < locked.count; ++k) {
      SkiplistNodeBase *src_node = locked.at(k);
      if (src_node == nullptr)
        continue;
      RegionDesc *src = src_node->value.load(cpp::MemoryOrder::ACQUIRE);
      if (src == nullptr)
        continue;

      const bool has_left_outside = src_node->lo < plan.range.lo();
      const bool has_right_outside = src_node->hi > plan.range.hi();
      if (!has_left_outside && !has_right_outside)
        continue; // inside-intent succ — consumed by inside_commit.

      if (has_left_outside) {
        if (left_sibling_ref == kBackingRefNull)
          return -EFAULT;
        const uintptr_t lo = src_node->lo;
        const uintptr_t hi =
            src_node->hi < plan.range.lo() ? src_node->hi : plan.range.lo();
        // Left-slice clone starts at the source's lo — no offset shift.
        RegionDesc *clone =
            clone_region_desc_for_fragment(src, src_node->lo, src_node->lo);
        if (clone == nullptr)
          return -ENOMEM;
        clone->backing_ref = left_sibling_ref;
        int rc = append_node_for_desc(new_nodes, plan.arena, lo, hi, clone,
                                       /*cleanup_value_on_abort=*/true);
        if (rc != 0)
          return rc;
      }

      if (has_right_outside) {
        if (right_sibling_ref == kBackingRefNull)
          return -EFAULT;
        const uintptr_t lo =
            src_node->lo > plan.range.hi() ? src_node->lo : plan.range.hi();
        const uintptr_t hi = src_node->hi;
        // Right-slice clone: section_offset shifts by (frag_lo - src.lo)
        // when `lo > src_node->lo` (the right-edge straddler case).
        RegionDesc *clone =
            clone_region_desc_for_fragment(src, src_node->lo, lo);
        if (clone == nullptr)
          return -ENOMEM;
        clone->backing_ref = right_sibling_ref;
        int rc = append_node_for_desc(new_nodes, plan.arena, lo, hi, clone,
                                       /*cleanup_value_on_abort=*/true);
        if (rc != 0)
          return rc;
      }
    }
    break;
  }
  }
  return 0;
}

//===----------------------------------------------------------------------===//
//  Post-Swap kernel-side fixups
//===----------------------------------------------------------------------===//

// Issue `nt_pal::protect` per locked succ to bring kernel-side protection
// into line with the new desc state (`NtProtectVirtualMemory` is per-VAD
// atomic; a range spanning multiple split placeholders needs per-VAD calls).
// Runs after Swap has published the new chain so a concurrent reader cannot
// observe stale prot.
//
// Per-succ protect is clipped to `[max(n.lo, plan.range.lo()),
// min(n.hi, plan.range.hi()))` — for an edge-straddler the outside
// survivor portion keeps its existing kernel protection, matching POSIX
// `mprotect(addr, len, prot)` which only touches `[addr, addr+len)`. The
// clip never collapses to an empty range here: mutate doesn't run
// preflight widening, so every locked succ overlaps the intent by
// construction.
[[nodiscard]] int post_swap_protect(const CommitPlan &plan,
                                     const LockedSet &locked) {
  if (!plan.issue_protect)
    return 0;
  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *old_node = locked.at(k);
    if (old_node == nullptr)
      continue;
    const uintptr_t lo =
        old_node->lo > plan.range.lo() ? old_node->lo : plan.range.lo();
    const uintptr_t hi =
        old_node->hi < plan.range.hi() ? old_node->hi : plan.range.hi();
    ULONG old_prot = 0;
    if (!nt_pal::protect(reinterpret_cast<void *>(lo),
                         static_cast<size_t>(hi - lo),
                         static_cast<ULONG>(plan.protect_value), &old_prot))
      return -EFAULT;
  }
  return 0;
}

// Apply COW translation to a base `PAGE_*` value when the OLD desc
// has `region_flag::COW`. Source of truth is the desc flag stamped
// at acquire time; MBI's `AllocationProtect` is the fallback for
// foreign mappings (untracked MEM_MAPPED with `PAGE_WRITECOPY` /
// `PAGE_EXECUTE_WRITECOPY` allocation prot). The substrate sees
// only tracked VAs inside its locked succs, so the desc check is the
// authoritative path; the MBI fallback runs once per chunk on the
// committed path for foreign-leaning shapes.
[[nodiscard]] LIBC_INLINE DWORD cow_translate_prot(DWORD new_prot,
                                                    bool desc_is_cow,
                                                    DWORD mbi_alloc_prot,
                                                    DWORD mbi_type) {
  bool needs_cow = desc_is_cow;
  if (!needs_cow && mbi_type == MEM_MAPPED) {
    DWORD ap = mbi_alloc_prot & 0xFFu;
    needs_cow = (ap == PAGE_WRITECOPY || ap == PAGE_EXECUTE_WRITECOPY);
  }
  if (!needs_cow)
    return new_prot;
  if (new_prot == PAGE_READWRITE)
    return PAGE_WRITECOPY;
  if (new_prot == PAGE_EXECUTE_READWRITE)
    return PAGE_EXECUTE_WRITECOPY;
  return new_prot;
}

// Per-chunk dispatch for the mutate-with-commit-on-uncommitted path.
// Walks each locked succ's intersection with `plan.range` via the
// kernel VAD chain (`nt_pal::RegionWalker`) and dispatches the three-
// way state machine the legacy mprotect slow path encoded:
//
//   MEM_FREE chunk                            → `-ENOMEM`
//   MEM_COMMIT chunk                           → `nt_pal::protect`
//   uncommitted + PROT_NONE target             → no-op
//   uncommitted + accessible + MEM_MAPPED      → `nt_pal::commit_in_reservation_no_writewatch`
//   uncommitted + accessible + MEM_PRIVATE     → `nt_pal::commit_replace_numa` (or `commit_replace`)
//
// COW translation per succ via the OLD desc's `region_flag::COW`;
// foreign-mapping fallback via MBI `AllocationProtect`. CFG-secured
// ranges are retried inside `nt_pal::protect` itself, so this phase
// stays single-call per chunk.
//
// Runs after Swap has published the new clones (with view_prot /
// PROT_DIVERGED already updated by the clone phase) so any
// concurrent reader pinning the new chain sees a desc consistent
// with the kernel state this phase establishes. Per-chunk MBI walks
// under the LOCKED hold are bounded by VAD count inside the succ;
// disjoint mutators on other descs proceed in parallel.
[[nodiscard]] int post_swap_per_chunk_dispatch(const CommitPlan &plan,
                                                const LockedSet &locked) {
  if (!plan.dispatch_per_chunk)
    return 0;

  auto ws = ::LIBC_NAMESPACE::windows::byte_scratch(4096);
  if (!ws)
    return -ENOMEM;

  const DWORD base_prot = static_cast<DWORD>(plan.protect_value);

  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *old_node = locked.at(k);
    if (old_node == nullptr)
      continue;
    RegionDesc *old_desc =
        old_node->value.load(cpp::MemoryOrder::ACQUIRE);
    const bool desc_is_cow =
        old_desc != nullptr && old_desc->has_flag(region_flag::COW);

    const uintptr_t lo = old_node->lo > plan.range.lo()
                             ? old_node->lo
                             : plan.range.lo();
    const uintptr_t hi = old_node->hi < plan.range.hi()
                             ? old_node->hi
                             : plan.range.hi();
    if (lo >= hi)
      continue;

    nt_pal::RegionWalker walk(reinterpret_cast<void *>(lo),
                               static_cast<SIZE_T>(hi - lo), ws.data(),
                               ws.size());
    while (walk.next()) {
      if (walk.entry->State == MEM_FREE)
        return -ENOMEM;

      const DWORD eff_prot = cow_translate_prot(
          base_prot, desc_is_cow, walk.entry->AllocationProtect,
          walk.entry->Type);

      if (walk.entry->State == MEM_COMMIT) {
        ULONG old_prot_out = 0;
        if (!nt_pal::protect(walk.chunk, walk.chunk_size, eff_prot,
                             &old_prot_out))
          return -EFAULT;
        continue;
      }

      // Uncommitted + PROT_NONE: leave as-is; NtProtect on
      // uncommitted pages returns STATUS_NOT_COMMITTED.
      if (eff_prot == PAGE_NOACCESS)
        continue;

      if (walk.entry->Type == MEM_MAPPED) {
        NTSTATUS st = nt_pal::commit_in_reservation_no_writewatch(
            walk.chunk, walk.chunk_size, eff_prot);
        if (NT_ERROR(st))
          return -ENOMEM;
        continue;
      }

      // MEM_PRIVATE + uncommitted + accessible: bare-placeholder
      // demand-map. NUMA hint via `plan.numa_node` from the caller;
      // unhinted commit on `<0` or kernel rejection of the NUMA
      // path.
      NTSTATUS st = STATUS_INVALID_PARAMETER;
      if (plan.numa_node >= 0) {
        st = nt_pal::commit_replace_numa(
            walk.chunk, walk.chunk_size, eff_prot,
            static_cast<ULONG>(plan.numa_node));
      }
      if (NT_ERROR(st))
        st = nt_pal::commit_replace(walk.chunk, walk.chunk_size, eff_prot);
      if (NT_ERROR(st))
        return -ENOMEM;
    }
  }
  return 0;
}

// Zero the `placeholder_base` of every OLD backing whose placeholder
// identity is consumed by a commit-phase commit, so Stage 2's
// `backing_kill_and_retire` skips `free_placeholder` for the consumed
// identity. OLD section / file handles are NOT zeroed — they are stale
// handles to the previous mapping and Stage 2 closes them.
//
// Skip any OLD backing that also appears as a NEW backing: a sibling
// re-map can reuse the same backing across a fragment, in which case
// zeroing its `placeholder_base` would orphan the kernel placeholder.
void post_swap_ownership_transfer(const CommitPlan &plan,
                                   const LockedSet &locked,
                                   const NewNodes &new_nodes) {
  if (plan.ownership_mode != CommitPlan::OwnershipMode::AllSuccs)
    return;
  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *old_node = locked.at(k);
    if (old_node == nullptr)
      continue;
    RegionDesc *d = old_node->value.load(cpp::MemoryOrder::ACQUIRE);
    if (d == nullptr || d->backing_ref == kBackingRefNull)
      continue;
    DescBacking *ob = deref_backing_raw(d->backing_ref);
    if (ob == nullptr)
      continue;
    bool reused_by_new = false;
    for (uint32_t j = 0; j < new_nodes.count; ++j) {
      SkiplistNodeBase *N = new_nodes.node_at(j);
      if (N == nullptr)
        continue;
      RegionDesc *Nd = N->value.load(cpp::MemoryOrder::ACQUIRE);
      if (Nd == nullptr || Nd->backing_ref == kBackingRefNull)
        continue;
      if (deref_backing_raw(Nd->backing_ref) == ob) {
        reused_by_new = true;
        break;
      }
    }
    if (reused_by_new)
      continue;
    // RELEASE: concurrent readers (msync / madvise / numa_ops) holding
    // pins on the OLD backing observe nullptr and skip — the correct
    // behaviour because the placeholder is being replaced. Section/file
    // handles read by those readers may still be the OLD ones; they'll
    // get `STATUS_INVALID_HANDLE` post-Stage-2 close, which the readers
    // tolerate.
    ob->placeholder_base.store(nullptr, cpp::MemoryOrder::RELEASE);
  }
}

//===----------------------------------------------------------------------===//
//  Survivor walk and Stage 2 teardown
//===----------------------------------------------------------------------===//

// Skiplist visitor that flips `found` to true on the first LIVE desc
// referencing `target`. Short-circuits subsequent visits via the same flag.
struct SurvivorScanCtx {
  DescBacking *target;
  bool found;
};

void survivor_scan_visitor(SkiplistNodeBase *node, void *ctx_p) {
  auto *ctx = static_cast<SurvivorScanCtx *>(ctx_p);
  if (ctx->found || node == nullptr)
    return;
  RegionDesc *d = node->value.load(cpp::MemoryOrder::ACQUIRE);
  if (d == nullptr || d->backing_ref == kBackingRefNull)
    return;
  if (deref_backing_raw(d->backing_ref) == ctx->target)
    ctx->found = true;
}

struct SurvivorScanAdapter {
  SurvivorScanCtx *ctx{nullptr};
  LIBC_INLINE void operator()(SkiplistNodeBase *node) {
    survivor_scan_visitor(node, ctx);
  }
};

/// Post-Swap synchronous kernel-state teardown for OLD backings.
///
/// Iterates `locked.succ` directly; for each succ's backing:
///
///   1. State pre-check — if `state != Live`, short-circuit. Handles both
///      the duplicate-shared-backing case (the first iteration on a shared
///      backing wins the state CAS; later iterations observe Killed in O(1)
///      and skip) and cross-envelope races (a peer envelope killed first).
///   2. NewNode survivor check — mutate-path clones reference the same
///      backing as their OLDs, so they must not be killed. Replace's new
///      commit allocates a fresh backing, so its OLDs have no NewNode
///      referencer and proceed to teardown.
///   3. Outside-range survivor scan — when the backing extent extends past
///      `range`, walk the slivers `(b_lo, range.lo)` ∪ `(range.hi, b_hi)`
///      for any LIVE referencer. Inside-range descs are in `locked.succ`
///      by construction; re-walking them would scale with the backing
///      extent rather than the operation. For brk's 256 MiB single-shot
///      backing, walking only the outside-range slivers turns a per-
///      cursor-advance O(256 MiB) walk into O(0) when `range` equals the
///      backing extent.
///   4. Race state CAS `Live → Killed`. The CAS is the linearisation point
///      for teardown ownership: the unique winner runs
///      `backing_kill_and_retire` synchronously, so kernel state is settled
///      before the envelope returns. ACQ_REL is required — `backing_kill_
///      and_retire` ACQUIRE-exchanges the kernel-state fields, and Stage 2
///      readers must observe the published Killed before either side reads
///      stale fields.
///
/// No candidates list, no dedup pass, no caps; cannot fail since the
/// per-iteration state-load short-circuit absorbs every redundant or raced
/// visit. The two `is_walk_range` calls share skiplist-domain pin slots
/// but each call re-anchors at entry, so no stale-pin carryover.
void run_stage2(Arena *arena, VaRange range, const LockedSet &locked,
                 const NewNodes &new_nodes) {
  for (uint32_t i = 0; i < locked.count; ++i) {
    SkiplistNodeBase *old_node = locked.at(i);
    if (old_node == nullptr)
      continue;
    RegionDesc *d = old_node->value.load(cpp::MemoryOrder::ACQUIRE);
    if (d == nullptr || d->backing_ref == kBackingRefNull)
      continue;
    DescBacking *b = deref_backing_raw(d->backing_ref);
    if (b == nullptr)
      continue;

    // (1) State pre-check — pairs with the CAS-RELEASE in step 4 to
    // ACQUIRE the killer's prior writes when observed Killed.
    if (b->state.load(cpp::MemoryOrder::ACQUIRE) != kBackingStateLive)
      continue;

    // (2) NewNode survivor check.
    bool has_survivor = false;
    for (uint32_t j = 0; j < new_nodes.count; ++j) {
      SkiplistNodeBase *N = new_nodes.node_at(j);
      if (N == nullptr)
        continue;
      RegionDesc *Nd = N->value.load(cpp::MemoryOrder::ACQUIRE);
      if (Nd == nullptr || Nd->backing_ref == kBackingRefNull)
        continue;
      if (deref_backing_raw(Nd->backing_ref) == b) {
        has_survivor = true;
        break;
      }
    }
    if (has_survivor)
      continue;

    // (3) Outside-range survivor scan. Skip the kill if any LIVE desc
    // still references this backing in the slivers
    // `(b_lo, range.lo)` ∪ `(range.hi, b_hi)`. Release's plan builder
    // pre-emptively rebinds those survivors onto fresh sibling
    // backings via `OutsideSurvivorRebind`, so this scan sees no hits
    // for the OLD backing on the shared-backing release path; the
    // kill then proceeds metadata-only against the OLD backing whose
    // `placeholder_base` was nulled by `post_swap_ownership_transfer`.
    uintptr_t b_lo = reinterpret_cast<uintptr_t>(
        b->placeholder_base.load(cpp::MemoryOrder::ACQUIRE));
    uintptr_t b_hi = b_lo + static_cast<uintptr_t>(b->placeholder_pages) *
                                kPageGranularity;
    if (b_lo != 0 && b_lo < range.lo()) {
      SurvivorScanCtx sctx{b, /*found=*/false};
      SurvivorScanAdapter adapter;
      adapter.ctx = &sctx;
      is_walk_range(arena, b_lo, range.lo(), adapter);
      if (sctx.found)
        continue;
    }
    if (b_lo != 0 && range.hi() < b_hi) {
      SurvivorScanCtx sctx{b, /*found=*/false};
      SurvivorScanAdapter adapter;
      adapter.ctx = &sctx;
      is_walk_range(arena, range.hi(), b_hi, adapter);
      if (sctx.found)
        continue;
    }

    // (4) Race the Live → Killed CAS; the winner owns synchronous
    // teardown.
    uint8_t expected = kBackingStateLive;
    if (b->state.compare_exchange_strong(
            expected, kBackingStateKilled, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE))
      backing_kill_and_retire(b);
  }
}

//===----------------------------------------------------------------------===//
//  Envelope driver — run_envelope
//===----------------------------------------------------------------------===//

// Bounded retry cap on Swap-CAS loss. Sustained loss past this count signals
// either pathological contention on the same interval or a starvation bug;
// either way `-EAGAIN` propagates to the caller for handling.
constexpr uint32_t kCommitRetryCap = 8;

using BuildPlanFn = int (*)(const CommitIntent &, Arena *, const LockedSet &,
                              CommitPlan &);

[[nodiscard]] LIBC_INLINE BuildPlanFn build_plan_for_op(OpKind op) {
  switch (op) {
  case OpKind::Acquire:           return &build_plan_acquire;
  case OpKind::Release:           return &build_plan_release;
  case OpKind::Replace:           return &build_plan_replace;
  case OpKind::Mutate:            return &build_plan_mutate;
  case OpKind::Split:             return &build_plan_split;
  case OpKind::AcquireAtReserved: return &build_plan_acquire_at_reserved;
  }
  return nullptr;
}

/// Run one transactional envelope: Lock → plan-build → kernel program →
/// Swap → post-Swap fixups → Stage 2 → Unlock.
///
/// Each typed op constructs a `CommitIntent` and arrives here via
/// `dispatch_per_arena_op`. The envelope is the single internal site that
/// invokes `nt_pal::*` for tracked VA; the public surface (`acquire`,
/// `release`, `replace`, `mutate`, `split`) is record-then-commit and never
/// lock-then-mutate. Returns 0 on success or a negative errno on failure;
/// the provisional list and `retire_unpublished_nodes` catch every backing
/// and node alloc so failure paths cannot leak.
int run_envelope(const CommitIntent &intent) {
  // `OpKind::Acquire` demands alloc-granularity-aligned base because the
  // envelope itself reserves the placeholder via NT's
  // `MEM_RESERVE_PLACEHOLDER`, which only accepts alloc-aligned bases.
  // `OpKind::AcquireAtReserved` and every interior op accept page-
  // aligned bases — the caller (or a prior typed op) already placed
  // the VAD, so NT's reserve constraint no longer applies.
  bool ok_range;
  switch (intent.op) {
  case OpKind::Acquire:
    ok_range = range_valid_acquire(intent.range);
    break;
  case OpKind::AcquireAtReserved:
  default:
    ok_range = range_valid_interior(intent.range);
    break;
  }
  if (LIBC_UNLIKELY(!ok_range))
    return -EINVAL;
  if (LIBC_UNLIKELY(!fits_one_arena(intent.range)))
    return -ENOTSUP; // Caller (the public typed op) decomposes.

  Arena *arena = resolve_or_install_arena(intent.range.lo());
  if (LIBC_UNLIKELY(arena == nullptr))
    return -ENOMEM;

  BuildPlanFn build = build_plan_for_op(intent.op);
  if (LIBC_UNLIKELY(build == nullptr))
    return -EINVAL;

  // Pin the backing-domain era for the rest of the envelope on
  // `BackingPinSlot::kEngineAnchor`. One call covers every retry — the
  // slot is never rotated by sibling va_tracker calls inside the loop,
  // and the typed slot tag is statically disjoint from the reader-side
  // `BackingPinSlot::kReaderPin` per the `BackingPinSlot` namespace's
  // static_assert. See `desc_backing.h` for the full discipline.
  anchor_backing_engine_pin();

  // The locked range may extend past `intent.range` for replace when a
  // shared backing extends past either edge. Widening is monotonic — we
  // never narrow — so a Swap-CAS retry re-acquires on the latest range
  // without risk of dropping a previously-discovered extension.
  uintptr_t lock_lo = intent.range.lo();
  uintptr_t lock_hi = intent.range.hi();

  for (uint32_t attempt = 0; attempt < kCommitRetryCap; ++attempt) {
    ProvisionalList prov;
    NewNodes new_nodes;
    CommitPlan plan;

    LockedSet locked;
    (void)locked.acquire(arena, lock_lo, lock_hi);
    if (!locked.valid()) {
      int err = locked.errno_;
      return err == 0 ? -EINVAL : err;
    }

    // Replace / release preflight: detect outside-range backings and
    // widen the locked range to cover their full extent, so the plan
    // builder sees every outside-range survivor desc in the locked set
    // and the clone phase can rebind them onto fresh sibling backings
    // without holding a too-narrow lock. Release reuses replace's
    // OutsideSurvivorRebind path to relocate survivor descs off the OLD
    // backing before Stage 2 kills it.
    //
    // Convergence: one extension iteration suffices. After widening to
    // (min over leftmost backings' b_lo, max over rightmost backings'
    // b_hi), the now-leftmost succ's backing is either the same one we
    // already saw extending past the original edge, or a wholly-inside
    // backing. Either way, no further extension is needed.
    if (intent.op == OpKind::Replace || intent.op == OpKind::Release) {
      uintptr_t want_lo = lock_lo;
      uintptr_t want_hi = lock_hi;
      for (uint32_t k = 0; k < locked.count; ++k) {
        SkiplistNodeBase *n = locked.at(k);
        if (n == nullptr)
          continue;
        RegionDesc *d = n->value.load(cpp::MemoryOrder::ACQUIRE);
        if (d == nullptr || d->backing_ref == kBackingRefNull)
          continue;
        DescBacking *b = deref_backing_raw(d->backing_ref);
        if (b == nullptr)
          continue;
        void *ph = b->placeholder_base.load(cpp::MemoryOrder::ACQUIRE);
        if (ph == nullptr)
          continue;
        uintptr_t b_lo = reinterpret_cast<uintptr_t>(ph);
        uintptr_t b_hi =
            b_lo + static_cast<uintptr_t>(b->placeholder_pages) *
                       kPageGranularity;
        if (b_lo < want_lo)
          want_lo = b_lo;
        if (b_hi > want_hi)
          want_hi = b_hi;
      }
      if (want_lo < lock_lo || want_hi > lock_hi) {
        // Drop the lock and re-acquire on the widened range. No kernel
        // work has been done, so this is not a Swap-CAS retry; we
        // increment `attempt` only via the loop step, and widening
        // converges in one iteration.
        Unlock(locked, /*include_pred=*/true);
        lock_lo = want_lo;
        lock_hi = want_hi;
        continue;
      }
    }

    // Plan-build: pure metadata inspection of the locked set. No kernel
    // work, no allocations beyond the stack-resident plan.
    int err = build(intent, arena, locked, plan);
    if (err != 0) {
      Unlock(locked, /*include_pred=*/true);
      return err;
    }

    // Kernel program: demote → split → coalesce → commit → clone.
    // Failure walks the provisional list and retires unpublished nodes.
    err = execute_plan(plan, locked, new_nodes, prov);
    if (err != 0) {
      retire_unpublished_nodes(new_nodes);
      rollback_provisional(prov);
      Unlock(locked, /*include_pred=*/true);
      return err;
    }

    // Linearisation point: Swap atomically detaches the OLD chain and
    // publishes the NewNodes. CAS loss means a concurrent envelope on
    // an overlapping range got there first; retry.
    if (!Swap(locked, new_nodes)) {
      retire_unpublished_nodes(new_nodes);
      rollback_provisional(prov);
      Unlock(locked, /*include_pred=*/true);
      continue;
    }

    // Post-linearisation: NewNodes are published.

    // Kernel-side protect (mutate path). `NtProtectVirtualMemory` is
    // per-VAD atomic; iterate `locked.succ`. The per-chunk dispatch
    // variant replaces this with a `RegionWalker`-driven walk that
    // also handles uncommitted-accessible chunks via
    // `commit_in_reservation` / `commit_replace[_numa]`; the two
    // phases are mutually exclusive (per `build_plan_mutate`).
    int prot_err = plan.dispatch_per_chunk
                       ? post_swap_per_chunk_dispatch(plan, locked)
                       : post_swap_protect(plan, locked);
    if (prot_err != 0) {
      // The chain is already published. Surface the error; the caller
      // sees that the protect side-effect failed even though the desc
      // state landed. This matches POSIX `mprotect`'s documented
      // behaviour (partial success on per-page kernel errors is
      // implementation-defined). FIXME(va-tracker): revert the desc
      // state via a follow-up mutate envelope.
      Unlock(locked, /*include_pred=*/false);
      return prot_err;
    }

    // Ownership transfer of OLD backings' placeholder identity (replace
    // path). Skips OLD backings reused by a NEW node so a sibling re-map
    // doesn't orphan its own placeholder.
    post_swap_ownership_transfer(plan, locked, new_nodes);

    // Stage 2: synchronous teardown of any OLD backing whose extent has
    // no LIVE referencer. Cannot fail — the state-load short-circuit
    // absorbs duplicates and cross-envelope races.
    run_stage2(arena, intent.range, locked, new_nodes);

    Unlock(locked, /*include_pred=*/false);
    prov.clear();
    return 0;
  }
  return -EAGAIN;
}

//===----------------------------------------------------------------------===//
//  Cross-arena dispatcher
//===----------------------------------------------------------------------===//

// Decompose a multi-arena range into per-arena envelopes. Each per-arena
// envelope is atomic; the multi-arena composite is N independent atomic
// ops. Acquire and replace can therefore leave partial state on `N > 1`
// failure — the caller observes the first failed per-arena envelope's
// errno and is responsible for any cleanup. Single-arena ranges (the
// common case) skip the decomposition entirely.
int dispatch_per_arena_op(CommitIntent intent) {
  if (fits_one_arena(intent.range))
    return run_envelope(intent);

  uintptr_t cursor = intent.range.lo();
  uintptr_t end = intent.range.hi();
  while (cursor < end) {
    uintptr_t leaf_hi = arena_hi_for(cursor);
    uintptr_t this_hi = (leaf_hi < end) ? leaf_hi : end;
    CommitIntent sub = intent;
    sub.range.start = reinterpret_cast<void *>(cursor);
    sub.range.bytes = static_cast<size_t>(this_hi - cursor);
    int rc = run_envelope(sub);
    if (rc != 0)
      return rc;
    cursor = this_hi;
  }
  return 0;
}

} // namespace

//===----------------------------------------------------------------------===//
//  Public typed ops
//===----------------------------------------------------------------------===//

// Each public op constructs one `CommitIntent` and dispatches through
// `dispatch_per_arena_op`. Single-entry mutations are exposed as ergonomic
// shorthands implemented as one-step transactions over the same engine —
// not as a parallel mechanism.

// Shared tail of every kernel-chosen acquire variant. The caller has
// already scouted a placeholder at `base` (any sub-2-GiB / unconstrained /
// future NUMA-affined scout dispatches here); this routine drives the
// AcquireAtReserved envelope and owns the placeholder cleanup on
// envelope failure.
//
// Kept in an anonymous namespace so the two public entries below stay
// the only escape hatches. The early bail on `base == nullptr` lets
// the callers funnel a failed scout through here without duplicating
// the ENOMEM return.
namespace {

constexpr uintptr_t kArenaStep = uintptr_t{1} << 32;

// Free every BARE-PLACEHOLDER VAD that still spans `[base, base + bytes)`
// after a partial-failure cleanup. Walks the range at arena-step
// granularity because every interior arena boundary was split into
// its own VAD by `split_reserved_at_arena_boundaries` before the
// dispatch ran.
//
// Probes each VAD via `MemoryRegionInformationEx` and only releases
// when `PlaceholderReservation == 1`: a committed VAD whose desc is
// still registered (rare — only reached if `vt::release` itself
// failed) must NOT be MEM_RELEASE'd here, because that releases the
// commit too and leaves the desc pointing at MEM_FREE memory. A
// MEM_FREE address fails the MRI_Ex query and is silently skipped.
LIBC_INLINE void free_placeholders_in_range(void *base, size_t bytes) {
  uintptr_t lo = reinterpret_cast<uintptr_t>(base);
  uintptr_t end = lo + bytes;
  uintptr_t cur = lo;
  while (cur < end) {
    MEMORY_REGION_INFORMATION mri{};
    if (nt_pal::query_region_mri(reinterpret_cast<void *>(cur), mri) &&
        mri.PlaceholderReservation != 0) {
      (void)nt_pal::free_placeholder(reinterpret_cast<void *>(cur));
    }
    // Next VAD starts at the next arena boundary above `cur`. If
    // `cur` is itself arena-aligned, advance by one full arena.
    cur = (cur & ~(kArenaStep - 1)) + kArenaStep;
  }
}

// Split the placeholder anchored at `base` at every arena boundary
// strictly within `[base, base + bytes)`. After return on success the
// scout reservation is N adjacent VADs whose boundaries align with
// the dispatcher's per-arena decomposition.
//
// Required because `MEM_REPLACE_PLACEHOLDER` requires the commit
// size to exactly match the underlying placeholder VAD
// (`Placeholders.md` §3); the kernel-chosen scout creates a single
// VAD spanning every arena, so without these splits the 2nd+ per-
// arena `commit_replace` would fail with STATUS_INVALID_PARAMETER.
//
// `OpKind::Acquire` does not need this because each per-arena sub-
// envelope reserves its own slice (one VAD per slice by
// construction). The fix applies only to `OpKind::AcquireAtReserved`
// (the `acquire_kernel_chosen[_32bit]` scout path).
//
// On split failure the helper frees every VAD it already created
// plus the un-split tail before returning, so the caller does not
// own any partial state and surfaces `-ENOMEM` directly.
[[nodiscard]] LIBC_INLINE int
split_reserved_at_arena_boundaries(void *base, size_t bytes) {
  uintptr_t lo = reinterpret_cast<uintptr_t>(base);
  uintptr_t end = lo + bytes;
  uintptr_t boundary = (lo & ~(kArenaStep - 1)) + kArenaStep;
  uintptr_t current_lo = lo;
  while (boundary < end) {
    size_t offset = static_cast<size_t>(boundary - current_lo);
    if (LIBC_UNLIKELY(!nt_pal::split_placeholder(
            reinterpret_cast<void *>(current_lo), offset))) {
      // Free every VAD we already split off plus the un-split
      // remainder. Walk arena boundaries from `lo` through and
      // including `current_lo` (the still-too-big VAD).
      uintptr_t cleanup_lo = lo;
      while (true) {
        (void)nt_pal::free_placeholder(
            reinterpret_cast<void *>(cleanup_lo));
        if (cleanup_lo == current_lo)
          break;
        cleanup_lo = (cleanup_lo & ~(kArenaStep - 1)) + kArenaStep;
      }
      return -ENOMEM;
    }
    current_lo = boundary;
    boundary += kArenaStep;
  }
  return 0;
}

[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::ErrorOr<void *>
finish_acquire_at_reserved(void *base, size_t bytes, RegionKind kind,
                           const AcquireMeta &meta) {
  if (base == nullptr)
    return ::LIBC_NAMESPACE::Error{ENOMEM};

  // Multi-arena handling. The scout reserves one big VAD; the per-
  // arena dispatcher splits the registration into N sub-envelopes,
  // but `commit_replace` needs each slice to be its own VAD. Pre-
  // split at every arena boundary before dispatch so the size-match
  // constraint holds for every per-arena commit.
  const bool multi_arena = !fits_one_arena(VaRange{base, bytes});
  if (multi_arena) {
    int srx = split_reserved_at_arena_boundaries(base, bytes);
    if (LIBC_UNLIKELY(srx != 0)) {
      // The helper already cleaned up every VAD it created.
      return ::LIBC_NAMESPACE::Error{-srx};
    }
  }

  CommitIntent intent;
  intent.op = OpKind::AcquireAtReserved;
  intent.range = VaRange{base, bytes};
  intent.kind = kind;
  intent.meta = meta;

  int rc = dispatch_per_arena_op(intent);
  if (rc != 0) {
    // Envelope failed. The Acquire-at-reserved plan never frees the
    // placeholder on its own — that contract belongs to the caller
    // (us).
    //
    // Multi-arena composite is best-effort per the public `acquire`
    // contract: some slices may have committed before the failing
    // one. Release any committed slices via `va_tracker::release`
    // (Stage 2 frees their kernel state and unregisters the descs)
    // and free every remaining placeholder VAD by walking arena
    // boundaries. Single-arena collapses to a direct free of the one
    // VAD.
    if (multi_arena) {
      (void)::LIBC_NAMESPACE::windows::va_tracker::release(
          VaRange{base, bytes});
      free_placeholders_in_range(base, bytes);
    } else {
      (void)nt_pal::free_placeholder(base);
    }
    return ::LIBC_NAMESPACE::Error{rc < 0 ? -rc : rc};
  }

  return base;
}

} // namespace

::LIBC_NAMESPACE::ErrorOr<RegionRef>
acquire(VaRange range, RegionKind kind, const AcquireMeta &meta) {
  // Public-side validation: base + bytes must both be page-aligned
  // and non-wrapping. The hint may sit at any page-aligned address;
  // when it is not also alloc-granularity-aligned, the page-aligned-
  // hint path below shaves the unused prefix off the surrounding
  // alloc granule so the user observes their requested address even
  // though NT itself can only place placeholders at alloc granularity.
  if (LIBC_UNLIKELY(!range_valid_interior(range)))
    return ::LIBC_NAMESPACE::Error{EINVAL};

  uintptr_t base_addr = range.lo();
  if ((base_addr & (kAllocGranularity - 1)) != 0) {
    // Page-aligned hint that is not alloc-aligned. NT cannot reserve
    // at this base directly, but a placeholder split is page-granular
    // — reserve the enclosing alloc granule, shave the prefix to
    // MEM_FREE, and hand the remaining placeholder (anchored at the
    // caller's exact hint) to the AcquireAtReserved envelope.
    //
    // Race semantics: the reserve+shave pair holds a placeholder at
    // the enclosing granule before yielding back. A concurrent
    // acquirer at any address inside that granule observes
    // `STATUS_CONFLICTING_ADDRESSES` on its own reserve and gets
    // EEXIST, same as the alloc-aligned hint path.
    const uintptr_t rounded_down =
        base_addr & ~(kAllocGranularity - 1);
    const size_t prefix_bytes =
        static_cast<size_t>(base_addr - rounded_down);
    const size_t total_reserve = prefix_bytes + range.bytes;
    void *reserved = nt_pal::reserve_placeholder_at(
        reinterpret_cast<void *>(rounded_down), total_reserve);
    if (reserved == nullptr)
      return ::LIBC_NAMESPACE::Error{EEXIST};
    if (LIBC_UNLIKELY(!nt_pal::split_placeholder(reserved, prefix_bytes))) {
      (void)nt_pal::free_placeholder(reserved);
      return ::LIBC_NAMESPACE::Error{ENOMEM};
    }
    PVOID prefix_p = reserved;
    SIZE_T prefix_size = prefix_bytes;
    (void)::NtFreeVirtualMemory(NtCurrentProcess(), &prefix_p,
                                &prefix_size, MEM_RELEASE);
    // `finish_acquire_at_reserved` handles multi-arena pre-arena-
    // boundary splits and the per-slice envelope shrink that
    // releases the over-reserve suffix on the last slice.
    auto result = finish_acquire_at_reserved(
        reinterpret_cast<void *>(base_addr), range.bytes, kind, meta);
    if (!result.has_value())
      return ::LIBC_NAMESPACE::Error{result.error()};
    return resolve(range.start);
  }

  // Alloc-aligned base: the envelope itself reserves the placeholder
  // per-arena, and the per-arena dispatcher decomposes multi-arena
  // ranges into independent atomic envelopes (each sub-envelope's
  // slice is shrunk independently within its own arena, so a sub-
  // granular tail spanning a 4 GiB boundary works without
  // coordination).
  CommitIntent intent;
  intent.op = OpKind::Acquire;
  intent.range = range;
  intent.kind = kind;
  intent.meta = meta;
  int rc = dispatch_per_arena_op(intent);
  if (rc != 0) {
    // Multi-arena composite failure: some per-arena sub-envelopes
    // may have committed and registered before the failing one.
    // Roll them back via `release` so the caller never observes a
    // partial multi-arena acquire. The failed sub-envelope's own
    // state was rolled back inside the envelope (its `reserve_first
    // == true` path frees its placeholder on commit failure, or
    // `backing_kill_and_retire` runs via the provisional list on
    // post-commit failures); slices that never ran were never
    // reserved. No `free_placeholders_in_range` walk is needed for
    // `OpKind::Acquire` for the same reason — each slice owns its
    // own reservation, and that reservation is either committed-
    // and-registered (handled by `release`) or already freed.
    if (range.bytes != 0)
      (void)release(range);
    return ::LIBC_NAMESPACE::Error{rc < 0 ? -rc : rc};
  }
  return resolve(range.start);
}

::LIBC_NAMESPACE::ErrorOr<void *>
acquire_kernel_chosen(size_t bytes, RegionKind kind,
                      const AcquireMeta &meta) {
  if (LIBC_UNLIKELY(bytes == 0 ||
                    (bytes & (kPageGranularity - 1)) != 0))
    return ::LIBC_NAMESPACE::Error{EINVAL};

  // Unconstrained scout — kernel picks any MEM_FREE base across the
  // full user VA, rounding the request up to alloc granularity. The
  // envelope's `AcquireAtReserved` path shrinks the placeholder to
  // exactly `bytes` before commit (see the
  // `op.reserve_bytes > op.bytes` branch in `run_envelope`), so a
  // sub-64 KiB request returns the over-reservation pad to MEM_FREE
  // without any pre-envelope NT work in the public path.
  return finish_acquire_at_reserved(
      nt_pal::reserve_placeholder(nullptr, bytes), bytes, kind, meta);
}

::LIBC_NAMESPACE::ErrorOr<void *>
acquire_kernel_chosen_32bit(size_t bytes, RegionKind kind,
                            const AcquireMeta &meta) {
  if (LIBC_UNLIKELY(bytes == 0 ||
                    (bytes & (kPageGranularity - 1)) != 0))
    return ::LIBC_NAMESPACE::Error{EINVAL};

  // Low-2-GiB-constrained scout for `MAP_32BIT`. Same envelope
  // contract as the unconstrained sibling — the placeholder is
  // never visible as MEM_FREE between the scout and the commit, so
  // a concurrent MAP_32BIT request cannot win the same VA. Sub-
  // granularity shrink runs inside the envelope.
  return finish_acquire_at_reserved(
      nt_pal::reserve_placeholder_32bit(bytes), bytes, kind, meta);
}

int release(VaRange range) {
  CommitIntent intent;
  intent.op = OpKind::Release;
  intent.range = range;
  int rc = dispatch_per_arena_op(intent);
  return rc < 0 ? -rc : rc;
}

int replace(VaRange range, RegionKind kind, const AcquireMeta &meta) {
  CommitIntent intent;
  intent.op = OpKind::Replace;
  intent.range = range;
  intent.kind = kind;
  intent.meta = meta;
  int rc = dispatch_per_arena_op(intent);
  return rc < 0 ? -rc : rc;
}

int mutate(VaRange range, DescMutator mutator, void *ctx, DWORD prot_change,
           bool commit_if_uncommitted_accessible, int numa_node) {
  // The substrate-owned view_prot / PROT_DIVERGED logic on the
  // commit-on-uncommitted path makes a null mutator legal — the
  // caller has nothing to add beyond the substrate's coverage
  // bookkeeping. On the plain-mutate path the mutator is still
  // required (every locked desc must have a mutator-applied clone
  // for Swap publish).
  if (LIBC_UNLIKELY(mutator == nullptr && !commit_if_uncommitted_accessible))
    return EINVAL;
  CommitIntent intent;
  intent.op = OpKind::Mutate;
  intent.range = range;
  intent.mutator = mutator;
  intent.mutator_ctx = ctx;
  intent.prot_change = prot_change;
  intent.commit_if_uncommitted_accessible = commit_if_uncommitted_accessible;
  intent.numa_node = numa_node;
  int rc = dispatch_per_arena_op(intent);
  return rc < 0 ? -rc : rc;
}

int split(void *boundary) {
  uintptr_t b = reinterpret_cast<uintptr_t>(boundary);
  if ((b & (kPageGranularity - 1)) != 0)
    return EINVAL;

  // The envelope range must lie entirely inside one arena (4 GiB leaf),
  // or `run_envelope`'s `fits_one_arena` check rejects it with `-ENOTSUP`.
  // A naive `[b - 4K, b + 4K)` window straddles an arena boundary
  // whenever `b` is itself 4 GiB-aligned, silently breaking the
  // single-cover, boundary-strictly-interior contract.
  //
  // Geometry: when `b` is 4 GiB-aligned, no desc can straddle `b` (the
  // arena edge is itself a hard boundary in the ART/skiplist routing),
  // so split at an arena edge is meaningless and returns `EINVAL`.
  // Otherwise the 2 × 4 KiB window is centred at `b` and clamped to
  // the arena interior; `b` must remain strictly inside the window so
  // `build_plan_split` can verify the boundary lies inside one captured
  // desc.
  uintptr_t arena_lo = b & ~(uintptr_t{0xFFFFFFFFu});
  uintptr_t arena_hi_excl = arena_lo + (uintptr_t{1} << 32);
  if (b == arena_lo)
    return EINVAL;
  uintptr_t lo = b - kPageGranularity;
  uintptr_t hi = b + kPageGranularity;
  if (lo < arena_lo)
    lo = arena_lo;
  if (hi > arena_hi_excl)
    hi = arena_hi_excl;
  if (b <= lo || b >= hi)
    return EINVAL;

  CommitIntent intent;
  intent.op = OpKind::Split;
  intent.range.start = reinterpret_cast<void *>(lo);
  intent.range.bytes = static_cast<size_t>(hi - lo);
  intent.boundary = boundary;
  // Single arena by construction.
  int rc = run_envelope(intent);
  return rc < 0 ? -rc : rc;
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
