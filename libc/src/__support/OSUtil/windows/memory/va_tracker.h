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
/// Lock + nt_pal phase + Swap + reap + Unlock. Multi-arena ranges
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
/// Interior ops (`release`, `replace`, `mutate`, `split`, `walk_range`,
/// `resolve`) require `start` and `bytes` to be page aligned (4 KiB on x86_64
/// and AArch64). The acquire family (`acquire`, `acquire_kernel_chosen`,
/// `acquire_kernel_chosen_32bit`) additionally requires alignment to NT
/// allocation granularity (64 KiB) because that is the kernel's `MEM_RESERVE_
/// PLACEHOLDER` base-placement granularity — see each op's precondition.
/// The API rejects misaligned, empty, or wrapping ranges with `Error(EINVAL)`.
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
/// own arena. On per-arena failure, the public surface automatically
/// rolls back every sub-envelope that had committed (via `release` on
/// the registered range) and frees any placeholder VADs the scout
/// established. The caller observes the first failed envelope's errno
/// and a fully-rolled-back state — no cross-arena partial residue,
/// no per-call cleanup work.
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

/// Drop every desc whose extent overlaps `range`.
///
/// Edge-straddler split is performed atomically inside the per-arena
/// envelope: a desc whose extent crosses a `range` boundary is split
/// in-place under the LOCKED hold, with the outside-`range` portion
/// preserved as a fresh survivor desc carrying the OLD metadata.
/// Callers do not pre-split and observe no race window between split
/// and release.
///
/// The post-Swap survivor walk plus per-backing state CAS owns
/// synchronous kernel teardown for any backing whose extent has
/// no LIVE referencer. The VA is `MEM_FREE` on return for the fully-
/// released portion.
[[nodiscard]] int release(VaRange range);

/// Atomic `MAP_FIXED` over `range`.
///
/// For each inside-`range` desc (or inside-portion of a straddler), the
/// engine demotes OLD kernel state back to a placeholder
/// (`unmap_view_preserve` for section-backed, `decommit_preserve`
/// clipped to the inside extent for anon-placeholder),
/// `coalesce_placeholders` joins fragments **only when MEM_FREE gaps
/// were filled by the gap-fill phase** (the optimisation skips a no-op
/// syscall on the contiguous-placeholder common case), then
/// `commit_replace` / `map_section_replace` installs the new mapping
/// into the placeholder. Old-backing placeholder ownership is
/// transferred to the new backing so the reaper cannot double-free. The VA
/// never crosses `MEM_FREE` — no freeze bracket needed.
///
/// Edge-straddler split is performed atomically inside the per-arena
/// envelope: a desc whose extent crosses a `range` boundary is split
/// in-place under the LOCKED hold, with the outside-`range` portion
/// preserved as a fresh survivor desc carrying the OLD metadata.
/// Callers do not pre-split and observe no race window between split
/// and replace.
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
/// \returns 0 on success; `-ENOTSUP` for heterogeneous view_prot /
///          flags / kind across descs sharing one wider backing, or
///          cross-chain abutment at the extended lock edges.
[[nodiscard]] int replace(VaRange range, RegionKind kind,
                          const AcquireMeta &meta);

/// Apply `mutator` to a fresh clone of every desc intersecting `range`,
/// then publish the clones via Swap.
///
/// Edge-straddler split is performed atomically inside the per-arena
/// envelope: a desc whose extent crosses a `range` boundary is split
/// into up to three clones (left-outside non-mutated, inside-mutated,
/// right-outside non-mutated) under the LOCKED hold. The mutator only
/// applies to the inside slice, matching POSIX `mprotect(addr, len,
/// prot)`'s "touch only `[addr, addr + len)`" contract. Per-VAD
/// `nt_pal::protect` in the post-Swap phase is clipped to the same
/// inside slice — outside survivors keep their existing kernel
/// protection. Callers do not pre-split and observe no race window
/// between split and mutate.
///
/// Clones share the source's `BackingRef` verbatim — pure metadata
/// mutation. The post-Swap survivor walk sees the clones and skips
/// reaping. When `prot_change` is non-zero, the engine also issues
/// `nt_pal::protect(inside_slice, prot_change)` inside the locked
/// envelope so kernel-side protection matches the new desc state.
/// Pure metadata mutators (NUMA rebind) pass 0.
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
/// `region_flag::COW`. The kernel-side protection write is the only
/// visible side effect — the substrate does NOT update the clone's
/// `view_prot` (acquire-time intent is preserved) because the
/// kernel holds the authoritative current protection and consumers
/// query MBI when they need it. `mutator` MAY be null on this path
/// because the protection write collapses into the substrate's
/// coverage logic. CFG-secured retries are absorbed by
/// `nt_pal::protect`.
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
///          `-EINVAL` if `boundary` is not a 4 KiB-aligned (NT page-
///          granularity) strict interior of the covering desc. NT supports
///          placeholder splits at any page-aligned offset; see
///          `Placeholders.md` §2 for the empirical contract.
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

/// One protection sub-run within a fork-replay entry's range.
///
/// `desc->view_prot` carries acquire-time intent only; the kernel holds
/// the authoritative per-page protection. The serializer walks MBI runs
/// across each desc's range and emits one `ProtectionRun` per uniform-
/// protection sub-run so the child observes the parent's exact
/// protection layout — including sub-region mprotect divergence —
/// regardless of whether the desc has default or explicit (brk /
/// posix_memalign / mremap-headroom) placeholder identity.
///
/// The first run's `prot` matches `AcquireMeta::view_prot`; the
/// child's `acquire` applies it as part of the initial commit. Runs
/// `[1..protection_count)` are applied post-acquire via
/// `nt_pal::protect`. When `protection_count == 1`, the range is
/// protection-uniform and no extra `NtProtect` calls fire.
///
/// Bytes are page-aligned. Sub-runs cover the entry's `range` without
/// gaps or overlap.
struct ProtectionRun {
  uint32_t offset_from_range_lo;
  uint32_t bytes;
  DWORD    prot;
  uint32_t reserved_; // padding to 16 bytes
};

/// Inline cap per Entry. Sized for typical mprotect patterns (one or
/// two carved sub-regions per desc). On overflow, the serializer
/// emits a single run with `meta.view_prot` and logs nothing — the
/// child observes uniform protection, same as the pre-Option-A
/// degraded path. Real workloads have ≤ 3 runs per desc; 16 is
/// generous headroom.
inline constexpr uint32_t kMaxProtectionRunsPerEntry = 16;

/// Sink fed by `serialize_for_fork`. The emit callback receives every
/// POSIX-visible mapping in ascending VA order.
///
/// `runs` and `run_count` describe the entry's per-sub-run protection
/// map. The callback may copy the runs into snapshot storage (the
/// pointer is transient — valid only for the duration of the call).
/// `run_count` is always ≥ 1.
struct ForkSink {
  using EmitFn = int (*)(void *ctx, VaRange r, RegionKind k,
                         const AcquireMeta &m,
                         const ProtectionRun *runs, uint32_t run_count);
  EmitFn emit{nullptr};
  void *ctx{nullptr};
};

/// Replayable snapshot consumed by `replay_in_child`. The child re-
/// `acquire`s each entry in order; ordering is irrelevant because the
/// engine treats each entry as an independent atomic envelope.
///
/// Each Entry inlines its `ProtectionRun` array (up to
/// `kMaxProtectionRunsPerEntry`). Replay applies the first run's prot
/// implicitly through the initial commit (the substrate uses
/// `meta.view_prot`), then iterates runs `[1..protection_count)` via
/// `nt_pal::protect` to reproduce the parent's sub-region protection.
struct ForkSnapshot {
  struct Entry {
    VaRange range;
    RegionKind kind;
    AcquireMeta meta;
    ProtectionRun protection_runs[kMaxProtectionRunsPerEntry];
    uint32_t protection_count;
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
