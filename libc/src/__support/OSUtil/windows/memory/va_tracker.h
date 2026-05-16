//===- va_tracker.h - POSIX-visible VA tracker public API -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Public façade tracking every POSIX-visible mapping. Outer ROWEX-synchronised
// ART (Leis, Kemper, Neumann - ICDE 2013; Leis et al. - DaMoN 2016) partitions
// the 47-bit user VA into 4 GiB-aligned leaves; inner per-arena concurrent
// interval skiplist (Kim, Kwon, Kang - SOSP 2025) holds the per-region descs.
//
// Mutations execute as a single atomic envelope per arena:
// Lock + nt_pal phase + Swap + reap + Unlock. Multi-arena ranges decompose
// into N independent per-arena envelopes, not a transactional N-tuple.
//
// Reads run wait-free under per-pointer Crystalline-W pins (Nikolaev,
// Ravindran - PLDI 2024) so the SIGSEGV classifier is bounded even under
// writer contention. Allocator-private VA bypasses this tracker entirely.
//
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

// Half-open `[start, start + bytes)`.
//
// Interior ops require 4 KiB alignment. The `acquire` family additionally
// requires 64 KiB (NT `MEM_RESERVE_PLACEHOLDER` base granularity). Misaligned,
// empty, or wrapping ranges are rejected with `Error(EINVAL)`.
struct VaRange {
  void *start{nullptr};
  size_t bytes{0};

  [[nodiscard]] LIBC_INLINE uintptr_t lo() const {
    return reinterpret_cast<uintptr_t>(start);
  }
  [[nodiscard]] LIBC_INLINE uintptr_t hi() const { return lo() + bytes; }
  [[nodiscard]] LIBC_INLINE bool empty() const { return bytes == 0; }
};

// Numeric values are public ABI — do not renumber. Cordon kinds (Image /
// Kernel / Foreign) live in `CordonKind`; libc-internal VA bypasses the
// tracker entirely.
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

// Valid only while the caller's Crystalline-W pin on the skiplist domain is
// held. The next `va_tracker` call on this thread rotates the pin slot and
// invalidates any previously-returned `RegionRef`.
struct RegionRef {
  RegionDesc *desc{nullptr};

  [[nodiscard]] LIBC_INLINE bool valid() const { return desc != nullptr; }
};

//===----------------------------------------------------------------------===//
// AcquireMeta — input to `acquire` and `replace`.
//===----------------------------------------------------------------------===//

// Per-desc fields (view_prot, flags, section_offset) land in the freshly
// built `RegionDesc`. Backing-resident fields (handles + placeholder
// identity) land in a freshly allocated `DescBacking` shared between every
// desc that maps the same source via a `BackingRef`.
struct AcquireMeta {
  DWORD view_prot{0};
  uint16_t flags{0};
  uint64_t section_offset{0};

  HANDLE section_handle{nullptr};
  HANDLE file_handle{nullptr};

  // Optional placeholder identity `[placeholder_base, placeholder_base +
  // placeholder_size)` when the kernel reservation must exceed the
  // registered range — brk (256 MiB shot, narrow registered range),
  // alignment headroom from `posix_memalign`, mremap grow-into-headroom.
  // Both zero means "identity equals `range`".
  void *placeholder_base{nullptr};
  size_t placeholder_size{0};
};

// Invoked by `mutate` on a fresh clone of each intersecting desc, inside
// the nt_pal phase under the LOCKED hold. The clone is published via Swap;
// reader-visible descs are never mutated in place.
using DescMutator = void (*)(RegionDesc *new_desc, void *ctx);

// Per-chunk gate for `mutate`'s `prot_change` path. Returns true to apply
// `prot_change` to the chunk, false to skip it. Invoked once per MBI chunk
// inside the LOCKED envelope; the substrate guarantees both pointers are
// non-null at call time. The filter must not block, must not re-enter any
// `va_tracker` op, and must not store the `desc` pointer past return — it
// is valid for the call duration only. `mbi` carries MBI State / Type /
// AllocationProtect for the chunk; `desc` is the OLD desc covering the
// chunk's VA (read under the LOCKED hold).
using MutateChunkFilter = bool (*)(const MEMORY_BASIC_INFORMATION *mbi,
                                   RegionDesc *desc);

// Invoked by `walk_range` for each intersecting desc, in ascending VA order.
using WalkVisitor = void (*)(VaRange covered, RegionDesc *desc, void *ctx);

//===----------------------------------------------------------------------===//
// Public typed operations.
//
// Common error codes (positive errno at the public surface; per-op decls
// below quote the engine's `-errno` convention):
//   EINVAL  Alignment / bounds violation; empty or wrapping range; null
//           mutator on the plain `mutate` path. (Straddlers are auto-split
//           atomically under the LOCKED hold — callers do NOT pre-split.)
//   EEXIST  `acquire` saw STATUS_CONFLICTING_ADDRESSES (range not
//           MEM_FREE).
//   ENOENT  `mutate` / `split` over a range with no covering desc.
//           `release` of an unmapped range degrades to a no-op per POSIX
//           munmap semantics.
//   ENOTSUP `release` / `replace` / `mutate` on a non-anon-placeholder
//           shape; `replace` on a wider shared backing whose siblings
//           disagree on prot / flags / kind, or whose extended-lock edge
//           abuts another chain.
//   ENOMEM  Partition / chunk / Crystalline pool exhaustion; kernel
//           STATUS_NO_MEMORY.
//   E2BIG   Provisional or candidate inline cap exceeded (> 16 backings
//           touched in one envelope; pathological).
//   EAGAIN  Bounded-retry exhaustion under contention.
//   EFAULT  Unexpected NTSTATUS from a kernel op the engine cannot
//           classify.
//===----------------------------------------------------------------------===//

// Atomicity is single-arena. A `range` spanning multiple ART leaves is
// decomposed into N per-arena envelopes; on any sub-envelope failure
// `acquire` rolls back every committed slice (via `release`) plus any
// placeholder VAD the scout established, so the caller observes the
// first-failed errno against a fully-rolled-back state.
//
// `release` / `replace` / `mutate` do NOT auto-rollback across arenas;
// they return the first sub-envelope's errno and leave earlier slices
// committed. Callers that need all-or-nothing across multiple arenas
// must wrap with their own `release` of the full range.
//
// Precondition: `range` is `MEM_FREE` — use `replace` to overwrite existing
// mappings. The returned ref is pinned on `kPinSlotCur` until the next
// va_tracker call on this thread.
[[nodiscard]] ::LIBC_NAMESPACE::ErrorOr<RegionRef>
acquire(VaRange range, RegionKind kind, const AcquireMeta &meta);

// `mmap(NULL, len, ...)` path. The scout reserves a placeholder before
// the envelope runs and the envelope reuses that reservation; the VA
// is MEM_RESERVE (never MEM_FREE) between scout and commit, closing the
// race a POSIX-side scout-loop would otherwise have to retry through.
// On envelope failure the placeholder is released before errno returns.
//
// Precondition: `bytes > 0` and 64 KiB-multiple.
[[nodiscard]] ::LIBC_NAMESPACE::ErrorOr<void *>
acquire_kernel_chosen(size_t bytes, RegionKind kind,
                      const AcquireMeta &meta);

// MAP_32BIT variant: scout uses `nt_pal::reserve_placeholder_32bit` so the
// chosen base lands in the low 2 GiB. Same race-free envelope contract.
[[nodiscard]] ::LIBC_NAMESPACE::ErrorOr<void *>
acquire_kernel_chosen_32bit(size_t bytes, RegionKind kind,
                            const AcquireMeta &meta);

// Drops every desc overlapping `range`. Edge straddlers are split atomically
// under the LOCKED hold and the outside slice is republished carrying OLD
// metadata — callers do not pre-split and observe no split/release race.
// The post-Swap survivor walk synchronously tears down any backing that
// loses its last LIVE referencer; fully-released VA is `MEM_FREE` on return.
[[nodiscard]] int release(VaRange range);

// Atomic `MAP_FIXED` over `range`.
//
// OLD kernel state is demoted back to a placeholder
// (`unmap_view_preserve` for section views, `decommit_preserve` clipped to
// the inside extent for anon), `coalesce_placeholders` joins fragments
// only when gap-fill actually filled MEM_FREE gaps (skip the no-op syscall
// on the contiguous case), then `commit_replace`/`map_section_replace`
// installs the new mapping. Old-backing placeholder ownership is
// transferred to the new backing — the reaper cannot double-free. VA never
// crosses MEM_FREE so no freeze bracket is needed.
//
// Edge straddlers are split atomically under the LOCKED hold; callers do
// not pre-split.
//
// Partial-replace of a wider shared backing: when an OLD inside-desc
// references a backing whose extent crosses out to an outside-`range`
// desc, the engine extends its internal lock to the backing's full
// extent, splits the placeholder at the range edges (section views) or
// leaves the surviving slice committed (private commits), re-maps the
// surviving sibling slice over the OLD section handle (section views)
// or carries it as pure metadata (private commits), and rebinds
// outside-range survivor descs to fresh sibling backings. Per-fragment
// kernel work stays O(1) regardless of survivor count.
//
// Returns `-ENOTSUP` when descs sharing a wider backing disagree on
// view_prot / flags / kind, or when an extended-lock edge abuts another
// chain.
[[nodiscard]] int replace(VaRange range, RegionKind kind,
                          const AcquireMeta &meta);

// Applies `mutator` to a fresh clone of every intersecting desc and
// publishes the clones via Swap. Clones share the source's `BackingRef`
// verbatim (pure metadata) so the survivor walk skips reaping.
//
// Edge straddlers are split atomically into up to three clones
// (left-outside, inside, right-outside) under the LOCKED hold. The mutator
// and any per-VAD protect both clip to the inside slice — matching POSIX
// `mprotect`'s "touch only `[addr, addr + len)`" contract.
//
// When `prot_change != 0` the engine issues `nt_pal::protect(inside,
// prot_change)` inside the locked envelope. NUMA-rebind and other pure-
// metadata mutators pass 0.
//
// When `commit_if_uncommitted_accessible`, the post-Swap phase replaces
// the per-VAD protect with a per-chunk walk: committed → protect;
// uncommitted+accessible+MEM_MAPPED → `commit_in_reservation`;
// uncommitted+accessible+MEM_PRIVATE → `commit_replace[_numa]` with
// `numa_node`; uncommitted+PROT_NONE → no-op. MEM_FREE mid-range aborts
// with ENOMEM. COW is translated per-succ from the OLD desc's
// `region_flag::COW`. The clone's `view_prot` is NOT updated — the
// kernel holds authoritative current protection and consumers query MBI;
// acquire-time intent stays preserved. `mutator` MAY be null on this
// path because the protection write absorbs the substrate's coverage.
//
// When `chunk_filter != nullptr` and `prot_change != 0`, the post-Swap
// phase walks each locked succ per-MBI-chunk and applies `prot_change`
// only to chunks for which `chunk_filter(&mbi, desc)` returns true.
// Restrictive protections valid on a subset of pages (notably
// `PAGE_REVERT_TO_FILE_MAP`, accepted only on file-backed CoW pages)
// require this path — otherwise the kernel rejects the per-VAD protect
// with `STATUS_INVALID_PARAMETER` on the first ineligible chunk and the
// op returns `-EFAULT`. `mutator` MAY be null on this path. Mutually
// exclusive with `commit_if_uncommitted_accessible` — if both are set,
// `commit_if_uncommitted_accessible` wins.
[[nodiscard]] int mutate(VaRange range, DescMutator mutator, void *ctx,
                         DWORD prot_change = 0,
                         bool commit_if_uncommitted_accessible = false,
                         int numa_node = -1,
                         MutateChunkFilter chunk_filter = nullptr);

// Splits the single covering desc into two clones sharing the source's
// `BackingRef`. No kernel work. Returns -ENOENT for no covering desc and
// -EINVAL when `boundary` is not a 4 KiB-aligned strict interior — NT
// accepts placeholder splits at any page-aligned offset.
[[nodiscard]] int split(void *boundary);

//===----------------------------------------------------------------------===//
// Reads — wait-free, AS-safe under per-pointer Crystalline-W pin.
//===----------------------------------------------------------------------===//

// Wait-free; SIGSEGV-callable. The returned ref is pinned on `kPinSlotCur`
// until the next va_tracker call on this thread rotates the slot — no
// explicit drop.
[[nodiscard]] ::LIBC_NAMESPACE::ErrorOr<RegionRef> resolve(void *addr);

// Ordered iteration in ascending VA. The visitor MUST NOT re-enter a
// mutating va_tracker op — re-entrant Map from inside a pinned walk is UB.
void walk_range(VaRange range, WalkVisitor visitor, void *ctx);

//===----------------------------------------------------------------------===//
// Fork serialization and replay.
//===----------------------------------------------------------------------===//

// One uniform-protection sub-run from MBI. The serializer emits these so
// the child reproduces sub-region mprotect divergence — `desc->view_prot`
// carries acquire-time intent only, the kernel holds authoritative per-
// page protection. The first run's `prot` equals `AcquireMeta::view_prot`
// and is applied by `acquire`'s initial commit; runs `[1..)` are applied
// post-acquire via `nt_pal::protect`. Sub-runs cover the entry's range
// without gaps or overlap; bytes are page-aligned.
struct ProtectionRun {
  uint32_t offset_from_range_lo;
  uint32_t bytes;
  DWORD    prot;
  uint32_t reserved_; // padding to 16 bytes
};

// Real workloads have <= 3 runs per desc; 16 is headroom. On overflow the
// serializer collapses to one uniform run carrying `meta.view_prot`.
inline constexpr uint32_t kMaxProtectionRunsPerEntry = 16;

// The `runs` pointer is transient — valid only for the duration of the
// emit call; copy out if storage is needed. `run_count` is always >= 1.
struct ForkSink {
  using EmitFn = int (*)(void *ctx, VaRange r, RegionKind k,
                         const AcquireMeta &m,
                         const ProtectionRun *runs, uint32_t run_count);
  EmitFn emit{nullptr};
  void *ctx{nullptr};
};

// Entries replay in any order — each is an independent atomic envelope.
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

// Cordon / image / kernel / libc-internal shapes are filtered out — the
// child re-establishes them through its own loader and bootstrap.
[[nodiscard]] int serialize_for_fork(ForkSink &sink);

// Stops at the first failure and returns its errno value.
[[nodiscard]] int replay_in_child(const ForkSnapshot &snap);

// Runs at fork priority 39 — after partition (38) so partition state is
// rebuilt, before memory-reconcile (50) so reconciliation sees the fresh
// tracker structure.
void va_tracker_fork_reinit();

// Drops every per-thread Crystalline-W reservation on every va_tracker
// domain so the parent's snapshot does not capture this thread's pin as a
// held era — would otherwise leak memory until the child clears it.
void pre_fork_drain();

//===----------------------------------------------------------------------===//
// Bootstrap and internal exposed helpers.
//===----------------------------------------------------------------------===//

[[nodiscard]] bool is_va_tracker_ready();

// Exposed for the engine's cross-arena dispatcher; not POSIX surface. New
// arenas are pinned to the current CPU at install and canonical for every
// subsequent op on that VA prefix.
[[nodiscard]] Arena *resolve_or_install_arena(uintptr_t va);

// Registers the ART and skiplist Crystalline domains, primes the load-key
// callback, brings the backing pool online, and warms every per-thread
// slot on the current thread.
uint32_t va_tracker_init_fn(::LIBC_NAMESPACE::internal::Receipt *out,
                            uint32_t cap);

//===----------------------------------------------------------------------===//
// Diagnostics.
//===----------------------------------------------------------------------===//

// Sampled without synchronisation; values are best-effort during
// concurrent mutation.
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
