//===- va_tracker.h - POSIX-visible VA tracker public API -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Layer 1 of the NT-POSIX memory architecture: the public façade that
/// tracks every POSIX-visible mapping. Composes an outer ROWEX-synchronised
/// ART (Leis, Kemper, Neumann - ICDE 2013; Leis et al. - DaMoN 2016 for the
/// ROWEX synchronisation model) partitioning the 47-bit user VA into
/// 4 GiB-aligned leaves, with an inner per-arena concurrent interval
/// skiplist (Kim, Kwon, Kang - SOSP 2025) holding the per-region descs.
///
/// Mutations execute as a single atomic envelope per arena:
/// Lock + nt_pal phase + Swap + Stage-2-decide + Unlock. Multi-arena ranges
/// are auto-decomposed into per-arena envelopes; the multi-arena composite
/// is observable as N independent atomic ops, not a transactional N-tuple.
///
/// Reads (`resolve`, `walk_range`) run wait-free under per-pointer
/// Crystalline-W pins (Nikolaev, Ravindran - PLDI 2024), so the SIGSEGV
/// classifier is bounded even under writer contention. Only POSIX-visible
/// mappings are tracked here; the allocator owns its own VA via `nt_pal::`
/// directly with a private membership bitmap.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_TRACKER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_TRACKER_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/art_index.h"
#include "src/__support/OSUtil/windows/memory/interval_skiplist.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

//===----------------------------------------------------------------------===//
// Range, kind, ref, and meta types.
//===----------------------------------------------------------------------===//

/// Half-open VA range, inclusive low / exclusive high.
///
/// `start` must be 64 KiB aligned (NT allocation granularity) and `bytes`
/// must be a positive multiple of 64 KiB; the API rejects other inputs
/// with `Error(EINVAL)`. Empty ranges are rejected.
struct VaRange {
  void *start{nullptr};
  size_t bytes{0};

  [[nodiscard]] LIBC_INLINE uintptr_t lo() const {
    return reinterpret_cast<uintptr_t>(start);
  }
  [[nodiscard]] LIBC_INLINE uintptr_t hi() const { return lo() + bytes; }
  [[nodiscard]] LIBC_INLINE bool empty() const { return bytes == 0; }
};

/// POSIX-flavoured mapping kind.
///
/// Numeric values are part of the public ABI; do not renumber. Cordon
/// kinds (Image / Kernel / Foreign) are pagemap-only and live in
/// `pagemap_cordon.h`'s `CordonKind`, not in this enum. Libc-internal VA
/// bypasses the tracker entirely and stamps the pagemap directly via
/// `internal_va_facade`.
enum class RegionKind : uint8_t {
  AnonPrivate = 0,
  AnonShared = 1,
  FilePrivate = 2,
  FileShared = 3,
  ShmPosix = 4,
  ShmSysV = 5,
  Brk = 6,
  StackGuard = 7,
};

/// Opaque reference returned by `acquire` and `resolve`.
///
/// Valid only for the duration of the caller's Crystalline-W pin on the
/// skiplist domain; escaping past pin release is undefined. The next
/// `va_tracker` call on this thread rotates the pin slot, invalidating
/// any previously-returned `RegionRef`.
struct RegionRef {
  RegionDesc *desc{nullptr};

  [[nodiscard]] LIBC_INLINE bool valid() const { return desc != nullptr; }
};

//===----------------------------------------------------------------------===//
// AcquireMeta — input to `acquire` and `replace`.
//===----------------------------------------------------------------------===//

/// Input record for `acquire` and `replace`.
///
/// Per-desc state (view_prot, flags, section_offset) is copied into the
/// freshly built `RegionDesc`. Backing-resident state (handles +
/// placeholder identity) is copied into a freshly allocated `DescBacking`
/// shared by every desc that points at the same source mapping via a
/// `BackingRef`.
///
/// `placeholder_*` are zero-defaulted: when both are zero the engine
/// infers `(range.start, range.bytes)` for the kernel placeholder. Brk's
/// 256 MiB single-shot reserve and aligned reservations whose kernel-
/// rounded size exceeds the registered range set them explicitly.
struct AcquireMeta {
  // Per-desc state. Transferred to the freshly built `RegionDesc`.
  DWORD view_prot{0};
  uint16_t flags{0};
  uint64_t section_offset{0};

  // Backing-resident state. Copied into the freshly allocated
  // `DescBacking` at envelope time. Multiple descs that share the backing
  // carry the same encoded `BackingRef`.
  HANDLE section_handle{nullptr};
  HANDLE file_handle{nullptr};

  /// Optional kernel-placeholder identity covering
  /// `[placeholder_base, placeholder_base + placeholder_size)`. When
  /// non-zero, the reservation may exceed the registered range. Used by:
  ///   * brk (one large placeholder, narrow registered range).
  ///   * `posix_memalign` / aligned reservations where NT alignment
  ///     headroom widens the placeholder past the requested size.
  ///   * `mremap` grow-into-headroom.
  /// Default zero values mean "placeholder identity == registered range";
  /// the engine populates the backing to match `range`.
  void *placeholder_base{nullptr};
  size_t placeholder_size{0};
};

/// Mutator callback invoked by `mutate` on a fresh clone of each desc
/// intersecting the range.
///
/// Runs inside the engine's nt_pal phase under the LOCKED hold. Its
/// effect lands as a Swap-published new desc — the engine never mutates
/// any desc visible to readers. Plain function-pointer ABI (no lambdas;
/// libc convention).
using DescMutator = void (*)(RegionDesc *new_desc, void *ctx);

/// Visitor callback invoked by `walk_range` for each desc inside the
/// requested range, in ascending VA order.
using WalkVisitor = void (*)(VaRange covered, RegionDesc *desc, void *ctx);

//===----------------------------------------------------------------------===//
// Public typed operations.
//
// Common error codes:
//   EINVAL  Alignment / bounds violation; empty range; straddler in the
//           range (caller must pre-split via `split()`); null mutator.
//   EEXIST  `acquire` saw STATUS_CONFLICTING_ADDRESSES (range not
//           MEM_FREE).
//   ENOENT  `mutate` / `split` over a range with no covering desc.
//           `release` of an unmapped range degrades to a no-op per POSIX
//           munmap semantics.
//   ENOMEM  Partition / chunk / Crystalline pool exhaustion; kernel
//           STATUS_NO_MEMORY.
//   E2BIG   Provisional or candidate inline cap exceeded (> 16 backings
//           touched in one envelope; pathological).
//   EAGAIN  Bounded-retry exhaustion under contention.
//   EFAULT  Unexpected NTSTATUS from a kernel op the engine cannot
//           classify.
//===----------------------------------------------------------------------===//

/// Acquire fresh VA, install in the tracker, allocate the shared
/// `DescBacking`, and stamp the pagemap.
///
/// Atomicity is **single-arena**: when `range` fits inside one 4 GiB
/// ART leaf the engine runs as one atomic envelope, and any failure
/// rolls back all partial state. The caller observes Error or
/// RegionRef, never both.
///
/// For a `range` that spans multiple ART leaves the engine decomposes
/// into N per-arena envelopes (see `dispatch_per_arena_op` in
/// `va_tracker_transaction.cpp`); each sub-envelope is atomic on its
/// own arena, but the multi-arena composite is **best-effort** — if
/// envelope `k` of N fails, envelopes `0..k-1` remain committed and
/// visible, and the caller observes the first failed envelope's
/// errno. Cross-arena cleanup is the caller's responsibility.
///
/// \pre `range` is `MEM_FREE`. The engine does not unmap an existing
///      mapping; use `replace` for that.
/// \returns A `RegionRef` pinned on the skiplist domain's `kPinSlotCur`,
///          valid until the next `va_tracker` call on this thread.
[[nodiscard]] ::LIBC_NAMESPACE::ErrorOr<RegionRef>
acquire(VaRange range, RegionKind kind, const AcquireMeta &meta);

/// Acquire fresh VA at a kernel-chosen MEM_FREE base.
///
/// The engine asks `nt_pal::reserve_placeholder(nullptr, bytes)` for a
/// MEM_FREE address before running the per-arena envelope; the
/// envelope skips its own pre-commit reserve step because the
/// placeholder already exists at the chosen base. The chosen base is
/// returned directly so the caller does not need a follow-up
/// `resolve()` to recover it.
///
/// Use this when the caller has no hint — typically `mmap(NULL, len,
/// ...)`. A hint-respecting acquire whose hint is known MEM_FREE
/// should use `acquire(VaRange{hint, len}, ...)` to honour the hint
/// instead of letting the kernel pick.
///
/// On any failure of the envelope the placeholder is released before
/// the errno is returned, so the caller observes a fully-rolled-back
/// state. The brief window between reservation and the envelope's
/// LOCKED hold is invisible to other POSIX-layer consumers (the VA
/// is MEM_RESERVE during it, not MEM_FREE), eliminating the
/// release / re-reserve race a POSIX-side scout loop would otherwise
/// have to retry through.
///
/// \pre `bytes > 0` and `bytes % 64 KiB == 0`.
/// \returns The chosen base on success. `Error(EINVAL)` for an
///          invalid size, `Error(ENOMEM)` for VA exhaustion,
///          otherwise the envelope's errno.
[[nodiscard]] ::LIBC_NAMESPACE::ErrorOr<void *>
acquire_kernel_chosen(size_t bytes, RegionKind kind,
                      const AcquireMeta &meta);

/// `MAP_32BIT` variant of `acquire_kernel_chosen`: the scout uses
/// `nt_pal::reserve_placeholder_32bit` so the chosen base lands in
/// the low 2 GiB. Same race-free envelope contract — the placeholder
/// is never visible as MEM_FREE between the scout and the commit, so
/// a concurrent MAP_32BIT consumer cannot win the same VA.
///
/// \pre `bytes > 0` and `bytes % 64 KiB == 0`.
[[nodiscard]] ::LIBC_NAMESPACE::ErrorOr<void *>
acquire_kernel_chosen_32bit(size_t bytes, RegionKind kind,
                            const AcquireMeta &meta);

/// Drop every desc whose extent is fully inside `range`.
///
/// The post-Swap survivor walk plus per-backing state CAS owns
/// synchronous Stage 2 kernel teardown for any backing whose extent has
/// no LIVE referencer. The VA is `MEM_FREE` on return for the fully-
/// released portion.
///
/// \pre No desc in `range` straddles a `range` boundary. Caller pre-
///      splits via `split()` for the straddle case; pre-split incurs no
///      kernel work, so pre-split + release is observably correct.
[[nodiscard]] int release(VaRange range);

/// Atomic `MAP_FIXED` over `range`.
///
/// For each fully-inside desc, the engine demotes OLD kernel state back
/// to a placeholder (`unmap_view_preserve` for section-backed,
/// `decommit_preserve` for anon-placeholder), `coalesce_placeholders`
/// joins fragments, then `commit_replace` / `map_section_replace`
/// installs the new mapping into the now-coalesced placeholder. Old-
/// backing placeholder ownership is transferred to the new backing so
/// Stage 2 cannot double-free. The VA never crosses `MEM_FREE` — no
/// freeze bracket needed.
///
/// Partial-replace of a wider shared backing (B2-β): when an OLD desc in
/// `range` references a backing whose extent extends past either edge to
/// a desc OUTSIDE `range`, the engine extends its internal lock to the
/// backing's full extent, splits the placeholder at the range edges
/// (section views) or leaves the surviving slice committed (private
/// commits), re-maps the surviving sibling slice over the OLD section
/// handle (section views) or carries it as pure metadata (private
/// commits), and rebinds outside-range survivor descs to fresh sibling
/// backings. Per-fragment kernel work stays O(1) regardless of survivor
/// count.
///
/// \pre No desc in `range` straddles a `range` boundary. Outside-range
///      descs sharing a wider backing are NOT straddlers in this sense.
/// \returns 0 on success; `-EINVAL` if a straddler sits on a `range`
///          edge; `-ENOTSUP` for heterogeneous view_prot / flags / kind
///          across descs sharing one wider backing, or cross-chain
///          abutment at the extended lock edges.
[[nodiscard]] int replace(VaRange range, RegionKind kind,
                          const AcquireMeta &meta);

/// Apply `mutator` to a fresh clone of every desc intersecting `range`,
/// then publish the clones via Swap.
///
/// Clones share the source's `BackingRef` verbatim — pure metadata
/// mutation. The post-Swap survivor walk sees the clones and skips
/// Stage 2. When `prot_change` is non-zero, the engine also issues
/// `nt_pal::protect(range, prot_change)` inside the locked envelope so
/// kernel-side protection matches the new desc state. Pure metadata
/// mutators (NUMA rebind) pass 0.
///
/// When `commit_if_uncommitted_accessible` is true, the post-Swap
/// phase replaces the per-VAD `nt_pal::protect` with a per-chunk
/// dispatch over each locked succ's intersection with `range`
/// (`nt_pal::RegionWalker` + three-way state machine: committed →
/// protect, uncommitted + accessible + MEM_MAPPED →
/// `commit_in_reservation`, uncommitted + accessible + MEM_PRIVATE →
/// `commit_replace[_numa]` using `numa_node`, uncommitted + PROT_NONE
/// → no-op). `MEM_FREE` mid-range aborts with `ENOMEM`. COW
/// translation is applied per-succ from the OLD desc's
/// `region_flag::COW`. The substrate also writes view_prot
/// (full-cover) or sets `region_flag::PROT_DIVERGED` (partial-cover)
/// on each clone — `mutator` MAY be null on this path because the
/// caller-side field updates collapse into the substrate's coverage
/// logic. CFG-secured retries are absorbed by `nt_pal::protect`.
///
/// \pre No desc in `range` straddles a `range` boundary — POSIX
///      `mprotect(addr, len, prot)` requires the mutation to touch only
///      `[addr, addr + len)`, so mutating a straddler would alter
///      regions outside the caller's range. Caller pre-splits via
///      `split()` for the straddle case.
[[nodiscard]] int mutate(VaRange range, DescMutator mutator, void *ctx,
                         DWORD prot_change = 0,
                         bool commit_if_uncommitted_accessible = false,
                         int numa_node = -1);

/// Split the single desc covering `boundary` into two clones sharing the
/// source's `BackingRef`.
///
/// No kernel work. Used as a pre-step by callers that need to operate on
/// a sub-range of an existing mapping (e.g. `MAP_FIXED` with a
/// straddler).
///
/// \returns 0 on success; `-ENOENT` if no desc covers `boundary`;
///          `-EINVAL` if `boundary` is not a 64 KiB-aligned strict
///          interior of the covering desc.
[[nodiscard]] int split(void *boundary);

//===----------------------------------------------------------------------===//
// Reads — wait-free, AS-safe under per-pointer Crystalline-W pin.
//===----------------------------------------------------------------------===//

/// Wait-free SIGSEGV-callable resolver.
///
/// Returns the covering `RegionRef` pinned on the skiplist domain's
/// `kPinSlotCur`. The pin persists until the next `va_tracker` call on
/// this thread rotates the slot; the caller dereferences the desc and
/// returns without an explicit drop step.
///
/// \returns Pinned `RegionRef` on success; `Error(ENOENT)` if no region
///          covers `addr`.
[[nodiscard]] ::LIBC_NAMESPACE::ErrorOr<RegionRef> resolve(void *addr);

/// Ordered iteration over every desc intersecting `range`, under
/// skiplist + ART pins.
///
/// The visitor MUST NOT call back into any mutating `va_tracker` entry
/// — re-entrant Map operations from inside a pinned walk are UB.
void walk_range(VaRange range, WalkVisitor visitor, void *ctx);

//===----------------------------------------------------------------------===//
// Fork serialization and replay.
//===----------------------------------------------------------------------===//

/// Sink fed by `serialize_for_fork`. The emit callback receives every
/// POSIX-visible mapping in ascending VA order.
struct ForkSink {
  using EmitFn = int (*)(void *ctx, VaRange r, RegionKind k,
                         const AcquireMeta &m);
  EmitFn emit{nullptr};
  void *ctx{nullptr};
};

/// Replayable snapshot consumed by `replay_in_child`. The child re-
/// `acquire`s each entry in order; ordering is irrelevant because the
/// engine treats each entry as an independent atomic envelope.
struct ForkSnapshot {
  struct Entry {
    VaRange range;
    RegionKind kind;
    AcquireMeta meta;
  };
  const Entry *entries{nullptr};
  size_t count{0};
};

/// Walk every populated leaf in the ART tree, emitting every POSIX-
/// visible mapping to `sink`. Cordon / image / kernel / libc-internal
/// shapes are filtered out — they are re-established by the child's own
/// loader / bootstrap.
[[nodiscard]] int serialize_for_fork(ForkSink &sink);

/// Re-`acquire` every entry in `snap` in the child. Stops at the first
/// failure and returns its errno value.
[[nodiscard]] int replay_in_child(const ForkSnapshot &snap);

/// `.libcfork$M` hook at priority `kForkPrioVaTracker = 39`.
///
/// Runs after `kForkPrioPartition = 38` so partition state is already
/// rebuilt, and before `kForkPrioMemoryReconcile = 50` so reconciliation
/// sees the fresh tracker structure.
void va_tracker_fork_reinit();

/// Pre-fork barrier: drop every per-thread Crystalline-W reservation on
/// every va_tracker domain so the parent's snapshot does not capture
/// this thread's pin as a held era.
void pre_fork_drain();

//===----------------------------------------------------------------------===//
// Bootstrap and internal exposed helpers.
//===----------------------------------------------------------------------===//

/// Returns true once `va_tracker_init_fn` has published the tracker as
/// ready. Used by the memory-primitives bootstrap latch.
[[nodiscard]] bool is_va_tracker_ready();

/// Resolve or install the arena that owns the ART leaf covering `va`.
///
/// Exposed for the engine's cross-arena dispatcher; not part of the
/// POSIX consumer surface. New arenas are pinned to the current CPU at
/// install time and are canonical for every subsequent op on that VA
/// prefix.
[[nodiscard]] Arena *resolve_or_install_arena(uintptr_t va);

/// Memory-primitives Phase 6 init. Registers the ART and skiplist
/// Crystalline domains, primes the load-key callback, brings the backing
/// pool online, and warms every per-thread slot on the current thread.
uint32_t va_tracker_init_fn(::LIBC_NAMESPACE::internal::Receipt *out,
                            uint32_t cap);

//===----------------------------------------------------------------------===//
// Diagnostics.
//===----------------------------------------------------------------------===//

/// Per-class counters for the ART and skiplist substrate. Sampled
/// without synchronisation — values are best-effort during concurrent
/// mutation.
struct VaTrackerStats {
  uint32_t art_live_chunks_per_type[4];
  uint32_t art_live_nodes_per_type[4];
  uint32_t skiplist_live_chunks_per_bucket[kBucketCount];
  uint32_t skiplist_live_nodes_per_bucket[kBucketCount];
  uint32_t arena_count;
};

[[nodiscard]] VaTrackerStats va_tracker_stats_snapshot();

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_TRACKER_H
