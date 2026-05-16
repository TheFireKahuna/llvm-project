//===- va_tracker_execute.cpp - per-op kernel programs --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-op execute functions, post-Swap fixups, OLD-backing reaper.
// Frame driver and public API surface live in `va_tracker_transaction.cpp`.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_transaction_internal.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/memory/desc_backing.h"
#include "src/__support/OSUtil/windows/memory/interval_skiplist.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
#include "src/__support/OSUtil/windows/nt_pal/protect.h"
#include "src/__support/OSUtil/windows/nt_pal/query.h"
#include "src/__support/OSUtil/windows/nt_pal/section.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

using internal::CommitIntent;
using internal::EdgeIdentity;
using internal::EdgeSet;
using internal::kAllocGranularity;
using internal::kPageGranularity;
using internal::kSideCount;
using internal::ProvisionalList;
using internal::Side;
using internal::side_index;

namespace {

[[nodiscard]] LIBC_INLINE constexpr bool kind_is_section_backed(RegionKind k) {
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

[[nodiscard]] LIBC_INLINE constexpr RegionShape shape_for_kind(RegionKind k) {
  return kind_is_section_backed(k) ? RegionShape::FILE_VIEW_MONO
                                    : RegionShape::ANON_PLACEHOLDER;
}

[[nodiscard]] LIBC_INLINE constexpr BackingShape
backing_shape_for_kind(RegionKind k) {
  return kind_is_section_backed(k) ? BackingShape::SectionView
                                    : BackingShape::PrivateCommit;
}

// FILE_VIEW_MONO is shared by every section-backed RegionKind, so this
// rebind is lossy across (FileShared, ShmPosix, ShmSysV). Callers that
// need exact preservation must not partial-replace shared backings
// across disparate kinds.
[[nodiscard]] LIBC_INLINE constexpr RegionKind
kind_from_shape(uint16_t shape_word) {
  switch (static_cast<RegionShape>(shape_word)) {
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

[[nodiscard]] LIBC_INLINE BackingRef encode_backing_ref(DescBacking *b) {
  if (b == nullptr)
    return kBackingRefNull;
  // ACQUIRE: subsequent `deref_backing_raw` consumers compare generation
  // for slot-recycling ABA defence.
  uint32_t gen = b->generation.load(cpp::MemoryOrder::ACQUIRE);
  return make_backing_ref(static_cast<uint16_t>(b->cached_chunk_id),
                          static_cast<uint16_t>(b->cached_slot_idx), gen);
}

[[nodiscard]] int append_new_node(NewNodes &out, Arena *arena, uintptr_t lo,
                                   uintptr_t hi, RegionDesc *desc) {
  if (lo >= hi)
    return -EINVAL;
  SkiplistNodeBase *n = bucket_alloc_node(sample_node_height(), arena);
  if (n == nullptr) {
    region_desc_release(desc);
    return -ENOMEM;
  }
  n->lo = lo;
  n->hi = hi;
  // RELEASE: the clone phase and the reaper read `value` off
  // `NewNodes` directly, before Swap publishes the node into the chain.
  n->value.store(desc, cpp::MemoryOrder::RELEASE);
  if (!out.push(n, /*cleanup_value_if_unpublished=*/true)) {
    n->value.store(nullptr, cpp::MemoryOrder::RELAXED);
    g_va_tracker_skiplist_domain.retire(n);
    region_desc_release(desc);
    return -ENOMEM;
  }
  return 0;
}

[[nodiscard]] LIBC_INLINE bool
locked_has_edge_straddler(const LockedSet &locked, uintptr_t edge_lo,
                          uintptr_t edge_hi) {
  for (uint32_t i = 0; i < locked.count; ++i) {
    SkiplistNodeBase *n = locked.at(i);
    if (n == nullptr)
      continue;
    if ((n->lo < edge_lo && n->hi > edge_lo) ||
        (n->lo < edge_hi && n->hi > edge_hi))
      return true;
  }
  return false;
}

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

[[nodiscard]] bool
locked_uniform_for_backing(const LockedSet &locked, DescBacking *target,
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
    if (deref_backing_raw(d->backing_ref) != target)
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
    if (prot != out_prot || flags != out_flags ||
        shape_word != out_shape_word)
      return false;
  }
  return found;
}

// Identify the leftmost/rightmost backings whose extent extends past
// `intent` (at most one each by VA contiguity), then snapshot every
// field the kernel-touching phases need. Errors:
//   `-ENOTSUP` for two distinct backings on one side (cross-chain
//   discontinuity) or heterogeneous prot/flags/shape across the descs
//   sharing one edge-extending backing.
[[nodiscard]] int capture_edges(const LockedSet &locked, VaRange intent,
                                 EdgeSet &out) {
  DescBacking *edge_b[kSideCount] = {nullptr, nullptr};
  uintptr_t edge_lo[kSideCount] = {0, 0};
  uintptr_t edge_hi[kSideCount] = {0, 0};

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
    if (b_lo < intent.lo()) {
      DescBacking *&slot = edge_b[side_index(Side::Left)];
      if (slot != nullptr && slot != b)
        return -ENOTSUP;
      slot = b;
      edge_lo[side_index(Side::Left)] = b_lo;
      edge_hi[side_index(Side::Left)] = b_hi;
    }
    if (b_hi > intent.hi()) {
      DescBacking *&slot = edge_b[side_index(Side::Right)];
      if (slot != nullptr && slot != b)
        return -ENOTSUP;
      slot = b;
      edge_lo[side_index(Side::Right)] = b_lo;
      edge_hi[side_index(Side::Right)] = b_hi;
    }
  }

  for (uint32_t si = 0; si < kSideCount; ++si) {
    DescBacking *b = edge_b[si];
    if (b == nullptr)
      continue;
    DWORD prot = 0;
    uint16_t flags = 0;
    uint16_t shape_word = 0;
    if (!locked_uniform_for_backing(locked, b, prot, flags, shape_word))
      return -ENOTSUP;
    RegionDesc *anchor = locked_find_desc_at_lo(locked, edge_lo[si]);
    if (anchor == nullptr)
      return -ENOTSUP;

    EdgeIdentity &E = out[si];
    E.present = true;
    E.backing = b;
    E.backing_lo = edge_lo[si];
    E.backing_hi = edge_hi[si];
    E.old_shape = b->shape;
    E.old_section_handle =
        b->section_handle.load(cpp::MemoryOrder::ACQUIRE);
    E.old_prot = prot;
    E.kind_for_desc = kind_from_shape(shape_word);
    E.flags_for_desc = flags;

    if (kSides[si] == Side::Left) {
      E.sibling_lo = edge_lo[si];
      E.sibling_hi = intent.lo();
      E.old_section_offset_at_sibling_lo = anchor->section_offset;
    } else {
      E.sibling_lo = intent.hi();
      E.sibling_hi = edge_hi[si];
      LARGE_INTEGER off = anchor->section_offset;
      off.QuadPart += static_cast<int64_t>(intent.hi() - edge_lo[si]);
      E.old_section_offset_at_sibling_lo = off;
    }
  }
  return 0;
}

// Section-view: whole-view unmap, idempotent across shared-backing
// succs — `STATUS_NOT_MAPPED_VIEW` on a duplicate is tolerated.
// Private commit: ONLY inside-intent succs demote. `decommit_preserve`
// on an outside survivor would lose its data (private commits have no
// backing storage). The outside slice stays committed; the clone phase
// republishes it onto a fresh sibling backing.
[[nodiscard]] int demote_by_shape(const LockedSet &locked, VaRange intent) {
  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *old_node = locked.at(k);
    if (old_node == nullptr)
      continue;
    RegionDesc *d = old_node->value.load(cpp::MemoryOrder::ACQUIRE);
    if (d == nullptr)
      continue;
    NTSTATUS st;
    switch (d->current_shape()) {
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
      // Edge-straddler: demote only the inside slice. NT's
      // `preserve_to_placeholder` is page-granular and auto-splits
      // the VAD into committed-left / decommitted-middle /
      // committed-right; the outside slice's data survives across
      // the envelope and is republished by the clone phase.
      const uintptr_t lo =
          old_node->lo > intent.lo() ? old_node->lo : intent.lo();
      const uintptr_t hi =
          old_node->hi < intent.hi() ? old_node->hi : intent.hi();
      if (lo >= hi) {
        st = STATUS_SUCCESS;
        break;
      }
      st = nt_pal::preserve_to_placeholder(
          reinterpret_cast<void *>(lo), static_cast<size_t>(hi - lo));
      break;
    }
    default:
      return -ENOTSUP;
    }
    if (!NT_SUCCESS(st))
      return -EFAULT;
  }
  return 0;
}

// `coalesce_placeholders` tolerates MEM_FREE gaps in its input, but the
// subsequent `commit_replace` then fails with
// `STATUS_CONFLICTING_ADDRESSES` because the FREE pages aren't in
// placeholder state. Reserve over each gap so the entire intent becomes
// a uniform placeholder span.
//
// Pathological cap; sustained breach signals a callsite that should
// fragment its request rather than pile every hole into a single replace.
inline constexpr uint32_t kMaxGapResv = 16;

[[nodiscard]] int fill_free_gaps(VaRange intent) {
  void *resvs[kMaxGapResv];
  uint32_t count = 0;

  nt_pal::RegionWalker walk(reinterpret_cast<void *>(intent.lo()),
                            static_cast<SIZE_T>(intent.bytes));
  if (!walk)
    return -EFAULT;
  while (walk.next()) {
    if (walk.entry->State != MEM_FREE)
      continue;
    // `RegionWalker` already clipped chunk/chunk_size to `intent`, so
    // an MBI extending past the intent edges is automatically narrowed
    // to the FREE gap inside our authority — no manual clipping here.
    void *r = nt_pal::reserve_placeholder(walk.chunk, walk.chunk_size);
    if (r == nullptr) {
      for (uint32_t i = 0; i < count; ++i)
        (void)nt_pal::free_placeholder(resvs[i]);
      return -EFAULT;
    }
    if (count >= kMaxGapResv) {
      (void)nt_pal::free_placeholder(r);
      for (uint32_t i = 0; i < count; ++i)
        (void)nt_pal::free_placeholder(resvs[i]);
      return -ENOMEM;
    }
    resvs[count++] = r;
  }
  return 0;
}

// Relies on the inside slice's VAD being commit-replaced without
// `MEM_WRITE_WATCH`; the kernel rejects every sub-range form of
// `NtFreeVirtualMemory` on a WW-armed VAD with
// `STATUS_FREE_VM_NOT_AT_BASE`. Section-view shapes need an unmap +
// split + free dance not wired here.
[[nodiscard]] int release_inside_slice_kernel(const LockedSet &locked,
                                               VaRange intent) {
  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *old_node = locked.at(k);
    if (old_node == nullptr)
      continue;
    if (old_node->lo < intent.lo() || old_node->hi > intent.hi())
      continue;
    RegionDesc *d = old_node->value.load(cpp::MemoryOrder::ACQUIRE);
    if (d == nullptr)
      continue;
    if (d->current_shape() != RegionShape::ANON_PLACEHOLDER)
      return -ENOTSUP;
    if (!nt_pal::interior_release(reinterpret_cast<void *>(old_node->lo),
                                   static_cast<size_t>(old_node->hi -
                                                       old_node->lo)))
      return -EFAULT;
  }
  return 0;
}

// Right-edge source address:
//   same backing both edges → after left split, the second placeholder
//   begins at intent.lo; right split targets intent.lo.
//   different backings → right split targets right edge's `backing_lo`
//   (captured by `capture_edges`; no re-walk).
[[nodiscard]] int split_section_view_placeholders_at_edges(const EdgeSet &edges,
                                           VaRange intent) {
  const EdgeIdentity &L = edges[side_index(Side::Left)];
  const EdgeIdentity &R = edges[side_index(Side::Right)];

  if (L.present && L.old_shape == BackingShape::SectionView) {
    if (!nt_pal::split_placeholder(
            reinterpret_cast<void *>(L.backing_lo),
            static_cast<size_t>(intent.lo() - L.backing_lo)))
      return -EFAULT;
  }
  if (R.present && R.old_shape == BackingShape::SectionView) {
    const bool same_both = L.present &&
                           L.old_section_handle == R.old_section_handle &&
                           L.old_section_handle != nullptr;
    uintptr_t src = same_both ? intent.lo() : R.backing_lo;
    if (!nt_pal::split_placeholder(
            reinterpret_cast<void *>(src),
            static_cast<size_t>(intent.hi() - src)))
      return -EFAULT;
  }
  return 0;
}

// Three callers, three placeholder provenances:
//   Acquire           — we reserve here at alloc granularity; shrink to
//                        exact when range is page-but-not-alloc aligned
//                        (or wider when meta.placeholder_size set for
//                        brk / posix_memalign / mremap headroom).
//   AcquireAtReserved — caller pre-reserved at alloc granularity (the
//                        kernel-chosen scout); we still own the shrink.
//   Replace           — caller's prior demote+coalesce produced an
//                        exact-size placeholder at intent.range; no
//                        shrink and no reserve.
enum class CommitPath : uint8_t { Acquire, AcquireAtReserved, Replace };

// Pushes the backing onto `prov` before any kernel work so a later
// failure rolls back via `backing_kill_and_retire`.
template <CommitPath Path>
[[nodiscard]] int commit_inside_range(const CommitIntent &i, Arena *arena,
                                       NewNodes &new_nodes,
                                       ProvisionalList &prov) {
  DescBacking *backing = backing_alloc();
  if (backing == nullptr)
    return -ENOMEM;
  prov.push_owner(backing);

  const bool section_backed = kind_is_section_backed(i.kind);
  void *identity_base = i.meta.placeholder_base != nullptr
                            ? i.meta.placeholder_base
                            : reinterpret_cast<void *>(i.range.lo());
  const size_t identity_bytes =
      i.meta.placeholder_size != 0 ? i.meta.placeholder_size : i.range.bytes;
  // Replace's placeholder is exact-size from coalesce; no shrink. The
  // acquire family rounds up to alloc granularity (or honours an
  // explicit caller override).
  const size_t reserve_bytes =
      Path == CommitPath::Replace
          ? identity_bytes
          : (i.meta.placeholder_size != 0
                 ? i.meta.placeholder_size
                 : ((i.range.bytes + (kAllocGranularity - 1)) &
                    ~(kAllocGranularity - 1)));
  const uint32_t identity_pages =
      static_cast<uint32_t>(identity_bytes / kPageGranularity);

  if constexpr (Path == CommitPath::Acquire) {
    void *reserved = nt_pal::reserve_placeholder(identity_base, reserve_bytes);
    if (reserved == nullptr)
      // STATUS_CONFLICTING_ADDRESSES — VA not MEM_FREE. The provisional
      // backing's kernel-state fields are still nullptr, so the
      // rollback path's free/close calls all no-op cleanly.
      return -EEXIST;
    identity_base = reserved;
  }

  if (reserve_bytes > identity_bytes) {
    if (LIBC_UNLIKELY(
            !nt_pal::split_placeholder(identity_base, identity_bytes))) {
      if constexpr (Path == CommitPath::Acquire)
        (void)nt_pal::free_placeholder(identity_base);
      return -ENOMEM;
    }
    PVOID pad = static_cast<char *>(identity_base) + identity_bytes;
    SIZE_T pad_size = reserve_bytes - identity_bytes;
    (void)::NtFreeVirtualMemory(NtCurrentProcess(), &pad, &pad_size,
                                MEM_RELEASE);
  }

  void *commit_base = reinterpret_cast<void *>(i.range.lo());
  LARGE_INTEGER sec_off;
  sec_off.QuadPart = static_cast<int64_t>(i.meta.section_offset);
  NTSTATUS st = section_backed
                    ? nt_pal::map_section_replace(i.meta.section_handle,
                                                   commit_base, i.range.bytes,
                                                   sec_off, i.meta.view_prot)
                    : nt_pal::commit_replace(commit_base, i.range.bytes,
                                              i.meta.view_prot);
  if (!NT_SUCCESS(st)) {
    // Backing's kernel-state fields are still null, so rollback's
    // `backing_kill_and_retire` skips `free_placeholder`. We own the
    // release here: Acquire — the placeholder we just reserved;
    // Replace — the one the envelope's prior demote+coalesce produced;
    // AcquireAtReserved — the one the caller pre-reserved (caller-owned;
    // caller's failure-path frees it on -ENOMEM return).
    if constexpr (Path == CommitPath::Acquire)
      (void)nt_pal::free_placeholder(identity_base);
    else if constexpr (Path == CommitPath::Replace)
      (void)nt_pal::free_placeholder(commit_base);
    return -ENOMEM;
  }

  backing_set_kernel_state(backing, identity_base, identity_pages,
                            backing_shape_for_kind(i.kind),
                            i.meta.section_handle, i.meta.file_handle);

  RegionDesc *desc = region_desc_alloc();
  if (desc == nullptr)
    return -ENOMEM;
  desc->backing_ref = encode_backing_ref(backing);
  desc->section_offset.QuadPart = static_cast<int64_t>(i.meta.section_offset);
  // RELEASE pairs with ACQUIRE in any subsequent chain reader.
  desc->shape.store(static_cast<uint16_t>(shape_for_kind(i.kind)),
                    cpp::MemoryOrder::RELEASE);
  desc->view_prot = i.meta.view_prot;
  desc->flags.store(i.meta.flags, cpp::MemoryOrder::RELEASE);
  desc->numa_interleave_mask = 0;
  return append_new_node(new_nodes, arena, i.range.lo(), i.range.hi(), desc);
}

// Coalesce ONLY when the post-demote intent contains more than one VAD.
// Single succ fully covering intent → one VAD, no coalesce needed.
[[nodiscard]] bool replace_needs_coalesce(const LockedSet &locked,
                                           VaRange intent) {
  if (locked.count != 1)
    return true;
  SkiplistNodeBase *only = locked.at(0);
  return only == nullptr || only->lo > intent.lo() || only->hi < intent.hi();
}

// Sibling backings carry no handle ownership: the OLD wider backing
// retains section/file handles and the reaper closes them. The kernel's
// view-section internal reference keeps the section alive across that
// close. Private commit needs no kernel work — the surviving slice
// stayed committed across `demote_by_shape`. Pushed as Kind::Sibling
// so a rollback retires this metadata-only without tearing down the
// L/R kernel state the still-LIVE OLD chain claims.
[[nodiscard]] int edge_remap_one(const EdgeIdentity &E,
                                  ProvisionalList &prov,
                                  DescBacking *&out_backing) {
  out_backing = nullptr;
  DescBacking *backing = backing_alloc();
  if (backing == nullptr)
    return -ENOMEM;
  prov.push_sibling(backing);

  void *base = reinterpret_cast<void *>(E.sibling_lo);
  size_t bytes = static_cast<size_t>(E.sibling_hi - E.sibling_lo);
  uint32_t pages = static_cast<uint32_t>(bytes / kPageGranularity);

  if (E.old_shape == BackingShape::SectionView) {
    NTSTATUS st = nt_pal::map_section_replace(
        E.old_section_handle, base, bytes,
        E.old_section_offset_at_sibling_lo, E.old_prot);
    if (!NT_SUCCESS(st))
      return -EFAULT;
  }

  backing_set_kernel_state(backing, base, pages, E.old_shape,
                            /*section_handle=*/nullptr,
                            /*file_handle=*/nullptr);
  out_backing = backing;
  return 0;
}

// Up to three clones per locked succ:
//   left-outside [src.lo, intent.lo)  — non-mutated
//   inside       [max(src.lo,intent.lo), min(src.hi,intent.hi)) — mutated
//   right-outside [intent.hi, src.hi) — non-mutated
// `clone_region_desc_for_fragment` shifts section_offset when frag_lo
// > src.lo. view_prot is not updated — the kernel is the source of
// truth for current per-page protection; post-Swap protect emits the
// only visible side effect, clipped to the inside slice.
[[nodiscard]] int clone_with_mutator(const LockedSet &locked, Arena *arena,
                                      VaRange intent, DescMutator mutator,
                                      void *mutator_ctx,
                                      NewNodes &new_nodes) {
  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *src_node = locked.at(k);
    if (src_node == nullptr)
      continue;
    RegionDesc *src = src_node->value.load(cpp::MemoryOrder::ACQUIRE);
    if (src == nullptr)
      continue;

    const uintptr_t inside_lo =
        src_node->lo > intent.lo() ? src_node->lo : intent.lo();
    const uintptr_t inside_hi =
        src_node->hi < intent.hi() ? src_node->hi : intent.hi();

    if (src_node->lo < intent.lo()) {
      RegionDesc *c = clone_region_desc_for_fragment(src, src_node->lo,
                                                      src_node->lo);
      if (c == nullptr)
        return -ENOMEM;
      int rc = append_new_node(new_nodes, arena, src_node->lo, inside_lo, c);
      if (rc != 0)
        return rc;
    }
    if (inside_lo < inside_hi) {
      RegionDesc *c =
          clone_region_desc_for_fragment(src, src_node->lo, inside_lo);
      if (c == nullptr)
        return -ENOMEM;
      if (mutator != nullptr)
        mutator(c, mutator_ctx);
      int rc = append_new_node(new_nodes, arena, inside_lo, inside_hi, c);
      if (rc != 0)
        return rc;
    }
    if (src_node->hi > intent.hi()) {
      RegionDesc *c =
          clone_region_desc_for_fragment(src, src_node->lo, inside_hi);
      if (c == nullptr)
        return -ENOMEM;
      int rc = append_new_node(new_nodes, arena, inside_hi, src_node->hi, c);
      if (rc != 0)
        return rc;
    }
  }
  return 0;
}

[[nodiscard]] int clone_split_at_boundary(const LockedSet &locked,
                                           Arena *arena, uintptr_t boundary,
                                           NewNodes &new_nodes) {
  if (locked.count != 1)
    return -ENOENT;
  SkiplistNodeBase *src_node = locked.at(0);
  if (src_node == nullptr)
    return -EINVAL;
  if (boundary <= src_node->lo || boundary >= src_node->hi)
    return -EINVAL;
  RegionDesc *src = src_node->value.load(cpp::MemoryOrder::ACQUIRE);
  if (src == nullptr)
    return -EINVAL;

  RegionDesc *left =
      clone_region_desc_for_fragment(src, src_node->lo, src_node->lo);
  if (left == nullptr)
    return -ENOMEM;
  int rc = append_new_node(new_nodes, arena, src_node->lo, boundary, left);
  if (rc != 0)
    return rc;

  RegionDesc *right =
      clone_region_desc_for_fragment(src, src_node->lo, boundary);
  if (right == nullptr)
    return -ENOMEM;
  return append_new_node(new_nodes, arena, boundary, src_node->hi, right);
}

// One clone per outside slice of each locked succ. Fully-inside succs
// are consumed by `commit_inside_range` and skipped here.
[[nodiscard]] int
clone_outside_survivors(const LockedSet &locked, Arena *arena, VaRange intent,
                         DescBacking *sibling_backings[kSideCount],
                         NewNodes &new_nodes) {
  BackingRef refs[kSideCount] = {kBackingRefNull, kBackingRefNull};
  for (uint32_t si = 0; si < kSideCount; ++si)
    if (sibling_backings[si] != nullptr)
      refs[si] = encode_backing_ref(sibling_backings[si]);

  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *src_node = locked.at(k);
    if (src_node == nullptr)
      continue;
    RegionDesc *src = src_node->value.load(cpp::MemoryOrder::ACQUIRE);
    if (src == nullptr)
      continue;

    const bool has_left = src_node->lo < intent.lo();
    const bool has_right = src_node->hi > intent.hi();
    if (!has_left && !has_right)
      continue;

    if (has_left) {
      const BackingRef ref = refs[side_index(Side::Left)];
      if (ref == kBackingRefNull)
        return -EFAULT;
      const uintptr_t hi =
          src_node->hi < intent.lo() ? src_node->hi : intent.lo();
      RegionDesc *c = clone_region_desc_for_fragment(src, src_node->lo,
                                                      src_node->lo);
      if (c == nullptr)
        return -ENOMEM;
      c->backing_ref = ref;
      int rc = append_new_node(new_nodes, arena, src_node->lo, hi, c);
      if (rc != 0)
        return rc;
    }
    if (has_right) {
      const BackingRef ref = refs[side_index(Side::Right)];
      if (ref == kBackingRefNull)
        return -EFAULT;
      const uintptr_t lo =
          src_node->lo > intent.hi() ? src_node->lo : intent.hi();
      RegionDesc *c = clone_region_desc_for_fragment(src, src_node->lo, lo);
      if (c == nullptr)
        return -ENOMEM;
      c->backing_ref = ref;
      int rc = append_new_node(new_nodes, arena, lo, src_node->hi, c);
      if (rc != 0)
        return rc;
    }
  }
  return 0;
}

[[nodiscard]] int
republish_outside_survivors(const LockedSet &locked, Arena *arena,
                                   VaRange intent, const EdgeSet &edges,
                                   ProvisionalList &prov,
                                   NewNodes &new_nodes) {
  DescBacking *siblings[kSideCount] = {nullptr, nullptr};
  bool any = false;
  for (uint32_t si = 0; si < kSideCount; ++si) {
    if (!edges[si].present)
      continue;
    any = true;
    int rc = edge_remap_one(edges[si], prov, siblings[si]);
    if (rc != 0)
      return rc;
  }
  if (!any)
    return 0;
  return clone_outside_survivors(locked, arena, intent, siblings, new_nodes);
}

} // anonymous namespace

namespace internal {

int execute_acquire(const CommitIntent &i, Arena *arena,
                     const LockedSet &, NewNodes &new_nodes,
                     ProvisionalList &prov) {
  return commit_inside_range<CommitPath::Acquire>(i, arena, new_nodes, prov);
}

int execute_acquire_at_reserved(const CommitIntent &i, Arena *arena,
                                 const LockedSet &, NewNodes &new_nodes,
                                 ProvisionalList &prov) {
  return commit_inside_range<CommitPath::AcquireAtReserved>(i, arena,
                                                             new_nodes, prov);
}

// Intent-edge straddlers are NOT rejected: `demote_by_shape` clips
// the kernel work to the inside slice, and `clone_outside_survivors`
// republishes the outside portion. The prior reject-and-EINVAL path
// raced posix-compliance — a peer mutation between a caller's
// `split()` and this op turned a benign no-op into a phantom EINVAL.
int execute_release(const CommitIntent &i, Arena *arena,
                     const LockedSet &locked, NewNodes &new_nodes,
                     ProvisionalList &prov) {
  EdgeSet edges{};
  int rc = capture_edges(locked, i.range, edges);
  if (rc != 0)
    return rc;
  rc = release_inside_slice_kernel(locked, i.range);
  if (rc != 0)
    return rc;
  return republish_outside_survivors(locked, arena, i.range, edges,
                                            prov, new_nodes);
}

int execute_replace(const CommitIntent &i, Arena *arena,
                     const LockedSet &locked, NewNodes &new_nodes,
                     ProvisionalList &prov) {
  // EXTENDED-edge straddler: a desc crossing the union of all preflight-
  // expanded backings' extents breaks one-iteration convergence of the
  // lock widening. Surface `-ENOTSUP`.
  if (locked_has_edge_straddler(locked, locked.lo, locked.hi))
    return -ENOTSUP;

  EdgeSet edges{};
  int rc = capture_edges(locked, i.range, edges);
  if (rc != 0)
    return rc;
  // FIXME: demote_by_shape and the subsequent split / coalesce phases
  // physically reshape the OLD wider VAD into L+M+R placeholders. The
  // OLD wider backing's `placeholder_base` field still describes the
  // pre-demote layout, so concurrent OLD-chain readers that
  // dereference it during the post-demote / pre-Swap window observe
  // kernel state inconsistent with the desc they're holding.
  rc = demote_by_shape(locked, i.range);
  if (rc != 0)
    return rc;
  rc = fill_free_gaps(i.range);
  if (rc != 0)
    return rc;
  rc = split_section_view_placeholders_at_edges(edges, i.range);
  if (rc != 0)
    return rc;
  if (replace_needs_coalesce(locked, i.range)) {
    NTSTATUS st = nt_pal::coalesce_placeholders(
        reinterpret_cast<void *>(i.range.lo()), i.range.bytes);
    if (!NT_SUCCESS(st))
      return -EFAULT;
  }
  rc = commit_inside_range<CommitPath::Replace>(i, arena, new_nodes, prov);
  if (rc != 0)
    return rc;
  return republish_outside_survivors(locked, arena, i.range, edges,
                                            prov, new_nodes);
}

int execute_mutate(const CommitIntent &i, Arena *arena,
                    const LockedSet &locked, NewNodes &new_nodes,
                    ProvisionalList &) {
  if (locked.count == 0)
    return -ENOENT;
  return clone_with_mutator(locked, arena, i.range, i.mutator,
                             i.mutator_ctx, new_nodes);
}

int execute_split(const CommitIntent &i, Arena *arena,
                   const LockedSet &locked, NewNodes &new_nodes,
                   ProvisionalList &) {
  return clone_split_at_boundary(locked, arena,
                                  reinterpret_cast<uintptr_t>(i.boundary),
                                  new_nodes);
}

} // namespace internal

namespace {

// Source of truth is the desc flag stamped at acquire time. MBI's
// `AllocationProtect` is the fallback for foreign mappings — the
// substrate sees only tracked VAs in its locked succs, so the MBI path
// only fires for foreign-leaning shapes.
[[nodiscard]] LIBC_INLINE DWORD
cow_translate_prot(DWORD new_prot, bool desc_is_cow, DWORD mbi_alloc_prot,
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

// `NtProtectVirtualMemory` is per-VAD atomic; a range spanning multiple
// split placeholders needs per-VAD calls. Clipped to inside-intent —
// edge-straddler outside survivors keep their existing protection per
// POSIX `mprotect` semantics.
[[nodiscard]] int post_swap_protect_basic(const CommitIntent &i,
                                           const LockedSet &locked) {
  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *old_node = locked.at(k);
    if (old_node == nullptr)
      continue;
    const uintptr_t lo =
        old_node->lo > i.range.lo() ? old_node->lo : i.range.lo();
    const uintptr_t hi =
        old_node->hi < i.range.hi() ? old_node->hi : i.range.hi();
    ULONG old_prot = 0;
    if (!nt_pal::protect(reinterpret_cast<void *>(lo),
                         static_cast<size_t>(hi - lo),
                         static_cast<ULONG>(i.prot_change), &old_prot))
      return -EFAULT;
  }
  return 0;
}

// Per-chunk dispatch for the commit-on-uncommitted path:
//   MEM_FREE                                    → -ENOMEM
//   MEM_COMMIT                                  → protect
//   uncommitted + PROT_NONE                     → no-op
//   uncommitted + MEM_MAPPED + accessible       → commit_in_reservation
//   uncommitted + MEM_PRIVATE + accessible      → commit_replace[_numa]
[[nodiscard]] int post_swap_per_chunk_dispatch(const CommitIntent &i,
                                                const LockedSet &locked) {
  auto ws = ::LIBC_NAMESPACE::windows::byte_scratch(4096);
  if (!ws)
    return -ENOMEM;
  const DWORD base_prot = static_cast<DWORD>(i.prot_change);

  for (uint32_t k = 0; k < locked.count; ++k) {
    SkiplistNodeBase *old_node = locked.at(k);
    if (old_node == nullptr)
      continue;
    RegionDesc *old_desc = old_node->value.load(cpp::MemoryOrder::ACQUIRE);
    const bool desc_is_cow =
        old_desc != nullptr && old_desc->has_flag(region_flag::COW);

    const uintptr_t lo =
        old_node->lo > i.range.lo() ? old_node->lo : i.range.lo();
    const uintptr_t hi =
        old_node->hi < i.range.hi() ? old_node->hi : i.range.hi();
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
      // `NtProtect` on uncommitted pages returns STATUS_NOT_COMMITTED;
      // PROT_NONE collapses to a no-op.
      if (eff_prot == PAGE_NOACCESS)
        continue;

      NTSTATUS st;
      if (walk.entry->Type == MEM_MAPPED) {
        st = nt_pal::commit_in_reservation_no_writewatch(
            walk.chunk, walk.chunk_size, eff_prot);
      } else {
        st = STATUS_INVALID_PARAMETER;
        if (i.numa_node >= 0)
          st = nt_pal::commit_replace_numa(walk.chunk, walk.chunk_size,
                                            eff_prot,
                                            static_cast<ULONG>(i.numa_node));
        if (NT_ERROR(st))
          st = nt_pal::commit_replace(walk.chunk, walk.chunk_size, eff_prot);
      }
      if (NT_ERROR(st))
        return -ENOMEM;
    }
  }
  return 0;
}

} // anonymous namespace

namespace internal {

int post_swap_mutate(const CommitIntent &i, const LockedSet &locked) {
  if (i.commit_if_uncommitted_accessible)
    return post_swap_per_chunk_dispatch(i, locked);
  if (i.prot_change != 0)
    return post_swap_protect_basic(i, locked);
  return 0;
}

// Null OLD `placeholder_base` so `backing_kill_and_retire` skips
// `free_placeholder` for the consumed identity. Section/file handles
// stay set — the reaper closes them. The reused-by-NEW guard
// covers a future code path where a sibling re-map shares a backing
// across fragments; today no execute path does that, but the cost is
// trivial and the bug it prevents is silent kernel-VAD orphan.
void post_swap_ownership_transfer(const LockedSet &locked,
                                   const NewNodes &new_nodes) {
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
    bool reused = false;
    for (uint32_t j = 0; j < new_nodes.count; ++j) {
      SkiplistNodeBase *N = new_nodes.node_at(j);
      if (N == nullptr)
        continue;
      RegionDesc *Nd = N->value.load(cpp::MemoryOrder::ACQUIRE);
      if (Nd == nullptr || Nd->backing_ref == kBackingRefNull)
        continue;
      if (deref_backing_raw(Nd->backing_ref) == ob) {
        reused = true;
        break;
      }
    }
    if (reused)
      continue;
    // RELEASE: concurrent readers observing the new null skip safely.
    ob->placeholder_base.store(nullptr, cpp::MemoryOrder::RELEASE);
  }
}

} // namespace internal

namespace {

struct SurvivorScanner {
  DescBacking *target;
  bool found{false};
  LIBC_INLINE void operator()(SkiplistNodeBase *node) {
    if (found || node == nullptr)
      return;
    RegionDesc *d = node->value.load(cpp::MemoryOrder::ACQUIRE);
    if (d == nullptr || d->backing_ref == kBackingRefNull)
      return;
    if (deref_backing_raw(d->backing_ref) == target)
      found = true;
  }
};

[[nodiscard]] bool any_live_referencer_in_range(Arena *arena, uintptr_t lo,
                                                 uintptr_t hi,
                                                 DescBacking *target) {
  SurvivorScanner scanner{target};
  is_walk_range(arena, lo, hi, scanner);
  return scanner.found;
}

} // anonymous namespace

namespace internal {

// After Swap, identify OLD backings whose extent has no live
// referencer and tear them down synchronously.
//
//   (1) state pre-check absorbs duplicates and cross-envelope races
//       in O(1).
//   (2) NewNode survivor check keeps mutate-path clones alive (they
//       share the OLD's backing).
//   (3) Outside-range survivor scan walks only the slivers outside
//       `range`; inside-range descs are in `locked` by construction.
//       For brk's 256 MiB single-shot backing, scanning only the
//       outside slivers turns a per-cursor-advance walk into O(0)
//       when `range == backing extent`.
//   (4) Live → Killed CAS — winner runs `backing_kill_and_retire`.
//       ACQ_REL: the teardown ACQUIRE-exchanges kernel-state fields,
//       and concurrent reader pins must observe Killed before either
//       side reads stale fields.
void reap_old_backings(Arena *arena, VaRange range, const LockedSet &locked,
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

    if (b->state.load(cpp::MemoryOrder::ACQUIRE) != kBackingStateLive)
      continue;

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

    // Null `placeholder_base` means ownership was transferred (replace
    // / release path) — no extent to scan, fall through to the kill.
    uintptr_t b_lo = reinterpret_cast<uintptr_t>(
        b->placeholder_base.load(cpp::MemoryOrder::ACQUIRE));
    if (b_lo != 0) {
      uintptr_t b_hi = b_lo + static_cast<uintptr_t>(b->placeholder_pages) *
                                  kPageGranularity;
      if (b_lo < range.lo() &&
          any_live_referencer_in_range(arena, b_lo, range.lo(), b))
        continue;
      if (range.hi() < b_hi &&
          any_live_referencer_in_range(arena, range.hi(), b_hi, b))
        continue;
    }

    uint8_t expected = kBackingStateLive;
    if (b->state.compare_exchange_strong(expected, kBackingStateKilled,
                                          cpp::MemoryOrder::ACQ_REL,
                                          cpp::MemoryOrder::ACQUIRE))
      backing_kill_and_retire(b);
  }
}

// Inverse of demote+split+commit+republish for SectionView OW; full
// contract on the declaration in `va_tracker_transaction_internal.h`.
bool try_restore_old_section_view(const LockedSet &locked, VaRange intent,
                                   const ProvisionalList &prov) {
  if (locked.count == 0)
    return false;
  SkiplistNodeBase *leftmost = locked.at(0);
  if (leftmost == nullptr)
    return false;
  RegionDesc *leftmost_desc =
      leftmost->value.load(cpp::MemoryOrder::ACQUIRE);
  if (leftmost_desc == nullptr ||
      leftmost_desc->backing_ref == kBackingRefNull)
    return false;

  DescBacking *ow_backing = deref_backing_raw(leftmost_desc->backing_ref);
  if (ow_backing == nullptr ||
      ow_backing->shape != BackingShape::SectionView)
    return false;

  HANDLE ow_section =
      ow_backing->section_handle.load(cpp::MemoryOrder::ACQUIRE);
  if (ow_section == nullptr)
    return false;

  void *ow_base_p =
      ow_backing->placeholder_base.load(cpp::MemoryOrder::ACQUIRE);
  if (ow_base_p == nullptr)
    return false;
  const uintptr_t b_lo = reinterpret_cast<uintptr_t>(ow_base_p);
  const uintptr_t b_hi =
      b_lo + static_cast<uintptr_t>(ow_backing->placeholder_pages) *
                 kPageGranularity;

  // OW's section_offset at b_lo: shift the leftmost desc's section_offset
  // back by (leftmost->lo - b_lo). Leftmost's lo equals b_lo when the
  // OW extent's leading edge isn't covered by a separate desc.
  LARGE_INTEGER ow_offset_at_b_lo = leftmost_desc->section_offset;
  ow_offset_at_b_lo.QuadPart -=
      static_cast<int64_t>(leftmost->lo - b_lo);

  const DWORD ow_prot = leftmost_desc->view_prot;

  // Phase 1: unmap each prov entry's kernel state to a placeholder. An
  // entry whose `placeholder_base` is null was allocated but never had
  // kernel state populated (e.g. a `commit_inside_range` that errored at
  // the map syscall before `backing_set_kernel_state`) and is skipped.
  // `STATUS_NOT_MAPPED_VIEW` is tolerated — a sibling-remap failure path
  // may have left an unmapped placeholder behind.
  for (uint32_t i = 0; i < prov.count; ++i) {
    DescBacking *b = prov.items[i].backing;
    if (b == nullptr)
      continue;
    void *base = b->placeholder_base.load(cpp::MemoryOrder::ACQUIRE);
    if (base == nullptr)
      continue;
    if (b->shape == BackingShape::SectionView) {
      NTSTATUS st = nt_pal::unmap_view_preserve_transient(base);
      if (!NT_SUCCESS(st) && st != STATUS_NOT_MAPPED_VIEW)
        return false;
    } else {
      // PrivateCommit inside a SectionView OW envelope happens when a
      // replace targets a section view with an anon-private NEW. The
      // NEW's commit becomes a placeholder via `decommit_preserve`.
      uint32_t pages = b->placeholder_pages;
      if (!nt_pal::decommit_preserve(
              base, static_cast<size_t>(pages) * kPageGranularity))
        return false;
    }
  }

  // Phase 2: coalesce iff the OW extent is wider than the intent (i.e.
  // there are ≥2 placeholder fragments after Phase 1). When OW exactly
  // equals intent, only the inside M placeholder exists and coalesce
  // would reject the single-VAD range with STATUS_CONFLICTING_ADDRESSES.
  const bool has_edges = (b_lo < intent.lo()) || (intent.hi() < b_hi);
  if (has_edges) {
    NTSTATUS st = nt_pal::coalesce_placeholders(
        reinterpret_cast<void *>(b_lo),
        static_cast<size_t>(b_hi - b_lo));
    if (!NT_SUCCESS(st))
      return false;
  }

  // Phase 3: remap OW.section at the wider placeholder. Exact-match size
  // — coalesce produced a single placeholder of exactly `b_hi - b_lo`.
  NTSTATUS st = nt_pal::map_section_replace(
      ow_section, reinterpret_cast<void *>(b_lo),
      static_cast<size_t>(b_hi - b_lo), ow_offset_at_b_lo, ow_prot);
  if (!NT_SUCCESS(st))
    return false;

  return true;
}

void rollback_provisional(ProvisionalList &prov,
                           bool kernel_already_restored) {
  for (uint32_t i = 0; i < prov.count; ++i) {
    DescBacking *b = prov.items[i].backing;
    if (b == nullptr)
      continue;
    // RELAXED: this thread is the unique owner — the backing was never
    // published to a chain.
    b->state.store(kBackingStateKilled, cpp::MemoryOrder::RELAXED);
    // `kernel_already_restored`: `try_restore_old_section_view`
    // wrapped every prov entry's kernel state up into OW's restored
    // view — running `backing_kill_and_retire` on an Owner here would
    // unmap a fragment of that restored view (NT unmaps the entire
    // view containing the address). Force metadata-only retire.
    //
    // Otherwise: Sibling kernel state is borrowed from a still-LIVE OW
    // (metadata-only retire); Owner kernel state was newly created by
    // this envelope and is ours to tear down (full teardown).
    const bool sibling =
        prov.items[i].kind == ProvisionalList::Kind::Sibling;
    if (kernel_already_restored || sibling)
      backing_retire_metadata_only(b);
    else
      backing_kill_and_retire(b);
  }
  prov.clear();
}

} // namespace internal

// Order is the contract:
//   1. Walk the claimed extent. For each non-MEM_FREE region whose
//      AllocationBase lies inside [b_lo, b_hi):
//        a. SectionView shape + MEM_MAPPED: unmap_view_preserve before
//           free, else NtFreeVirtualMemory returns STATUS_UNABLE_TO_-
//           DELETE_SECTION. STATUS_NOT_MAPPED_VIEW tolerated.
//        b. free_placeholder(AllocationBase) — releases the whole VAD.
//   2. Close section, then file. Either may be null — sibling re-map
//      backings carry no handle ownership.
//   3. Retire metadata to Crystalline-W.
//
// Walks rather than calling free_placeholder once at saved_base
// because a `replace` envelope's failed rollback can leave the wider
// claim as L_committed + M_FREE + R_committed (PrivateCommit OW that
// hit `bookmarks.restart` — coalesce can't mix committed-with-
// placeholder, and demote-content of L/R would be a regression). A
// single-call kill at saved_base would free only the b_lo fragment,
// orphaning R. Single-VAD case (the common one) is one MBI entry; the
// walk's overhead is one extra MBI query (~218 ns).
// `last_freed_alloc_base` collapses the per-AllocationBase walk when a
// single VAD splits across multiple MBI rows (committed + decommitted
// sub-regions). The AllocationBase range check rejects foreign
// allocations whose base lies inside our extent.
//
// Caller must have CAS'd `state` to Killed first — that store is the
// linearisation point declaring teardown ownership. Siblings on the
// rollback path use `backing_retire_metadata_only` instead.
void backing_kill_and_retire(DescBacking *backing) {
  if (LIBC_UNLIKELY(backing == nullptr))
    __builtin_trap();
  LIBC_ASSERT(backing->state.load(cpp::MemoryOrder::ACQUIRE) ==
              kBackingStateKilled);

  // `shape` is plain (set once at `backing_set_kernel_state`).
  // ACQ_REL on the field exchanges: ACQUIRE so concurrent reader pins
  // see our prior writes when they observe null; RELEASE publishes
  // null to any post-grace observer.
  const BackingShape shape = backing->shape;
  const uint32_t placeholder_pages = backing->placeholder_pages;
  HANDLE saved_section = backing->section_handle.exchange(
      nullptr, cpp::MemoryOrder::ACQ_REL);
  HANDLE saved_file = backing->file_handle.exchange(
      nullptr, cpp::MemoryOrder::ACQ_REL);
  void *saved_base = backing->placeholder_base.exchange(
      nullptr, cpp::MemoryOrder::ACQ_REL);

  if (saved_base != nullptr) {
    const uintptr_t b_lo = reinterpret_cast<uintptr_t>(saved_base);
    const size_t span =
        static_cast<size_t>(placeholder_pages) * kPageGranularity;
    const uintptr_t b_hi = b_lo + span;
    nt_pal::RegionWalker walk(saved_base, static_cast<SIZE_T>(span));
    if (LIBC_LIKELY(static_cast<bool>(walk))) {
      uintptr_t last_freed_alloc_base = 0;
      while (walk.next()) {
        if (walk.entry->State == MEM_FREE)
          continue;
        const uintptr_t alloc_base_v =
            reinterpret_cast<uintptr_t>(walk.entry->AllocationBase);
        if (alloc_base_v < b_lo || alloc_base_v >= b_hi)
          continue;
        if (alloc_base_v == last_freed_alloc_base)
          continue;
        void *alloc_base = walk.entry->AllocationBase;
        if (shape == BackingShape::SectionView &&
            walk.entry->Type == MEM_MAPPED)
          (void)nt_pal::unmap_view_preserve_transient(alloc_base);
        (void)nt_pal::free_placeholder(alloc_base);
        last_freed_alloc_base = alloc_base_v;
      }
    } else {
      // RegionWalker auto-scratch failed (no thread_scratch). Legacy
      // single-VAD teardown; fragmented extents leak from this branch.
      if (shape == BackingShape::SectionView)
        (void)nt_pal::unmap_view_preserve_transient(saved_base);
      (void)nt_pal::free_placeholder(saved_base);
    }
  }
  if (saved_section != nullptr)
    (void)::NtClose(saved_section);
  if (saved_file != nullptr)
    (void)::NtClose(saved_file);

  g_va_tracker_backing_domain.retire(backing);
}

// `placeholder_base` is RELEASE-nulled defensively to match the
// `backing_kill_and_retire` discipline — today no reader can resolve a
// stale BackingRef to this slot (the only descs that held the
// sibling's ref were in `new_nodes` and were retired by
// `retire_unpublished_nodes` immediately before this call, and
// `new_nodes` descs never reach the published chain), so the null
// store is for future readers if that invariant ever weakens.
void backing_retire_metadata_only(DescBacking *backing) {
  if (LIBC_UNLIKELY(backing == nullptr))
    __builtin_trap();
  // RELAXED: rollback owner is the unique writer; the backing was
  // never published past `prov` and cannot race a peer kill.
  backing->state.store(kBackingStateKilled, cpp::MemoryOrder::RELAXED);
  backing->placeholder_base.store(nullptr, cpp::MemoryOrder::RELEASE);
  g_va_tracker_backing_domain.retire(backing);
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
