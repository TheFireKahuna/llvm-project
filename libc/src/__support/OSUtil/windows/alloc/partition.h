//===-- alloc::partition --- type-isolated 4 GiB VA partitions ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Type-isolated 4 GiB virtual-address partitions for the allocator.
//
// Owns the coarse VA windows every other allocator subsystem (buddy_arena,
// segment/slab_page, IndexedPool, ThreadScratch) draws chunks from, and is
// the source of truth for "is this VA allocator-owned?" -- consulted by the
// fault classifier on every page fault to skip allocator-internal VA.
//
// Three load-bearing properties:
//   * Type isolation across `(class, numa_node)`. Cross-class type confusion
//     cannot reach across partition boundaries; every partition is bracketed
//     by `kPartitionGuardBytes` of PAGE_NOACCESS placeholder.
//   * Compact pointers at 4 GiB stride (Scudo Primary64 style): a 32-bit
//     intra-partition offset uniquely identifies an allocation.
//   * Two oracles. Hot-path `lookup(addr)` resolves through a coarse pagemap
//     (snmalloc-style flat index; Liétar et al., ISMM 2019) in one ACQUIRE
//     load + bounds check, wait-free, ~3 ns warm. Cold-path `reserve_or_grow`
//     deduplicates concurrent same-`(class, node)` reservations through a
//     lock-free open-addressed slot table on the packed key.
//
// Reservation is demand-driven per `(class, numa_node)`. Core libc-internal
// partitions are eager-reserved at Tier A and PINNED; user-class partitions
// are reserved lazily and may retire.
//
// Retire is event-driven (never polled): `decommit_chunk_unregister`
// observing both counters at zero with state LIVE inlines the retire path.
// The Crystalline-W (Nikolaev / Ravindran, PLDI 2024) free callback fires
// only once every concurrent `lookup()` pin holder has released.
//
// Partners: partition_class.h (taxonomy + sizing constants),
// partition_numa_select.h (NUMA selector), pagemap.{h,cpp} (per-chunk
// publish/retire beneath `commit_chunk`/`decommit_chunk`),
// buddy_arena.{h,cpp} (principal chunk consumer),
// sealed_va_publisher.{h,cpp} (cross-subsystem VA-disjointness validation).
//
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

// Retire state, encoded in `PartitionDescriptor::retire_state`. Transitions
// live in partition.cpp; the only path out of LIVE is try_retire_inline,
// which requires LIVE so PINNED partitions never retire. IDLE is reserved
// for a future "park instead of retire" optimisation; `commit_chunk_register`
// handles the IDLE -> LIVE re-arm so the path is forward-compatible, but
// today no producer emits IDLE.
inline constexpr uint32_t kStateLive = 0;
inline constexpr uint32_t kStateIdle = 1;
inline constexpr uint32_t kStateDraining = 2;
inline constexpr uint32_t kStateRetired = 3;
inline constexpr uint32_t kStatePinned = 4;

// Per-partition runtime descriptor; 128 B, two-cache-line layout.
//
// Allocated from a sealed Zone 0 pool and never freed for process lifetime: a
// Crystalline-W retire batch may carry a stale slot index that re-resolves
// to a fresh allocation through the same slot, so `descriptor_seq` (fresh
// ProcessPrng draw per init) re-establishes per-tenancy identity through the
// canary -- a replay against a recycled slot mismatches.
//
// Cache-line split is structural, not advisory: `alignas(64)` on
// `bytes_committed` pins line-1 mutator state at offset 64, keeping
// fetch_add/fetch_sub coherence traffic off the line-0 lookup hot path.
// `offsetof` is unavailable here (non-standard-layout via CrystallineNode);
// size/alignment static_asserts pin the layout.
//
// Thread-ownership: any thread may read line 0 wait-free; only chunk-owner
// code mutates line 1's counters; the retire-state CAS chain in
// `try_retire_inline` is the sole transition path.
struct alignas(128) PartitionDescriptor
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
  // Crystalline-W intrusive runtime fields are emitted directly via the macro
  // (rather than inherited from a Crystalline-internal base) so the descriptor
  // remains standard-layout-modulo-the-CrystallineNode-base. The first member
  // declared below (`base`, 8-byte aligned) lands at offset 24 with a 4-byte
  // gap at [20..23].
  LIBC_CRYSTALLINE_NODE_FIELDS(PartitionDescriptor);

  // Cache line 0 -- read-mostly identity.
  void *base{};              // Partition VA, 4 GiB-aligned.
  uintptr_t mask{};          // `~(kPartitionBytes - 1)`.
  size_t bytes{};            // Always `kPartitionBytes`; carried for symmetry
                             // with chunk descriptors.
  uint32_t key{};            // Packed `(class, numa_node)`.
  uint32_t descriptor_seq{}; // ProcessPrng per-tenancy seed; one of the four
                             // XOR inputs to `canary`.

  // Cache line 1 -- atomic-mutated counters and retire state. `alignas(64)`
  // pins this field at offset 64.
  alignas(64) cpp::Atomic<uint64_t> bytes_committed{};
  cpp::Atomic<uint32_t> active_chunks{};
  cpp::Atomic<uint32_t> retire_state{};
  void *leading_guard{};  // PAGE_NOACCESS placeholder at `base`.
  void *trailing_guard{}; // PAGE_NOACCESS placeholder at `base + bytes`.
  uint64_t canary{};      // process_cookie ^ partition_secret ^ base ^
                          // descriptor_seq; free callback traps on mismatch.

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

// One slot of the open-addressed reserve table. The descriptor pointer is
// itself the linearization point: the publisher's RELEASE CAS synchronizes-
// with any ACQUIRE load of a non-null pointer.
struct alignas(8) PartitionTableSlot {
  cpp::Atomic<PartitionDescriptor *> descriptor{};
};
static_assert(sizeof(PartitionTableSlot) == 8);

// Lock-free open-addressed table deduplicating concurrent `reserve_or_grow`
// for the same `(class, numa_node)`. `kReserveTableSize` slots at 8 B each
// fit in a few cache lines, so a linear probe is cheap relative to the
// 4 GiB-VA reservation syscall it deduplicates.
struct alignas(64) ReserveTable {
  PartitionTableSlot slots[kReserveTableSize]{};
};
static_assert(sizeof(ReserveTable) == kReserveTableSize * 8);

// Flat partition-granularity index backing the hot-path `lookup()`. One entry
// per `kPartitionBytes` (4 GiB) slice of user VA; indexed by
// `(addr - coverage_base) >> kPartitionShift`. The OS commits pages lazily
// on first store, so a typical workload (<= 64 active partitions) commits
// roughly one OS page for the whole table -- the snmalloc pagemap pattern
// (Liétar et al., ISMM 2019).
struct alignas(64) CoarsePagemap {
  void *coverage_base;   // User-VA base, aligned to `kPartitionBytes`.
  size_t coverage_bytes; // `(max_user_va_aligned_down - coverage_base)`.
  uint8_t _pad[64 - 16]; // Separates the header from `entries` so the first
                         // entry starts a fresh cache line.
  cpp::Atomic<PartitionDescriptor *> entries[kCoarseMaxEntries];
};

// Crystalline-W retire frequency for the partition domain. Half of the buddy
// arena's value because partition retires fire only on empty-transition
// (rare); per-thread retire batches stay small.
constexpr uint32_t kPartitionRetireFreq = 4;

void partition_free_descriptor(PartitionDescriptor *desc);

// MaxIdx = 1: call sites are init_node / retire only -- no protect()/anchor()
// pins from inside this layer.
inline constexpr uint32_t kPartitionMaxIdx = 1;

// Process-wide Crystalline-W (Nikolaev / Ravindran, PLDI 2024) domain. The
// free callback runs only after every concurrent `lookup()` pin holder has
// released.
extern ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    PartitionDescriptor, &partition_free_descriptor, kPartitionRetireFreq,
    kPartitionMaxIdx>
    g_partition_domain;

} // namespace partition
} // namespace alloc
} // namespace windows

namespace concurrent {
// Codec mapping `PartitionDescriptor *` to its pool index for Crystalline-W's
// batch-link encoding. Pool base from sealed Zone 0 (arbitrary-write cannot
// redirect the encode/decode pair); capacity 256 fits in 8 bits, codec
// output `index + 1` is always non-zero. Lives in the header so every
// consumer of `g_partition_domain` sees it at CrystallineDomain instantiation.
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

// Resolve `addr` to its owning partition descriptor, or `nullptr` if outside
// every partition. Wait-free, ~3 ns warm. Pin discipline: callers that
// dereference the returned pointer must pin `g_partition_domain` across the
// dereference -- `lookup()` itself does not pin, and a returned pointer may
// sit in a retire batch whose epoch has not advanced. The fault classifier
// only compares pointers for identity, so it needs no pin.
[[nodiscard]] LIBC_INLINE PartitionDescriptor *lookup(const void *addr) {
  // `cpp::Atomic::load` is non-const, so the pagemap handle is non-const even
  // though this is a read-mostly path. Matches `pagemap_slot_unchecked`.
  auto *coarse = static_cast<CoarsePagemap *>(
      ::LIBC_NAMESPACE::g_pcb.zone0.partition_coarse_pagemap());
  if (LIBC_UNLIKELY(coarse == nullptr))
    return nullptr;
  uintptr_t a = reinterpret_cast<uintptr_t>(addr);
  uintptr_t base = reinterpret_cast<uintptr_t>(coarse->coverage_base);
  if (LIBC_UNLIKELY(a < base || a - base >= coarse->coverage_bytes))
    return nullptr;
  size_t idx = (a - base) >> kPartitionShift;
  // ACQUIRE pairs with the RELEASE publication store in `reserve_or_grow`
  // Phase 3 and the RELEASE clearing store in `try_retire_inline` Phase 3
  // (see partition.cpp); a non-null result is a descriptor whose fields are
  // fully published.
  return coarse->entries[idx].load(cpp::MemoryOrder::ACQUIRE);
}

[[nodiscard]] LIBC_INLINE bool is_managed(const void *addr) {
  return lookup(addr) != nullptr;
}

//===----------------------------------------------------------------------===//
// Public API -- cold paths (reservation, chunk-register/unregister, retire)
//===----------------------------------------------------------------------===//

// Demand-reserve a 4 GiB partition for `(cls, numa_node)`; returns the
// freshly published or peer-published descriptor. Same-`(cls, node)` thunder
// serialises through the primary slot plus linear probing -- only the CAS
// winner's reservation is kept.
//
// On affined NUMA failure the function falls through to a same-class replica:
// any `(cls, *)` peer is strictly better than ENOMEM because cross-node free
// is unchanged and NT first-touch places committed pages on the requesting
// CPU's node regardless of reservation hint. Returns nullptr if VA reserve
// fails, the descriptor pool is exhausted, or the reserve table is full.
[[nodiscard]] PartitionDescriptor *
reserve_or_grow(PartitionClass cls, uint16_t numa_node);

// Counter-bookkeeping and retire-state gate for a fresh chunk commit. Most
// consumers should use `commit_chunk` below; this is the raw entry point for
// callers that want counter bookkeeping without the kernel pair. Returns 0,
// -EINVAL on null descriptor, or -EAGAIN if the partition is mid-retire
// (caller restarts via `reserve_or_grow`).
[[nodiscard]] int commit_chunk_register(PartitionDescriptor *desc,
                                         void *chunk_base,
                                         size_t chunk_bytes);

// Counter-bookkeeping and empty-transition retire trigger. If both counters
// reach zero while `retire_state` is LIVE, the caller's thread inlines the
// retire path. Prefer `decommit_chunk`. Returns post-decrement
// `bytes_committed`.
size_t decommit_chunk_unregister(PartitionDescriptor *desc,
                                  void *chunk_base, size_t chunk_bytes);

// Unified chunk commit: placeholder split + commit-replace + counter
// bookkeeping + pagemap registration + pagemap publish, rolled back in
// reverse order on any step's failure. Caller invariants: `chunk_base` and
// `chunk_bytes` aligned to `kPagemapChunkBytes` (64 KiB); `chunk_base`
// inside `desc`'s middle placeholder; `pagemap_slot_idx` opaque to this
// layer (the consumer decodes it). Returns 0, -EAGAIN if mid-retire, or
// -EIO on syscall failure (VA may sit in an in-between placeholder state).
[[nodiscard]] int commit_chunk(PartitionDescriptor *desc, void *chunk_base,
                                size_t chunk_bytes, DWORD page_prot,
                                ::LIBC_NAMESPACE::windows::alloc::VaChunkConsumer
                                    pagemap_tag,
                                uint32_t pagemap_slot_idx);

// Inverse of `commit_chunk` in reverse-step order. Pagemap retire fires first
// so a concurrent fault classifier sees "not tracked" before pages decommit
// and counters drop. Failures are best-effort: partition stays consistent,
// but affected VA may leak as a placeholder.
void decommit_chunk(PartitionDescriptor *desc, void *chunk_base,
                    size_t chunk_bytes);

// Centralised canary so init, free-callback validation, and fork reinit all
// derive the same value from the same four XOR inputs (see partition.cpp).
[[nodiscard]] uint64_t compute_canary(void *base, uint32_t descriptor_seq);

//===----------------------------------------------------------------------===//
// Tier A bootstrap
//===----------------------------------------------------------------------===//

// Tier A initialiser, registered at memory phase 5. Reserves the coarse
// pagemap, the descriptor pool, and the eager core partitions (each stamped
// PINNED). Fills `out[0..cap)` with substrate-registry receipts that flag
// these reservations LIBC_INTERNAL; returns the count written.
uint32_t partition_init_fn(::LIBC_NAMESPACE::internal::Receipt *out,
                            uint32_t cap);

//===----------------------------------------------------------------------===//
// Fork reinit
//===----------------------------------------------------------------------===//

// Fork-child reinit at `kForkPrioPartition`. Re-rolls `partition_secret` in
// the Zone 0b unseal window held open by `libc_fork_reinit_impl`, then walks
// the reserve table refreshing every descriptor's canary. The init latch is
// reset in case fork happened mid-init.
void partition_fork_reinit();

//===----------------------------------------------------------------------===//
// Diagnostics
//===----------------------------------------------------------------------===//

// Aggregate snapshot of the partition layer's live state. Used by tests.
struct PartitionStats {
  uint32_t total_partitions_live;   // Distinct live partition descriptors.
  uint32_t descriptor_pool_used;    // Occupancy bitmap popcount.
  uint64_t total_va_reserved_bytes; // Sum of `desc->bytes` across live.
  uint64_t total_committed_bytes;   // Sum of `desc->bytes_committed`.
};

[[nodiscard]] PartitionStats partition_stats_snapshot();

} // namespace partition
} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
