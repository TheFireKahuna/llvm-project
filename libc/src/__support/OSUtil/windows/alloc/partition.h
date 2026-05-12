//===-- alloc::partition --- type-isolated 4 GiB VA partitions ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Type-isolated 4 GiB virtual-address partitions for the allocator.
///
/// The partition layer owns the coarse VA windows that every other allocator
/// subsystem (buddy arena, segment / slab-page consumers, IndexedPool,
/// ThreadScratch) draws chunks from. It is the source of truth for the
/// query "is this VA allocator-owned?", consulted by the fault classifier
/// on every page fault to skip allocator-internal VA from POSIX-visible
/// tracking.
///
/// What the partition layer provides:
///
///   * Type-isolated VA disjointness across `(class, numa_node)` pairs.
///     Cross-class type-confusion attacks cannot reach across partition
///     boundaries: every partition is bracketed by `kPartitionGuardBytes`
///     of PAGE_NOACCESS placeholder guard, and partition VA ranges never
///     overlap.
///
///   * Compact-pointer enablement at 4 GiB stride. Within one partition,
///     a 32-bit offset uniquely identifies an allocation -- the Scudo
///     Primary64-style compact pointer scheme. Slab metadata can carry
///     32-bit references in place of 64-bit pointers.
///
///   * Two oracles, two queries, one descriptor pool:
///       - Coarse pagemap (flat index at 4 GiB stride, snmalloc (Liétar
///         et al., ISMM 2019) style): hot-path `lookup(addr)` resolves to
///         the owning descriptor with a single ACQUIRE load and one
///         bounds-check branch. Wait-free; ~3 ns warm.
///       - Reserve slot table (lock-free open-addressed hash on packed
///         `(class, node)`): cold-path `reserve_or_grow` deduplication
///         under contention. Hash + roughly 1.5 probes average.
///
///   * Demand reservation per `(class, numa_node)`. Idle classes consume
///     zero VA. The core libc-internal partitions are eager-reserved at
///     Tier A on the node-agnostic node (because they are guaranteed
///     needed); user-class partitions are reserved lazily on the first
///     allocation that maps to them.
///
///   * Event-driven retire (no polling, no heartbeat, no periodic sweep).
///     Retire fires inline when `decommit_chunk_unregister` observes both
///     `bytes_committed` and `active_chunks` drop to zero. The
///     decrementing thread CAS-flips LIVE -> DRAINING, clears the coarse
///     pagemap entry, clears the reserve slot, and hands the descriptor
///     to `g_partition_domain`. Crystalline-W (Nikolaev / Ravindran,
///     PLDI 2024) gates the free callback until in-flight `lookup()` pin
///     holders release; the callback issues MEM_RELEASE on the 4 GiB VA.
///
///   * Five-state retire machine:
///       - LIVE     - in use; commit/decommit accepted.
///       - IDLE     - counters dropped to zero; reusable. A subsequent
///                    commit re-arms to LIVE before the decrementing
///                    thread reaches retire.
///       - DRAINING - retire in progress; `commit_chunk_register` rejects.
///       - RETIRED  - descriptor about to be reaped by the free callback.
///       - PINNED   - core libc-internal partition; never retires.
///
/// Hard caller invariants:
///
///   1. `commit_chunk_register` must be called before any chunk commit
///      against the partition's VA. The state gate rejects DRAINING /
///      RETIRED with -EAGAIN; the caller must retry via `reserve_or_grow`
///      to obtain a fresh descriptor.
///
///   2. `decommit_chunk_unregister` must be called after every successful
///      decommit, with `chunk_bytes` matching the prior register call.
///      Mismatched bytes corrupt the empty-transition observation and
///      the free callback traps on the resulting non-zero counters.
///
///   3. The chunk owner is the sole writer of any chunk pagemap entry
///      within the partition's range. Cross-class chunks are forbidden
///      by partition class identity (a partition is bound to one class).
///
///   4. Lookup callers that intend to dereference the returned descriptor
///      must first pin `g_partition_domain` (Crystalline-W discipline).
///      The fault classifier only compares partition pointers; no pin
///      is required there.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PARTITION_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PARTITION_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/OSUtil/windows/alloc/partition_class.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_primitives_bootstrap.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {
namespace partition {

//===----------------------------------------------------------------------===//
// Retire-state machine
//===----------------------------------------------------------------------===//
//
// Encoded in `PartitionDescriptor::retire_state`. Transitions:
//
//   LIVE     -> IDLE       Counters reached zero in decommit_chunk_unregister.
//   IDLE     -> LIVE       commit_chunk_register re-armed; retire cancelled.
//   LIVE     -> DRAINING   try_retire_inline observed empty-transition and
//                          won the CAS.
//   DRAINING -> LIVE       Re-verification inside try_retire_inline observed
//                          a non-zero counter (a concurrent commit raced past
//                          the empty observation).
//   DRAINING -> RETIRED    Coarse pagemap and reserve slot have been cleared;
//                          descriptor handed to the Crystalline-W domain.
//   RETIRED  -> (free)     Crystalline-W advanced the epoch past every
//                          in-flight lookup pin; the free callback runs.
//
// PINNED is stamped at Tier A on the core libc-internal partitions and is
// non-transitioning. The retire CAS in try_retire_inline requires LIVE; on
// PINNED it fails unconditionally, so core partitions never retire.

inline constexpr uint32_t kStateLive = 0;
inline constexpr uint32_t kStateIdle = 1;
inline constexpr uint32_t kStateDraining = 2;
inline constexpr uint32_t kStateRetired = 3;
inline constexpr uint32_t kStatePinned = 4;

/// Per-partition runtime descriptor; 128 B, two-cache-line layout.
///
/// The descriptor is allocated from a sealed Zone 0 pool, never freed for the
/// process lifetime, and reused across partition lifecycles. A retire batch
/// may carry stale slot indices; the per-descriptor `descriptor_seq` (drawn
/// fresh from ProcessPrng on every initialisation) gives the canary enough
/// entropy that a replay against a recycled descriptor mismatches.
///
/// Cache-line layout:
///
///   - Line 0 (offsets 0..63): inherited CrystallineNode header plus the
///     read-mostly identity fields (`base`, `mask`, `bytes`, `key`,
///     `descriptor_seq`). `lookup()` touches only this line.
///   - Line 1 (offsets 64..127): atomic-mutated counters, `retire_state`,
///     guard pointers, canary. Commit/decommit hot path touches only this
///     line, so the lookup hot path's cache line is never invalidated by
///     `bytes_committed.fetch_add` / `fetch_sub` traffic.
///
/// The split is enforced structurally: `alignas(64)` on `bytes_committed`
/// forces it to offset 64, and the size/alignment static_asserts pin the
/// total layout. `offsetof` is unavailable on PartitionDescriptor under
/// strict C++ (the class inherits from `CrystallineNode` and adds non-static
/// members, so it is non-standard-layout).
///
/// Thread-ownership invariant: any thread may read line 0 wait-free; only
/// chunk-owner code mutates line 1's counters; the retire-state CAS chain in
/// `try_retire_inline` is the sole transition path between LIVE / IDLE /
/// DRAINING / RETIRED.
///
/// \see g_partition_domain for the Crystalline-W reclamation gate.
struct alignas(128) PartitionDescriptor
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
  // The Crystalline-W intrusive runtime fields are emitted directly via the
  // macro (instead of inherited from a Crystalline-internal base) so the
  // descriptor remains standard-layout-modulo-the-CrystallineNode-base. The
  // first member declared below (`base`, 8-byte aligned) lands at offset 24
  // with a 4-byte gap at [20..23].
  LIBC_CRYSTALLINE_NODE_FIELDS(PartitionDescriptor);

  // Cache line 0 -- read-mostly identity.
  void *base{};              ///< Partition VA, 4 GiB-aligned.
  uintptr_t mask{};          ///< `~(kPartitionBytes - 1)`; masks an address
                             ///< down to the partition base.
  size_t bytes{};            ///< Always `kPartitionBytes` today; carried for
                             ///< symmetry with chunk descriptors.
  uint32_t key{};            ///< Packed `(class, numa_node)` identity.
  uint32_t descriptor_seq{}; ///< Per-descriptor randomness drawn from
                             ///< ProcessPrng at init; canary input.

  // Cache line 1 -- atomic-mutated counters and retire state. The explicit
  // alignas(64) places this field (and all subsequent members) at offset 64,
  // i.e. the start of the second cache line. This keeps the lookup hot path
  // (line 0 only) coherence-isolated from the commit/decommit hot path
  // (line 1 only).
  alignas(64) cpp::Atomic<uint64_t> bytes_committed{};
  cpp::Atomic<uint32_t> active_chunks{};
  cpp::Atomic<uint32_t> retire_state{};
  void *leading_guard{};  ///< PAGE_NOACCESS placeholder of
                          ///< `kPartitionGuardBytes` at `base`.
  void *trailing_guard{}; ///< PAGE_NOACCESS placeholder of
                          ///< `kPartitionGuardBytes` at `base + bytes`.
  uint64_t canary{};      ///< `process_cookie ^ partition_secret ^
                          ///< uintptr_t(base) ^ descriptor_seq`. Validated by
                          ///< the free callback to detect tamper.

  [[nodiscard]] LIBC_INLINE PartitionClass class_id() const {
    return unpack_class(key);
  }
  [[nodiscard]] LIBC_INLINE uint16_t numa_node() const {
    return unpack_node(key);
  }
};

static_assert(sizeof(PartitionDescriptor) == 128,
              "PartitionDescriptor must occupy exactly two cache lines (128 B)");
static_assert(alignof(PartitionDescriptor) == 128,
              "PartitionDescriptor must be cache-line-pair aligned");

/// One slot of the open-addressed reserve table.
///
/// The descriptor pointer is itself the linearization point: a single CAS
/// publishes the entire descriptor, because the publisher's RELEASE store
/// synchronizes-with any ACQUIRE load of a non-null pointer. All descriptor
/// fields become visible to readers once a non-null slot is observed.
struct alignas(8) PartitionTableSlot {
  cpp::Atomic<PartitionDescriptor *> descriptor{};
};
static_assert(sizeof(PartitionTableSlot) == 8);

/// Lock-free open-addressed table that deduplicates concurrent
/// `reserve_or_grow` calls for the same `(class, numa_node)` pair.
///
/// Size is `kReserveTableSize` slots (a power of two) at 8 B each. The whole
/// table fits in a handful of cache lines, so a linear probe across the table
/// is cheap relative to the 4 GiB-VA reservation syscall it deduplicates.
struct alignas(64) ReserveTable {
  PartitionTableSlot slots[kReserveTableSize]{};
};
static_assert(sizeof(ReserveTable) == kReserveTableSize * 8);

/// Flat partition-granularity index for the hot-path `lookup()`.
///
/// One entry per `kPartitionBytes` (4 GiB) slice of user VA. Indexed by
/// `(addr - coverage_base) >> kPartitionShift`. Lookup is one ACQUIRE load
/// and one predictable bounds-check branch.
///
/// Backing storage is `kCoarseMaxEntries * sizeof(void*)` (256 KiB at the
/// default capacity), reserved at Tier A. The OS commits pages lazily on
/// first store, so a typical workload (<= 64 active partitions) commits
/// roughly a single OS page for the entire table.
///
/// The flat layout follows snmalloc's pagemap design (Liétar et al.,
/// ISMM 2019): wait-free single-load lookup at the cost of one allocation
/// of contiguous VA, with cold pages staying uncommitted.
struct alignas(64) CoarsePagemap {
  void *coverage_base;   ///< User-VA base, aligned to `kPartitionBytes`.
  size_t coverage_bytes; ///< `(max_user_va_aligned_down - coverage_base)`.
  uint8_t _pad[64 - 16]; ///< Separates the header from `entries` so the
                         ///< first entry starts a fresh cache line.
  cpp::Atomic<PartitionDescriptor *> entries[kCoarseMaxEntries];
};

/// Crystalline-W retire frequency for the partition descriptor domain.
///
/// Half of the buddy arena's value (8). Partition retires fire only on
/// empty-transition (rare per process lifetime), so each thread's retire
/// batch stays small; the maximum residual is 3 nodes per thread, drained
/// at thread exit. Lower frequency keeps the per-thread retire-batch slab
/// footprint small.
constexpr uint32_t kPartitionRetireFreq = 4;

void partition_free_descriptor(PartitionDescriptor *desc);

/// MaxIdx for the partition domain. Call sites: init_node / retire
/// only — no protect()/anchor() pins. MaxIdx = 1 sizes the (unused)
/// reservation slot space minimally.
inline constexpr uint32_t kPartitionMaxIdx = 1;

/// Process-wide Crystalline-W (Nikolaev / Ravindran, PLDI 2024) domain
/// governing partition descriptor reclamation. The free callback runs only
/// after every concurrent `lookup()` pin holder has released, so a wait-free
/// reader can never observe a freed descriptor.
extern ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    PartitionDescriptor, &partition_free_descriptor, kPartitionRetireFreq,
    kPartitionMaxIdx>
    g_partition_domain;

} // namespace partition
} // namespace alloc
} // namespace windows

namespace concurrent {
/// Codec specialization mapping `PartitionDescriptor *` to its pool index
/// for Crystalline-W's batch-link encoding.
///
/// The pool base is read from sealed Zone 0, so an attacker with an
/// arbitrary-write primitive cannot redirect the encode/decode pair. Pool
/// capacity is `kPartitionDescPoolCapacity` (256), so the index fits in 8
/// bits; the codec output (index + 1) is always non-zero and well under
/// bit 31. The specialization lives in the header so every consumer of
/// `g_partition_domain` sees it at the point of CrystallineDomain
/// instantiation.
template <>
struct BatchLinkCodec<
    ::LIBC_NAMESPACE::windows::alloc::partition::PartitionDescriptor> {
  using Node =
      ::LIBC_NAMESPACE::windows::alloc::partition::PartitionDescriptor;
  LIBC_INLINE static uint32_t encode(CrystallineNode *n) noexcept {
    auto *base = static_cast<Node *>(
        ::LIBC_NAMESPACE::g_pcb.zone0.partition_desc_pool_base());
    return 1u + static_cast<uint32_t>(static_cast<Node *>(n) - base);
  }
  LIBC_INLINE static CrystallineNode *decode(uint32_t code) noexcept {
    auto *base = static_cast<Node *>(
        ::LIBC_NAMESPACE::g_pcb.zone0.partition_desc_pool_base());
    return &base[code - 1u];
  }
};
} // namespace concurrent

namespace windows {
namespace alloc {
namespace partition {

//===----------------------------------------------------------------------===//
// Public API -- hot path
//===----------------------------------------------------------------------===//

/// Resolve `addr` to its owning partition descriptor, or `nullptr` if the
/// address falls outside every allocator partition.
///
/// Wait-free; warm latency ~3 ns. Called by the fault classifier on every
/// page fault to keep allocator-internal VA out of POSIX-visible tracking.
///
/// The single ACQUIRE load on the coarse-pagemap entry synchronizes-with the
/// RELEASE store inside `reserve_or_grow` that publishes a descriptor, so a
/// non-null result carries fully-visible `desc->base`, `desc->mask`,
/// `desc->key`, etc.
///
/// Caller pin discipline: a caller that intends to dereference the returned
/// pointer must hold a Crystalline-W pin on `g_partition_domain` across the
/// dereference. `lookup()` itself does not pin -- a returned pointer may be
/// in a retire batch whose epoch has not yet advanced. The fault classifier
/// only compares pointers for identity and skips dereference, so it needs no
/// pin.
///
/// \returns the owning `PartitionDescriptor *`, or `nullptr` if `addr` is
///          outside every partition's VA range.
[[nodiscard]] LIBC_INLINE PartitionDescriptor *lookup(const void *addr) {
  // The pagemap pointer is read non-const because `cpp::Atomic::load` is a
  // non-const member. Conceptually the table is read-mostly from a lookup
  // caller's perspective; the const-correctness boundary of `cpp::Atomic`
  // forces the non-const handle. Matches the convention used by
  // `pagemap_slot_unchecked`.
  auto *coarse = static_cast<CoarsePagemap *>(
      ::LIBC_NAMESPACE::g_pcb.zone0.partition_coarse_pagemap());
  if (LIBC_UNLIKELY(coarse == nullptr))
    return nullptr;
  uintptr_t a = reinterpret_cast<uintptr_t>(addr);
  uintptr_t base = reinterpret_cast<uintptr_t>(coarse->coverage_base);
  if (LIBC_UNLIKELY(a < base || a - base >= coarse->coverage_bytes))
    return nullptr;
  size_t idx = (a - base) >> kPartitionShift;
  // ACQUIRE pairs with the RELEASE publication in `reserve_or_grow` and the
  // RELEASE clearing-store in `try_retire_inline`; a non-null result is a
  // descriptor whose fields are fully published.
  return coarse->entries[idx].load(cpp::MemoryOrder::ACQUIRE);
}

/// Returns true when `addr` falls inside any allocator partition.
[[nodiscard]] LIBC_INLINE bool is_managed(const void *addr) {
  return lookup(addr) != nullptr;
}

//===----------------------------------------------------------------------===//
// Public API -- cold paths (reservation, chunk-register/unregister, retire)
//===----------------------------------------------------------------------===//

/// Demand-reserve a 4 GiB partition for `(cls, numa_node)`, returning either
/// the freshly published descriptor or the descriptor previously published
/// by a peer.
///
/// Same-`(cls, node)` thunder is serialized through the deterministic primary
/// slot plus linear probing: only the CAS winner's syscall reservation is
/// kept; losers roll back theirs and return the winner's descriptor.
///
/// The protocol is four-phase: (1) probe the reserve table for an existing
/// entry; (2) call `nt_pal::reserve_placeholder_*` outside any sentinel
/// claim, so a thread re-entering the partition layer mid-syscall cannot
/// self-deadlock; (3) RELEASE-publish into the coarse pagemap; (4) CAS-
/// publish into the reserve table. If the reserve-table CAS loses to a peer
/// with the same key, the loser rolls back phases 3, 2 and the descriptor
/// allocation.
///
/// On affined `numa_node` failure the function falls through to a "same-
/// class replica" scan: any previously-published `(cls, *)` peer is a
/// strictly better answer than ENOMEM, since cross-node free is unchanged
/// and NT first-touch still places committed pages on the requesting CPU's
/// node regardless of the partition's reservation hint.
///
/// \param cls       Partition class identity.
/// \param numa_node NUMA node hint, or `kNodeAgnostic` for "no preference".
/// \returns a descriptor, or `nullptr` if the underlying VA reservation
///          fails, the descriptor pool is exhausted, or the reserve table
///          is full (more than `kReserveTableSize` distinct active
///          `(cls, node)` pairs).
[[nodiscard]] PartitionDescriptor *
reserve_or_grow(PartitionClass cls, uint16_t numa_node);

/// Counter-bookkeeping and retire-state gate for a fresh chunk commit.
///
/// The chunk owner calls this *before* committing a chunk into the
/// partition's VA. Validates `retire_state` (accepts LIVE / PINNED;
/// re-arms IDLE -> LIVE via CAS; rejects DRAINING / RETIRED), then
/// increments `bytes_committed` and `active_chunks`.
///
/// Most consumers should use the unified `commit_chunk` below, which
/// bundles this counter step with the underlying placeholder split and
/// commit-replace and pagemap publish. The raw register entry point
/// remains exposed for the rare consumer that wants counter bookkeeping
/// without the kernel pair.
///
/// \returns 0 on success, `-EINVAL` on a null descriptor, `-EAGAIN` if
///          the partition is mid-retire (caller restarts via
///          `reserve_or_grow`).
[[nodiscard]] int commit_chunk_register(PartitionDescriptor *desc,
                                         void *chunk_base,
                                         size_t chunk_bytes);

/// Counter-bookkeeping and empty-transition retire trigger.
///
/// The chunk owner calls this *after* decommitting a chunk. Decrements
/// `bytes_committed` and `active_chunks`; if both reach zero while
/// `retire_state` is LIVE, the calling thread inlines the retire path
/// (CAS LIVE -> DRAINING, re-verify counters, clear coarse pagemap entry,
/// clear reserve slot, mark RETIRED, hand to `g_partition_domain`).
///
/// As with `commit_chunk_register`, prefer the unified `decommit_chunk`
/// for typical consumers.
///
/// \returns the post-decrement `bytes_committed` value.
size_t decommit_chunk_unregister(PartitionDescriptor *desc,
                                  void *chunk_base, size_t chunk_bytes);

/// Unified chunk commit: placeholder split + commit-replace + counter
/// bookkeeping + pagemap registration + pagemap publish, all rolled back
/// atomically on any step's failure.
///
/// Steps:
///   1. `nt_pal::split_placeholder(chunk_base, chunk_bytes)` carves the
///      chunk-sized hole out of the partition's middle placeholder.
///   2. `nt_pal::commit_replace(chunk_base, chunk_bytes, page_prot)`
///      replaces the placeholder with committed pages. Always passes
///      MEM_WRITE_WATCH so the hardware dirty bitmap is armed for fork
///      CoW preservation and quarantine sweeps.
///   3. `commit_chunk_register` updates partition counters and gates on
///      retire state.
///   4. `pagemap_register_range` upgrades the pagemap OS pages covering
///      this range to PAGE_READWRITE.
///   5. `pagemap_publish_range` publishes `(slot_idx, tag)` to every
///      `kPagemapChunkBytes` sub-entry the chunk covers.
///
/// On failure at any step, earlier steps are rolled back in reverse order
/// (`pagemap_retire_range`, `decommit_preserve`, `commit_chunk_unregister`,
/// `coalesce_placeholders`).
///
/// Caller invariants:
///   - `chunk_base` and `chunk_bytes` are aligned to the pagemap stamp
///     granularity (`kPagemapChunkBytes`, 64 KiB). Asserted in debug.
///   - `chunk_base` lies in `desc`'s middle placeholder, between the
///     leading and trailing guards.
///   - `pagemap_tag` matches the consumer's `VaChunkConsumer` enum value.
///   - `pagemap_slot_idx` is opaque to the partition layer; the consumer
///     decodes it on lookup.
///
/// \returns 0 on success; `-EAGAIN` if the partition is mid-retire (caller
///          restarts via `reserve_or_grow`); `-EIO` on syscall failure
///          (partition VA may be in an in-between placeholder state; the
///          caller should advance to the next chunk slot).
[[nodiscard]] int commit_chunk(PartitionDescriptor *desc, void *chunk_base,
                                size_t chunk_bytes, DWORD page_prot,
                                ::LIBC_NAMESPACE::windows::alloc::VaChunkConsumer
                                    pagemap_tag,
                                uint32_t pagemap_slot_idx);

/// Unified chunk decommit. Runs the inverse of `commit_chunk` in
/// reverse-step order; pagemap retire happens first so a concurrent fault
/// classifier observes "not tracked" before pages are decommitted and
/// partition counters drop. Failures are best-effort: the partition stays
/// consistent even if a kernel call fails partway, but the affected VA
/// may leak as a placeholder.
void decommit_chunk(PartitionDescriptor *desc, void *chunk_base,
                    size_t chunk_bytes);

/// Compute the descriptor canary from its identity.
///
/// Centralized so the init path, free-callback validation, and fork
/// reinit all derive the same value from the same inputs. Inputs:
/// `process_cookie` and `partition_secret` (both in sealed Zone 0b,
/// re-rolled on fork), `base` (immutable after publish), and
/// `descriptor_seq` (immutable for the descriptor's pool tenancy).
[[nodiscard]] uint64_t compute_canary(void *base, uint32_t descriptor_seq);

//===----------------------------------------------------------------------===//
// Tier A bootstrap
//===----------------------------------------------------------------------===//

/// Tier A initialiser, registered via `LIBC_REGISTER_MEMORY_PRIMITIVE` at
/// memory phase 5. Reserves the coarse pagemap, the descriptor pool, and
/// the eager core libc-internal partitions; pins each core partition by
/// stamping retire_state = PINNED.
///
/// \param out Receipt array to fill with reservation records that the
///            substrate registry will mark as LIBC_INTERNAL.
/// \param cap Capacity of `out`.
/// \returns the number of receipts written.
uint32_t partition_init_fn(::LIBC_NAMESPACE::internal::Receipt *out,
                            uint32_t cap);

//===----------------------------------------------------------------------===//
// Fork reinit
//===----------------------------------------------------------------------===//

/// Fork-child reinit, registered at `LibcForkReinitPriority::kForkPrioPartition`.
///
/// Re-rolls `partition_secret` (under the Zone 0b unseal window held open
/// by `libc_fork_reinit_impl`) and walks the reserve table to refresh
/// every descriptor's canary against the new `process_cookie` plus new
/// `partition_secret`. The init latch is reset so a fork that happened
/// mid-init re-runs cleanly.
void partition_fork_reinit();

//===----------------------------------------------------------------------===//
// Diagnostics
//===----------------------------------------------------------------------===//

/// Aggregate snapshot of the partition layer's live state. Used by tests.
struct PartitionStats {
  uint32_t total_partitions_live;   ///< Distinct live partition descriptors.
  uint32_t descriptor_pool_used;    ///< Occupancy bitmap popcount.
  uint64_t total_va_reserved_bytes; ///< Sum of `desc->bytes` across live
                                    ///< partitions.
  uint64_t total_committed_bytes;   ///< Sum of `desc->bytes_committed`.
};

[[nodiscard]] PartitionStats partition_stats_snapshot();

} // namespace partition
} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
