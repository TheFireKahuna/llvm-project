//===- va_tracker_transaction.cpp - envelope driver + public surface ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Frame driver and public typed-op surface. Per-op kernel programs,
// post-Swap fixups, and the OLD-backing reaper live in
// `va_tracker_execute.cpp`.
//
// Envelope: Lock → (preflight widen for replace/release) → execute →
// Swap (linearisation) → post-Swap → ownership transfer → reap →
// Unlock. Bounded retry on Swap-CAS loss surfaces `-EAGAIN`. Disjoint
// VA ops run in parallel — no tree-wide lock (Kim, Kwon, Kang, SOSP
// 2025).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_transaction_internal.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/memory/desc_backing.h"
#include "src/__support/OSUtil/windows/memory/interval_skiplist.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
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
using internal::kAllocGranularity;
using internal::kCommitRetryCap;
using internal::kOpKindCount;
using internal::kPageGranularity;
using internal::OpKind;
using internal::ProvisionalList;

namespace internal {

// Acquire-family base must be alloc-granularity aligned (NT
// `MEM_RESERVE_PLACEHOLDER` constraint); interior ops accept page
// granularity. `bytes` is page-aligned in both — the envelope rounds
// the reservation up and shrinks the placeholder before commit.
bool range_valid_acquire(VaRange r) {
  if (r.bytes == 0)
    return false;
  uintptr_t lo = r.lo();
  if ((lo & (kAllocGranularity - 1)) != 0)
    return false;
  if ((r.bytes & (kPageGranularity - 1)) != 0)
    return false;
  return lo + r.bytes >= lo;
}

bool range_valid_interior(VaRange r) {
  if (r.bytes == 0)
    return false;
  uintptr_t lo = r.lo();
  if ((lo & (kPageGranularity - 1)) != 0)
    return false;
  if ((r.bytes & (kPageGranularity - 1)) != 0)
    return false;
  return lo + r.bytes >= lo;
}

} // namespace internal

namespace {

// 4 GiB-aligned 4 GiB ART leaves.
[[nodiscard]] LIBC_INLINE bool fits_one_arena(VaRange r) {
  return (r.lo() >> 32) == ((r.hi() - 1) >> 32);
}

[[nodiscard]] LIBC_INLINE uintptr_t arena_hi_for(uintptr_t lo) {
  return (lo & ~uintptr_t{0xFFFFFFFFu}) + (uintptr_t{1} << 32);
}

// One row per `OpKind`, indexed by `static_cast<uint32_t>(op)`.
// Constexpr-emitted into `.rdata`; every dispatch site reduces to two
// constant pointer loads, eliminating the three open-coded `OpKind`
// switches the prior driver used.
struct TypedOpDescriptor {
  internal::ValidateRangeFn validate_range;
  internal::ExecuteFn execute;
  internal::PostSwapFn post_swap;     // null = no post-Swap work
  bool needs_preflight;               // widen lock for edge-extending bks
  bool needs_ownership_transfer;      // null OLD placeholder_base
};

inline constexpr TypedOpDescriptor kOpTable[internal::kOpKindCount] = {
    /* Acquire */
    {&internal::range_valid_acquire,  &internal::execute_acquire,
     nullptr, false, false},
    /* Release */
    {&internal::range_valid_interior, &internal::execute_release,
     nullptr, true,  true},
    /* Replace */
    {&internal::range_valid_interior, &internal::execute_replace,
     nullptr, true,  true},
    /* Mutate */
    {&internal::range_valid_interior, &internal::execute_mutate,
     &internal::post_swap_mutate, false, false},
    /* Split */
    {&internal::range_valid_interior, &internal::execute_split,
     nullptr, false, false},
    /* AcquireAtReserved */
    {&internal::range_valid_interior, &internal::execute_acquire_at_reserved,
     nullptr, false, false},
};

static_assert(static_cast<uint32_t>(OpKind::Acquire) == 0);
static_assert(static_cast<uint32_t>(OpKind::Release) == 1);
static_assert(static_cast<uint32_t>(OpKind::Replace) == 2);
static_assert(static_cast<uint32_t>(OpKind::Mutate) == 3);
static_assert(static_cast<uint32_t>(OpKind::Split) == 4);
static_assert(static_cast<uint32_t>(OpKind::AcquireAtReserved) == 5);

// Convergence in one iteration by VA contiguity: after widening to
// (min over leftmost backings' b_lo, max over rightmost backings'
// b_hi), the now-leftmost succ's backing is either the same one we
// saw extending past the original edge, or a wholly-inside backing.
void compute_widened_lock(const LockedSet &locked, uintptr_t &lock_lo,
                          uintptr_t &lock_hi) {
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
    uintptr_t b_hi = b_lo + static_cast<uintptr_t>(b->placeholder_pages) *
                                kPageGranularity;
    if (b_lo < want_lo)
      want_lo = b_lo;
    if (b_hi > want_hi)
      want_hi = b_hi;
  }
  lock_lo = want_lo;
  lock_hi = want_hi;
}

[[nodiscard]] int run_envelope(const CommitIntent &intent) {
  const TypedOpDescriptor &op =
      kOpTable[static_cast<uint32_t>(intent.op)];

  if (LIBC_UNLIKELY(!op.validate_range(intent.range)))
    return -EINVAL;
  if (LIBC_UNLIKELY(!fits_one_arena(intent.range)))
    return -ENOTSUP;

  Arena *arena = resolve_or_install_arena(intent.range.lo());
  if (LIBC_UNLIKELY(arena == nullptr))
    return -ENOMEM;

  // One anchor covers every retry — slot tag is statically disjoint
  // from the reader-side pin per `BackingPinSlot`'s static_assert.
  anchor_backing_engine_pin();

  // Widening is monotonic — a Swap-CAS retry re-acquires on the
  // latest range without dropping a previously-discovered extension.
  uintptr_t lock_lo = intent.range.lo();
  uintptr_t lock_hi = intent.range.hi();

  for (uint32_t attempt = 0; attempt < kCommitRetryCap; ++attempt) {
    ProvisionalList prov;
    NewNodes new_nodes;

    LockedSet locked;
    (void)locked.acquire(arena, lock_lo, lock_hi);
    if (!locked.valid()) {
      int err = locked.errno_;
      return err == 0 ? -EINVAL : err;
    }

    if (op.needs_preflight) {
      uintptr_t want_lo = lock_lo;
      uintptr_t want_hi = lock_hi;
      compute_widened_lock(locked, want_lo, want_hi);
      if (want_lo < lock_lo || want_hi > lock_hi) {
        Unlock(locked, /*include_pred=*/true);
        lock_lo = want_lo;
        lock_hi = want_hi;
        continue;
      }
    }

    int err = op.execute(intent, arena, locked, new_nodes, prov);
    if (err != 0) {
      // For ops that run `demote_by_shape` (replace / release —
      // `needs_preflight`), the L+M+R fragmentation must be undone
      // before retire: OW's eventual `kill_and_retire` only reaches
      // the b_lo fragment via interior-pointer unmap, so any unrestored
      // M / R fragment orphans permanently.
      retire_unpublished_nodes(new_nodes);
      const bool restored =
          op.needs_preflight &&
          internal::try_restore_old_section_view(locked, intent.range,
                                                  prov);
      internal::rollback_provisional(prov, restored);
      Unlock(locked, /*include_pred=*/true);
      return err;
    }

    if (!Swap(locked, new_nodes)) {
      // Swap returns false only on `bookmarks.restart` (upper-level
      // peer mid-splice observed during Step 0) or the defensive
      // `set.pred == nullptr` check; the Step-3 linearisation CAS
      // traps on failure rather than returning. Same structural
      // restore as the execute-failure branch — the next iteration's
      // `demote_by_shape` then operates on a restored wider view
      // instead of fragmented placeholders.
      retire_unpublished_nodes(new_nodes);
      const bool restored =
          op.needs_preflight &&
          internal::try_restore_old_section_view(locked, intent.range,
                                                  prov);
      internal::rollback_provisional(prov, restored);
      Unlock(locked, /*include_pred=*/true);
      continue;
    }

    if (op.post_swap != nullptr) {
      // Chain is published. Caller sees protect-side-effect failure
      // even though desc state landed — POSIX `mprotect` partial
      // success is implementation-defined.
      int prot_err = op.post_swap(intent, locked);
      if (prot_err != 0) {
        Unlock(locked, /*include_pred=*/false);
        return prot_err;
      }
    }

    if (op.needs_ownership_transfer)
      internal::post_swap_ownership_transfer(locked, new_nodes);

    // Reaper always runs; for acquire it's a no-op walk over an empty
    // `locked`.
    internal::reap_old_backings(arena, intent.range, locked, new_nodes);

    Unlock(locked, /*include_pred=*/false);
    prov.clear();
    return 0;
  }
  return -EAGAIN;
}

// Multi-arena ranges decompose into per-arena envelopes. The composite
// is N independent atomic ops — partial failure leaves earlier slices
// committed; the public op rolls them back via `release`.
[[nodiscard]] int dispatch_per_arena_op(CommitIntent intent) {
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

constexpr uintptr_t kArenaStep = uintptr_t{1} << 32;

// Walks at arena-step granularity because every interior arena boundary
// became its own VAD via `split_reserved_at_arena_boundaries`. Probes
// MRI_Ex first: a committed VAD whose desc is still registered (rare,
// only if `vt::release` itself failed) must NOT be MEM_RELEASE'd here —
// that would release the commit too and leave the desc dangling.
LIBC_INLINE void free_placeholders_in_range(void *base, size_t bytes) {
  uintptr_t end = reinterpret_cast<uintptr_t>(base) + bytes;
  uintptr_t cur = reinterpret_cast<uintptr_t>(base);
  while (cur < end) {
    MEMORY_REGION_INFORMATION mri{};
    if (nt_pal::query_region_mri(reinterpret_cast<void *>(cur), mri) &&
        mri.PlaceholderReservation != 0)
      (void)nt_pal::free_placeholder(reinterpret_cast<void *>(cur));
    cur = (cur & ~(kArenaStep - 1)) + kArenaStep;
  }
}

// `MEM_REPLACE_PLACEHOLDER` requires the commit size to match the
// underlying VAD exactly. The kernel-chosen scout creates a single VAD
// spanning every arena; without these splits the 2nd+ per-arena
// `commit_replace` would fail with `STATUS_INVALID_PARAMETER`.
// `OpKind::Acquire` doesn't need this — each per-arena sub-envelope
// reserves its own slice.
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

  const bool multi_arena = !fits_one_arena(VaRange{base, bytes});
  if (multi_arena) {
    int srx = split_reserved_at_arena_boundaries(base, bytes);
    if (LIBC_UNLIKELY(srx != 0))
      return ::LIBC_NAMESPACE::Error{-srx};
  }

  CommitIntent intent;
  intent.op = OpKind::AcquireAtReserved;
  intent.range = VaRange{base, bytes};
  intent.kind = kind;
  intent.meta = meta;

  int rc = dispatch_per_arena_op(intent);
  if (rc != 0) {
    // AcquireAtReserved never frees the placeholder itself — that's
    // our contract. Multi-arena: release committed slices, then free
    // any remaining placeholder VADs.
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

// Page-aligned-but-not-alloc-aligned hint: reserve the enclosing alloc
// granule, shave the prefix to MEM_FREE, hand the remaining placeholder
// (anchored at the exact hint) to AcquireAtReserved. A concurrent
// acquirer inside the granule observes `STATUS_CONFLICTING_ADDRESSES`
// on its own reserve and gets `EEXIST` — same as the alloc-aligned
// path.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::ErrorOr<void *>
acquire_page_aligned_hint(VaRange range, RegionKind kind,
                          const AcquireMeta &meta) {
  const uintptr_t base_addr = range.lo();
  const uintptr_t rounded_down = base_addr & ~(kAllocGranularity - 1);
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
  (void)::NtFreeVirtualMemory(NtCurrentProcess(), &prefix_p, &prefix_size,
                              MEM_RELEASE);
  return finish_acquire_at_reserved(reinterpret_cast<void *>(base_addr),
                                     range.bytes, kind, meta);
}

} // anonymous namespace

::LIBC_NAMESPACE::ErrorOr<RegionRef>
acquire(VaRange range, RegionKind kind, const AcquireMeta &meta) {
  if (LIBC_UNLIKELY(!internal::range_valid_interior(range)))
    return ::LIBC_NAMESPACE::Error{EINVAL};

  if ((range.lo() & (kAllocGranularity - 1)) != 0) {
    auto result = acquire_page_aligned_hint(range, kind, meta);
    if (!result.has_value())
      return ::LIBC_NAMESPACE::Error{result.error()};
    return resolve(range.start);
  }

  CommitIntent intent;
  intent.op = OpKind::Acquire;
  intent.range = range;
  intent.kind = kind;
  intent.meta = meta;
  int rc = dispatch_per_arena_op(intent);
  if (rc != 0) {
    // Multi-arena composite: some slices may have committed. Roll
    // them back via `release` so the caller never observes a partial
    // multi-arena acquire. No `free_placeholders_in_range` needed —
    // each Acquire slice owns its own reservation, which is either
    // committed-and-registered (handled by `release`) or already
    // freed by the envelope's own rollback.
    if (range.bytes != 0)
      (void)release(range);
    return ::LIBC_NAMESPACE::Error{rc < 0 ? -rc : rc};
  }
  return resolve(range.start);
}

// Scout-then-envelope is race-free: the placeholder is never visible
// as MEM_FREE between the scout and the commit, so a concurrent
// acquirer cannot win the same VA. A POSIX-side scout loop using
// release/re-reserve would race here.
::LIBC_NAMESPACE::ErrorOr<void *>
acquire_kernel_chosen(size_t bytes, RegionKind kind,
                      const AcquireMeta &meta) {
  if (LIBC_UNLIKELY(bytes == 0 || (bytes & (kPageGranularity - 1)) != 0))
    return ::LIBC_NAMESPACE::Error{EINVAL};
  return finish_acquire_at_reserved(
      nt_pal::reserve_placeholder(nullptr, bytes), bytes, kind, meta);
}

::LIBC_NAMESPACE::ErrorOr<void *>
acquire_kernel_chosen_32bit(size_t bytes, RegionKind kind,
                            const AcquireMeta &meta) {
  if (LIBC_UNLIKELY(bytes == 0 || (bytes & (kPageGranularity - 1)) != 0))
    return ::LIBC_NAMESPACE::Error{EINVAL};
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
           bool commit_if_uncommitted_accessible, int numa_node,
           MutateChunkFilter chunk_filter) {
  // Plain-mutate requires a mutator (every locked desc must produce a
  // mutator-applied clone). The commit-if-uncommitted path and the
  // filtered-protect path both make a null mutator legal — the
  // substrate-side coverage logic covers it.
  if (LIBC_UNLIKELY(mutator == nullptr && !commit_if_uncommitted_accessible &&
                    chunk_filter == nullptr))
    return EINVAL;
  CommitIntent intent;
  intent.op = OpKind::Mutate;
  intent.range = range;
  intent.mutator = mutator;
  intent.mutator_ctx = ctx;
  intent.prot_change = prot_change;
  intent.commit_if_uncommitted_accessible = commit_if_uncommitted_accessible;
  intent.numa_node = numa_node;
  intent.chunk_filter = chunk_filter;
  int rc = dispatch_per_arena_op(intent);
  return rc < 0 ? -rc : rc;
}

int split(void *boundary) {
  uintptr_t b = reinterpret_cast<uintptr_t>(boundary);
  if ((b & (kPageGranularity - 1)) != 0)
    return EINVAL;

  // The envelope range must lie in one arena. A naive `[b - 4K, b +
  // 4K)` window straddles an arena boundary whenever `b` is itself
  // 4 GiB-aligned. At an arena edge no desc can straddle `b` anyway
  // (the edge is a hard routing boundary), so split there is
  // meaningless.
  uintptr_t arena_lo = b & ~uintptr_t{0xFFFFFFFFu};
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
  int rc = run_envelope(intent);
  return rc < 0 ? -rc : rc;
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
