//===- va_tracker_transaction_internal.h -- engine-private types *- C++ -*===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared between `va_tracker_transaction.cpp` (frame driver) and
// `va_tracker_execute.cpp` (per-op execute functions). Not exposed
// outside the engine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_TRACKER_TRANSACTION_INTERNAL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_TRACKER_TRANSACTION_INTERNAL_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/memory/desc_backing.h"
#include "src/__support/OSUtil/windows/memory/interval_skiplist.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {
namespace internal {

// NT `MEM_RESERVE_PLACEHOLDER` base-placement granularity. Interior
// placeholder ops (split/release/replace/mutate) accept page granularity.
inline constexpr uintptr_t kAllocGranularity = 64u * 1024u;
inline constexpr uintptr_t kPageGranularity = 4u * 1024u;

// Past this count of consecutive Swap-CAS losses, surface `-EAGAIN`.
inline constexpr uint32_t kCommitRetryCap = 8;

// `AcquireAtReserved` is `Acquire` over a caller-pre-reserved placeholder;
// the engine skips its own pre-commit reserve. Used by `acquire_kernel_chosen`
// so the POSIX layer can let the kernel pick a base without a release /
// re-reserve race window.
enum class OpKind : uint8_t {
  Acquire           = 0,
  Release           = 1,
  Replace           = 2,
  Mutate            = 3,
  Split             = 4,
  AcquireAtReserved = 5,
};
inline constexpr uint32_t kOpKindCount = 6;

struct CommitIntent {
  OpKind        op{OpKind::Acquire};
  VaRange       range{};
  RegionKind    kind{RegionKind::AnonPrivate};
  AcquireMeta   meta{};
  DescMutator   mutator{nullptr};
  void *        mutator_ctx{nullptr};
  DWORD         prot_change{0};
  void *        boundary{nullptr};
  // When set, mutate's post-Swap walks each succ's intent intersection via
  // `nt_pal::RegionWalker` and dispatches per-chunk (committed → protect,
  // uncommitted+accessible → commit_in_reservation or commit_replace[_numa],
  // uncommitted+PROT_NONE → no-op).
  bool          commit_if_uncommitted_accessible{false};
  // `-1` selects unhinted commit_replace. Honoured only on the
  // commit-if-uncommitted path.
  int           numa_node{-1};
  // Set on the mutate path when the caller wants per-chunk gating of
  // `prot_change`. Used by `post_swap_filtered_protect` so restrictive
  // protections (e.g. `PAGE_REVERT_TO_FILE_MAP`, valid only on file-
  // backed CoW pages) skip ineligible chunks instead of failing the
  // per-VAD protect. Honoured only when `commit_if_uncommitted_accessible`
  // is false and `prot_change != 0`.
  MutateChunkFilter chunk_filter{nullptr};
};

enum class Side : uint8_t { Left = 0, Right = 1 };
inline constexpr size_t kSideCount = 2;
inline constexpr Side kSides[kSideCount] = {Side::Left, Side::Right};

[[nodiscard]] LIBC_INLINE constexpr uint32_t side_index(Side s) {
  return static_cast<uint32_t>(s);
}

// OLD identity for one edge-extending backing, snapshotted once under
// LOCKED. Captured on replace/release when a shared backing extends
// past the intent edge. Consumers (edge_split, edge_remap, outside-
// survivor clone, ownership transfer) read the snapshot — they MUST
// NOT re-load `E.backing->placeholder_base` etc., that would
// re-introduce a TOCTOU against a peer envelope's reaper.
struct EdgeIdentity {
  bool present{false};
  DescBacking *backing{nullptr};
  // OLD backing's full extent at capture time.
  uintptr_t backing_lo{0};
  uintptr_t backing_hi{0};
  // Surviving sibling slice — the portion of the OLD extent outside
  // `intent.range` on this edge.
  //   Left:  (backing_lo, intent.range.lo).
  //   Right: (intent.range.hi, backing_hi).
  uintptr_t sibling_lo{0};
  uintptr_t sibling_hi{0};
  BackingShape old_shape{BackingShape::PrivateCommit};
  HANDLE old_section_handle{nullptr};
  // Right edge: anchor.section_offset + (intent.hi - backing_lo).
  LARGE_INTEGER old_section_offset_at_sibling_lo{};
  DWORD old_prot{0};
  // Rejected as `-ENOTSUP` if heterogeneous across the descs sharing
  // the OLD backing — multi-prot sibling re-map would need multiple
  // views per edge.
  RegionKind kind_for_desc{RegionKind::AnonPrivate};
  uint16_t flags_for_desc{0};
};
using EdgeSet = EdgeIdentity[kSideCount];

// Cap = max backings allocated per execute pass: inside_commit + 2 edge
// siblings. Interior succs cannot have backings extending past edges by
// VA contiguity, so this bound is structural.
//
// `Kind` selects the rollback dispatch. `Owner` (the inside backing
// from `commit_inside_range`) owns its newly-created placeholder /
// view; rollback tears it down via `backing_kill_and_retire`. `Sibling`
// (from `edge_remap_one`) borrows kernel state from a still-LIVE OLD
// wider backing; rollback metadata-only-retires it via
// `backing_retire_metadata_only`. The distinction is kept in `prov`
// rather than on `DescBacking` itself — see
// `backing_retire_metadata_only` for the race that an on-backing flag
// would expose.
struct ProvisionalList {
  enum class Kind : uint8_t { Owner, Sibling };
  struct Entry {
    DescBacking *backing{nullptr};
    Kind kind{Kind::Owner};
  };
  static constexpr uint32_t kCap = 3;
  Entry items[kCap]{};
  uint32_t count{0};

  LIBC_INLINE void push_owner(DescBacking *b) {
    LIBC_ASSERT(count < kCap && "provisional list overflow");
    items[count++] = Entry{b, Kind::Owner};
  }
  LIBC_INLINE void push_sibling(DescBacking *b) {
    LIBC_ASSERT(count < kCap && "provisional list overflow");
    items[count++] = Entry{b, Kind::Sibling};
  }
  LIBC_INLINE void clear() {
    for (uint32_t i = 0; i < count; ++i)
      items[i] = Entry{};
    count = 0;
  }
};

using ExecuteFn = int (*)(const CommitIntent &intent, Arena *arena,
                          const LockedSet &locked, NewNodes &new_nodes,
                          ProvisionalList &prov);
using PostSwapFn = int (*)(const CommitIntent &intent,
                           const LockedSet &locked);
using ValidateRangeFn = bool (*)(VaRange r);

[[nodiscard]] int execute_acquire(const CommitIntent &, Arena *,
                                   const LockedSet &, NewNodes &,
                                   ProvisionalList &);
[[nodiscard]] int execute_acquire_at_reserved(const CommitIntent &, Arena *,
                                               const LockedSet &, NewNodes &,
                                               ProvisionalList &);
[[nodiscard]] int execute_release(const CommitIntent &, Arena *,
                                   const LockedSet &, NewNodes &,
                                   ProvisionalList &);
[[nodiscard]] int execute_replace(const CommitIntent &, Arena *,
                                   const LockedSet &, NewNodes &,
                                   ProvisionalList &);
[[nodiscard]] int execute_mutate(const CommitIntent &, Arena *,
                                  const LockedSet &, NewNodes &,
                                  ProvisionalList &);
[[nodiscard]] int execute_split(const CommitIntent &, Arena *,
                                 const LockedSet &, NewNodes &,
                                 ProvisionalList &);

[[nodiscard]] int post_swap_mutate(const CommitIntent &, const LockedSet &);
void post_swap_ownership_transfer(const LockedSet &, const NewNodes &);
void reap_old_backings(Arena *, VaRange, const LockedSet &, const NewNodes &);

// Inverse of demote+split+commit+republish for SectionView OW. Unmap-
// preserve each prov entry's kernel state, coalesce the resulting run
// of L+M+R placeholders, map_section_replace OW.section back over the
// wider extent. On success caller must metadata-only-retire every prov
// entry — their kernel state is now part of OW's restored view, and
// `backing_kill_and_retire` on an Owner would unmap a fragment of it
// via NT's interior-pointer view-unmap. Returns false on:
//   * PrivateCommit OW — `coalesce_placeholders` over the auto-split
//     L_committed / M_placeholder / R_committed mix is rejected with
//     STATUS_CONFLICTING_ADDRESSES, and `preserve_to_placeholder` on
//     L/R would discard the caller's outside-survivor content.
//   * Empty `locked`, null OW handle, or any kernel failure mid-phase.
// Idempotent on partial prov state: entries with null `placeholder_base`
// (incomplete backing alloc) are skipped.
[[nodiscard]] bool try_restore_old_section_view(
    const LockedSet &locked, VaRange intent,
    const ProvisionalList &prov);

// `kernel_already_restored == true` forces every entry through
// metadata-only retire regardless of `Kind`. Pass true when
// `try_restore_old_section_view` succeeded — kill_and_retire on an
// Owner would otherwise unmap a fragment of OW's restored view.
void rollback_provisional(ProvisionalList &prov,
                           bool kernel_already_restored = false);

[[nodiscard]] bool range_valid_acquire(VaRange r);
[[nodiscard]] bool range_valid_interior(VaRange r);

} // namespace internal
} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_TRACKER_TRANSACTION_INTERNAL_H
