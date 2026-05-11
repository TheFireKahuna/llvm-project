//===-- alloc::partition --- implementation ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implementation of the 4 GiB type-isolated VA partition layer. The header
// states the externally-visible contract (state machine, caller invariants,
// pin discipline); this file carries the protocol-step rationale --
// publication ordering between the coarse pagemap and the reserve table,
// the LIVE/IDLE/DRAINING re-arm CAS, the empty-transition retire path, and
// the Crystalline-W (Nikolaev / Ravindran, PLDI 2024) free-callback gate.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/alloc/partition.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/partition_class.h"
#include "src/__support/OSUtil/windows/alloc/primitives/canary_seed.h"
#include "src/__support/OSUtil/windows/alloc/primitives/init_latch.h"
#include "src/__support/OSUtil/windows/alloc/primitives/occupancy_bitmap.h"
#include "src/__support/OSUtil/windows/alloc/sealed_va_publisher.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_primitives_bootstrap.h"
#include "src/__support/OSUtil/windows/nt_pal/pal_state.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
#include "src/__support/OSUtil/windows/nt_pal/protect.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "hdr/errno_macros.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include "hdr/stdint_proxy.h"
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {
namespace partition {

//===----------------------------------------------------------------------===//
// File-scope state
//===----------------------------------------------------------------------===//

namespace {

// Occupancy bitmap for the descriptor pool. AtomicBitmap is zero-init valid;
// the pool's backing VA is sealed in Zone 0 as `partition_desc_pool_base_`.
// Trap-on-collision is enabled because a double-acquire of the same slot
// would be a structural bug: every allocation CAS-takes a fresh bit.
::LIBC_NAMESPACE::internal::alloc_primitives::AtomicBitmap<
    kPartitionDescPoolCapacity, /*trap_on_collision=*/true>
    g_desc_pool_occupancy;

// Per-thread randomized starting word reduces CAS contention on the
// occupancy bitmap when multiple threads allocate descriptors concurrently.
cpp::Atomic<uint32_t> g_desc_pool_hint{0};

// Tier A init latch: `try_begin` in `partition_init_fn`; `publish_ready` at
// the end of the same function; `fork_reinit` in the fork hook.
::LIBC_NAMESPACE::internal::alloc_primitives::InitLatch g_partition_init;

} // namespace

// File-scope reserve table. Sized at `kReserveTableSize` slots of 8 B each;
// at the default capacity this is 1 KiB / 16 cache lines, fully L1-resident.
// The table lives in libc.dll BSS so it CoW-inherits across fork (no special
// fork handling needed beyond the per-descriptor canary refresh). Hot-path
// callers reach it through the sealed Zone 0 pointer
// `g_pcb.zone0.partition_reserve_table()` so an attacker with an arbitrary-
// write primitive cannot redirect reservation deduplication.
ReserveTable g_reserve_table;

// Crystalline-W domain definition (the header declares it extern). Lives at
// `partition` namespace scope so the extern declaration resolves to this
// symbol rather than to a separate anonymous-namespace symbol.
::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    PartitionDescriptor, &partition_free_descriptor, kPartitionRetireFreq>
    g_partition_domain;

//===----------------------------------------------------------------------===//
// Descriptor pool
//===----------------------------------------------------------------------===//
//
// The descriptor pool VA is sealed in Zone 0, so an arbitrary-write primitive
// cannot redirect allocation. The pool storage is never released for the
// process lifetime: a Crystalline-W retire batch may legitimately carry a
// stale slot index that re-resolves to a fresh allocation through the same
// slot. Per-descriptor identity is re-established through the
// `descriptor_seq` bump on every allocation, not through storage churn.

[[nodiscard]] PartitionDescriptor *desc_pool_alloc() {
  auto *base = static_cast<PartitionDescriptor *>(
      ::LIBC_NAMESPACE::g_pcb.zone0.partition_desc_pool_base());
  if (LIBC_UNLIKELY(base == nullptr))
    return nullptr;

  uint32_t start_word =
      g_desc_pool_hint.fetch_add(1, cpp::MemoryOrder::RELAXED);

  using Bitmap = decltype(g_desc_pool_occupancy);
  constexpr size_t WORDS = Bitmap::word_count;

  for (size_t attempt = 0; attempt < WORDS; ++attempt) {
    size_t w = (start_word + attempt) % WORDS;
    uint64_t bits = ~g_desc_pool_occupancy
                         .word_at<cpp::MemoryOrder::RELAXED>(w);
    while (bits) {
      // `bits` is non-zero by the loop guard, so ctzll's result is defined.
      unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
      size_t idx = w * 64u + bit;
      if (LIBC_UNLIKELY(idx >= kPartitionDescPoolCapacity)) {
        bits &= bits - 1;
        continue;
      }
      if (g_desc_pool_occupancy.try_acquire(idx))
        return &base[idx];
      // Lost the bit race to another acquirer; clear it from our local
      // scratch word and try the next one.
      bits &= bits - 1;
    }
  }
  return nullptr;
}

void desc_pool_free(PartitionDescriptor *desc) {
  if (LIBC_UNLIKELY(desc == nullptr))
    __builtin_trap();
  auto *base = static_cast<PartitionDescriptor *>(
      ::LIBC_NAMESPACE::g_pcb.zone0.partition_desc_pool_base());
  size_t idx = static_cast<size_t>(desc - base);
  if (LIBC_UNLIKELY(idx >= kPartitionDescPoolCapacity))
    __builtin_trap();

  // Wipe the CrystallineNode header and descriptor body so a stale reader
  // sees a fresh tombstone rather than the previous owner's data. The
  // CrystallineNode header in particular must be zeroed: `next` /
  // `batch_link` / `refs` must be back to fresh state for the next
  // allocation through this slot to behave correctly as a domain node.
  desc->cn_next.store(nullptr, cpp::MemoryOrder::RELAXED);
  desc->batch_link.store(0u, cpp::MemoryOrder::RELAXED);
  desc->refs.store(0, cpp::MemoryOrder::RELAXED);
  desc->base = nullptr;
  desc->mask = 0;
  desc->bytes = 0;
  desc->key = 0;
  desc->descriptor_seq = 0;
  desc->bytes_committed.store(0, cpp::MemoryOrder::RELAXED);
  desc->active_chunks.store(0, cpp::MemoryOrder::RELAXED);
  desc->retire_state.store(kStateLive, cpp::MemoryOrder::RELAXED);
  desc->leading_guard = nullptr;
  desc->trailing_guard = nullptr;
  desc->canary = 0;

  g_desc_pool_occupancy.clear(idx);
}

//===----------------------------------------------------------------------===//
// Canary computation
//===----------------------------------------------------------------------===//
//
//   canary = process_cookie ^ partition_secret ^ uintptr_t(base)
//                                              ^ descriptor_seq
//
// Inputs:
//   - process_cookie:   kernel-supplied, re-probed by `pal_fork_reinit`
//                       after fork. Lives in PCB Zone 0b, sealed
//                       PAGE_READONLY at runtime so an arbitrary-write
//                       primitive cannot poison this XOR input.
//   - partition_secret: ProcessPrng-derived; also in Zone 0b. Re-rolled
//                       on fork via `partition_fork_reinit` inside the
//                       Zone 0b unseal window.
//   - base:             descriptor's partition VA; immutable after publish.
//   - descriptor_seq:   per-descriptor random seq from ProcessPrng at
//                       init; immutable for the descriptor's pool tenancy.
//
// Tampering with any input produces a canary mismatch that the free
// callback traps on. `process_cookie` and `partition_secret` rotate on
// fork, so a parent's canary cannot be replayed against a child.

uint64_t compute_canary(void *base, uint32_t descriptor_seq) {
  uint64_t cookie =
      static_cast<uint64_t>(::LIBC_NAMESPACE::g_pcb.zone0b.process_cookie());
  uint64_t secret = ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
  uint64_t base_bits = reinterpret_cast<uintptr_t>(base);
  uint64_t seq = static_cast<uint64_t>(descriptor_seq);
  return cookie ^ secret ^ base_bits ^ seq;
}

//===----------------------------------------------------------------------===//
// Guard pages
//===----------------------------------------------------------------------===//
//
// Each partition is bracketed by `kPartitionGuardBytes` PAGE_NOACCESS guards
// at head and tail, implemented by splitting the partition's parent
// placeholder into three sub-placeholders:
//
//   [ leading guard ] [ middle ] [ trailing guard ]
//   ^ base            ^ +guard   ^ +bytes-guard
//
// A placeholder reservation is `MEM_RESERVE | MEM_RESERVE_PLACEHOLDER` with
// PAGE_NOACCESS protection: no PTE backing, so any access faults. The split
// gives PAGE_NOACCESS guards for free with zero commit charge.
//
// Chunk owners commit chunks within the *middle* placeholder via
// `commit_replace`, which itself splits the middle further as chunks are
// committed and decommitted.
//
// Release contract: when the free callback runs (after empty-transition
// retire and Crystalline-W epoch advance), chunk owners must have called
// `decommit_preserve` -- not bare `MEM_DECOMMIT` -- so every sub-range
// across `[base, base + bytes)` is a placeholder. `coalesce_placeholders`
// then unifies the three (or more, after chunk fragmentation) placeholders
// back into a single 4 GiB placeholder before `free_placeholder`. A
// coalesce failure is a chunk-owner contract violation; trap so the leak
// is loud.

namespace {

[[nodiscard]] bool install_partition_guards(void *base, size_t bytes) {
  if (LIBC_UNLIKELY(bytes < 2 * kPartitionGuardBytes))
    return false;

  // Split off the leading guard: parent placeholder `[base, base + bytes)`
  // becomes `[base, base + kPartitionGuardBytes)` (leading guard) plus
  // `[base + kPartitionGuardBytes, base + bytes)` (rest).
  if (!::LIBC_NAMESPACE::nt_pal::split_placeholder(base, kPartitionGuardBytes))
    return false;

  // Split the trailing guard off the rest. The rest begins at
  // `base + kPartitionGuardBytes`; cutting at offset `bytes - 2 * guard`
  // yields a middle `[base + guard, base + bytes - guard)` and a trailing
  // `[base + bytes - guard, base + bytes)`.
  void *middle = static_cast<char *>(base) + kPartitionGuardBytes;
  size_t middle_size = bytes - 2 * kPartitionGuardBytes;
  if (!::LIBC_NAMESPACE::nt_pal::split_placeholder(middle, middle_size)) {
    // Roll back the leading split. Both halves are placeholders, so a
    // coalesce across `[base, base + bytes)` re-unifies them. A failure
    // here is unrecoverable (kernel state diverged from our model): trap
    // so the leak is loud rather than silent.
    if (!NT_SUCCESS(::LIBC_NAMESPACE::nt_pal::coalesce_placeholders(
            base, bytes)))
      __builtin_trap();
    return false;
  }
  return true;
}

// Coalesces the partition's placeholder fragments back into a single span
// ahead of `free_placeholder`. Precondition: `bytes_committed == 0` and
// every chunk owner decommitted via `decommit_preserve` (so the entire
// span is in placeholder state). Trap on failure -- that's a chunk-owner
// contract violation, not a recoverable condition here.
void release_partition_guards(void *base, size_t bytes) {
  if (base == nullptr || bytes < 2 * kPartitionGuardBytes)
    return;
  if (!NT_SUCCESS(::LIBC_NAMESPACE::nt_pal::coalesce_placeholders(base,
                                                                    bytes)))
    __builtin_trap();
}

void init_descriptor(PartitionDescriptor *desc, void *base, size_t bytes,
                     PartitionClass cls, uint16_t numa_node,
                     uint32_t descriptor_seq) {
  desc->base = base;
  desc->mask = ~(bytes - 1);
  desc->bytes = bytes;
  desc->key = pack_partition_key(cls, numa_node);
  desc->descriptor_seq = descriptor_seq;
  desc->bytes_committed.store(0, cpp::MemoryOrder::RELAXED);
  desc->active_chunks.store(0, cpp::MemoryOrder::RELAXED);
  desc->retire_state.store(kStateLive, cpp::MemoryOrder::RELAXED);
  desc->leading_guard = base;
  desc->trailing_guard = static_cast<char *>(base) + bytes -
                          kPartitionGuardBytes;
  desc->canary = compute_canary(base, descriptor_seq);
}

[[nodiscard]] uint32_t draw_descriptor_seq() {
  ::LIBC_NAMESPACE::internal::alloc_primitives::SingleCanarySeed seed{};
  ::LIBC_NAMESPACE::internal::alloc_primitives::init_seed_or_trap(seed);
  // The seq is only one component of the canary; the other components
  // carry the full 64 bits of entropy from `process_cookie` and
  // `partition_secret`. Truncating here is acceptable.
  return static_cast<uint32_t>(seed.seed);
}

} // namespace

//===----------------------------------------------------------------------===//
// Coarse pagemap accessors
//===----------------------------------------------------------------------===//

namespace {

[[nodiscard]] CoarsePagemap *mutable_coarse_pagemap() {
  return static_cast<CoarsePagemap *>(
      ::LIBC_NAMESPACE::g_pcb.zone0.partition_coarse_pagemap());
}

[[nodiscard]] ReserveTable *mutable_reserve_table() {
  return static_cast<ReserveTable *>(
      ::LIBC_NAMESPACE::g_pcb.zone0.partition_reserve_table());
}

[[nodiscard]] size_t coarse_index_of(void *partition_base) {
  CoarsePagemap *coarse = mutable_coarse_pagemap();
  uintptr_t a = reinterpret_cast<uintptr_t>(partition_base);
  uintptr_t b = reinterpret_cast<uintptr_t>(coarse->coverage_base);
  return (a - b) >> kPartitionShift;
}

} // namespace

//===----------------------------------------------------------------------===//
// reserve_or_grow
//===----------------------------------------------------------------------===//

PartitionDescriptor *reserve_or_grow(PartitionClass cls, uint16_t numa_node) {
  ReserveTable *table = mutable_reserve_table();
  if (LIBC_UNLIKELY(table == nullptr))
    return nullptr;
  uint32_t key = pack_partition_key(cls, numa_node);
  uint32_t primary = primary_slot(key);

  // Phase 1: probe for an existing entry. Wait-free; ACQUIRE pairs with the
  // RELEASE CAS in Phase 4 so any non-null descriptor we observe is fully
  // initialised. A null slot terminates the probe -- linear probing with
  // open addressing means the cluster ends at the first null.
  for (uint32_t i = 0; i < kReserveTableSize; ++i) {
    uint32_t slot = (primary + i) & kReserveTableMask;
    PartitionDescriptor *existing =
        table->slots[slot].descriptor.load(cpp::MemoryOrder::ACQUIRE);
    if (existing == nullptr)
      break;
    if (existing->key == key)
      return existing;
  }

  // Phase 2: optimistic syscall. No sentinel claim is held on the table
  // across the syscall, so a thread that re-enters the partition layer
  // (e.g. via a fault during reservation) cannot self-deadlock on its own
  // claim. The reservation is forced to `kPartitionBytes` alignment so each
  // partition occupies exactly one coarse-pagemap entry. For
  // `kNodeAgnostic` the NumaNode extended parameter is omitted because NT
  // rejects `(ULONG)-1` as out-of-range and returns STATUS_UNSUCCESSFUL.
  //
  // `reservation.status` is captured but not branched on: every failure
  // mode routes through the same Phase 2.5 fallback because the recovery
  // strategy ("any same-class replica") does not depend on *why* the
  // affined reserve failed. The status sits in the returned struct for
  // free; future diagnostic wiring can read it without re-syscalling.
  ::LIBC_NAMESPACE::nt_pal::PlaceholderReservation reservation =
      (numa_node == kNodeAgnostic)
          ? ::LIBC_NAMESPACE::nt_pal::reserve_placeholder_aligned(
                kPartitionBytes, kPartitionBytes)
          : ::LIBC_NAMESPACE::nt_pal::reserve_placeholder_numa_aligned(
                kPartitionBytes, kPartitionBytes,
                static_cast<ULONG>(numa_node));
  void *new_base = reservation.base;
  if (new_base == nullptr) {
    // Phase 2.5: same-class replica fallback. On `kNodeAgnostic` failure
    // there is nothing better to fall back to -- that path was already the
    // kernel-chosen placement. On affined failure a previously-published
    // peer replica `(cls, *)` is a strictly better answer than ENOMEM:
    // cross-node free is unaffected and NT first-touch still places
    // committed pages on the requesting CPU's node regardless of the
    // partition's reservation hint.
    //
    // The scan is read-only and safe without a Crystalline-W pin by the
    // same argument as Phase 1: a descriptor reused for a different
    // `(cls, node)` after retire has its `key` field rewritten in
    // `desc_pool_free` (zeroed) before re-publication, so a stale read
    // either matches the current owner's class (correct fallback) or does
    // not (skipped).
    if (numa_node != kNodeAgnostic) {
      for (uint32_t i = 0; i < kReserveTableSize; ++i) {
        PartitionDescriptor *cand =
            table->slots[i].descriptor.load(cpp::MemoryOrder::ACQUIRE);
        if (cand != nullptr && unpack_class(cand->key) == cls)
          return cand;
      }
    }
    return nullptr;
  }

  if (!install_partition_guards(new_base, kPartitionBytes)) {
    (void)::LIBC_NAMESPACE::nt_pal::free_placeholder(new_base);
    return nullptr;
  }

  PartitionDescriptor *desc = desc_pool_alloc();
  if (desc == nullptr) {
    // No descriptor pool slot available. Coalesce the guard splits back
    // into a single placeholder before `free_placeholder` (which requires
    // the entire span be one placeholder).
    release_partition_guards(new_base, kPartitionBytes);
    (void)::LIBC_NAMESPACE::nt_pal::free_placeholder(new_base);
    return nullptr;
  }

  uint32_t seq = draw_descriptor_seq();
  init_descriptor(desc, new_base, kPartitionBytes, cls, numa_node, seq);
  g_partition_domain.init_node(desc);

  // Phase 3: publish the coarse pagemap entry first. If the publishing
  // thread dies between this store and the Phase 4 reserve-slot CAS, the
  // orphan partition is still lookup-able -- the fault classifier
  // correctly skips its VA. The orphan VA leaks for process lifetime (no
  // event triggers retire for an unreachable partition), but correctness
  // is preserved. The RELEASE store synchronizes-with every subsequent
  // ACQUIRE load in `lookup()`.
  CoarsePagemap *coarse = mutable_coarse_pagemap();
  size_t coarse_idx = coarse_index_of(new_base);
  if (LIBC_UNLIKELY(coarse_idx >= kCoarseMaxEntries)) {
    // Partition VA fell outside the coarse pagemap's coverage. Should not
    // happen on a healthy system: `nt_pal::reserve_placeholder_numa`
    // returns user-mode VA and `coverage_bytes` spans the full user-VA
    // range. Abort cleanly rather than corrupt state.
    desc_pool_free(desc);
    release_partition_guards(new_base, kPartitionBytes);
    (void)::LIBC_NAMESPACE::nt_pal::free_placeholder(new_base);
    return nullptr;
  }
  coarse->entries[coarse_idx].store(desc, cpp::MemoryOrder::RELEASE);

  // Phase 4: CAS-publish into the reserve table. Loser observes the peer's
  // descriptor and rolls back. Bounded retries (at most `kReserveTableSize`)
  // because the table is open-addressed and primary-slot probing is linear.
  for (uint32_t i = 0; i < kReserveTableSize; ++i) {
    uint32_t slot = (primary + i) & kReserveTableMask;
    PartitionDescriptor *expected = nullptr;
    // ACQ_REL on success: releases the descriptor we wrote in
    // `init_descriptor` to any peer that subsequently observes this slot,
    // and acquires for cross-thread ordering even though `expected` was
    // nullptr. The ACQUIRE failure ordering pairs with a winning peer's
    // RELEASE.
    if (table->slots[slot].descriptor.compare_exchange_strong(
            expected, desc, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE)) {
      return desc;
    }
    // CAS lost: `expected` now holds the peer's descriptor.
    if (expected != nullptr && expected->key == key) {
      // Peer published the same `(cls, node)`. Roll back our publish and
      // adopt the peer's descriptor.
      coarse->entries[coarse_idx].store(nullptr, cpp::MemoryOrder::RELEASE);
      desc_pool_free(desc);
      release_partition_guards(new_base, kPartitionBytes);
      (void)::LIBC_NAMESPACE::nt_pal::free_placeholder(new_base);
      return expected;
    }
    // Wrong key in slot; continue probing.
  }

  // Reserve table exhausted: roll back coarse pagemap and VA.
  coarse->entries[coarse_idx].store(nullptr, cpp::MemoryOrder::RELEASE);
  desc_pool_free(desc);
  release_partition_guards(new_base, kPartitionBytes);
  (void)::LIBC_NAMESPACE::nt_pal::free_placeholder(new_base);
  return nullptr;
}

//===----------------------------------------------------------------------===//
// commit_chunk_register
//===----------------------------------------------------------------------===//

int commit_chunk_register(PartitionDescriptor *desc, void * /*chunk_base*/,
                           size_t chunk_bytes) {
  if (LIBC_UNLIKELY(desc == nullptr))
    return -EINVAL;

  // State gate. Loop handles IDLE -> LIVE re-arm CAS contention.
  for (;;) {
    uint32_t state = desc->retire_state.load(cpp::MemoryOrder::ACQUIRE);
    if (state == kStateLive || state == kStatePinned)
      break;
    if (state == kStateIdle) {
      // Re-arm IDLE -> LIVE via CAS. If we win, the empty-transition
      // retire path has not yet acquired the LIVE -> DRAINING CAS for
      // this descriptor (the retire path requires LIVE, not IDLE; if it
      // had observed IDLE, it would not have started a retire). Loser
      // retries.
      uint32_t expected = kStateIdle;
      if (desc->retire_state.compare_exchange_strong(
              expected, kStateLive, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::ACQUIRE))
        break;
      continue;
    }
    // DRAINING / RETIRED: caller must retry via `reserve_or_grow`.
    return -EAGAIN;
  }

  // ACQ_REL on both counters orders the increments with the state load
  // above (so a peer observing kStateLive sees the freshly-incremented
  // counters) and with the matching fetch_sub in
  // `decommit_chunk_unregister` (so the empty-transition observer reads a
  // consistent counter pair).
  desc->bytes_committed.fetch_add(chunk_bytes, cpp::MemoryOrder::ACQ_REL);
  desc->active_chunks.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  return 0;
}

//===----------------------------------------------------------------------===//
// try_retire_inline -- empty-transition retire trigger
//===----------------------------------------------------------------------===//

namespace {

void try_retire_inline(PartitionDescriptor *desc) {
  // Phase 1: CAS LIVE -> DRAINING. PINNED partitions never retire (the CAS
  // requires LIVE). A concurrent commit may have flipped IDLE -> LIVE
  // before our observation; if so, the peer thread will increment counters
  // and the Phase 2 re-verify will catch it.
  uint32_t live = kStateLive;
  if (!desc->retire_state.compare_exchange_strong(
          live, kStateDraining, cpp::MemoryOrder::ACQ_REL,
          cpp::MemoryOrder::RELAXED))
    return;

  // Phase 2: re-verify counters. Race window between our empty observation
  // in `decommit_chunk_unregister` (counters == 0) and the DRAINING flip
  // here: a concurrent `commit_chunk_register` may have observed LIVE and
  // incremented counters. Roll DRAINING -> LIVE if so.
  if (desc->bytes_committed.load(cpp::MemoryOrder::ACQUIRE) != 0 ||
      desc->active_chunks.load(cpp::MemoryOrder::ACQUIRE) != 0) {
    desc->retire_state.store(kStateLive, cpp::MemoryOrder::RELEASE);
    return;
  }

  // Phase 3: clear the coarse pagemap entry. Subsequent `lookup()` calls
  // miss this partition's VA -- correct, since the re-verify proved no
  // live chunks exist on it.
  CoarsePagemap *coarse = mutable_coarse_pagemap();
  size_t coarse_idx = coarse_index_of(desc->base);
  coarse->entries[coarse_idx].store(nullptr, cpp::MemoryOrder::RELEASE);

  // Phase 4: clear the reserve slot. Subsequent `reserve_or_grow` for the
  // same `(cls, node)` allocates a fresh descriptor.
  ReserveTable *table = mutable_reserve_table();
  uint32_t primary = primary_slot(desc->key);
  for (uint32_t i = 0; i < kReserveTableSize; ++i) {
    uint32_t slot = (primary + i) & kReserveTableMask;
    PartitionDescriptor *expected = desc;
    if (table->slots[slot].descriptor.compare_exchange_strong(
            expected, static_cast<PartitionDescriptor *>(nullptr),
            cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::RELAXED))
      break;
  }

  // Phase 5: hand to Crystalline-W. The free callback runs only after
  // every concurrent `lookup()` pin holder has released, so a wait-free
  // reader can never observe a freed descriptor.
  desc->retire_state.store(kStateRetired, cpp::MemoryOrder::RELEASE);
  g_partition_domain.retire(desc);
}

} // namespace

//===----------------------------------------------------------------------===//
// decommit_chunk_unregister
//===----------------------------------------------------------------------===//

size_t decommit_chunk_unregister(PartitionDescriptor *desc,
                                  void * /*chunk_base*/, size_t chunk_bytes) {
  if (LIBC_UNLIKELY(desc == nullptr))
    return 0;

  // ACQ_REL on the counter pair pairs with the matching fetch_add in
  // `commit_chunk_register`; this fence-equivalent is what lets the
  // post-decrement check see a consistent pair.
  uint64_t prev_bytes = desc->bytes_committed.fetch_sub(
      chunk_bytes, cpp::MemoryOrder::ACQ_REL);
  uint64_t after_bytes = prev_bytes - chunk_bytes;
  uint32_t prev_chunks =
      desc->active_chunks.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
  uint32_t after_chunks = prev_chunks - 1;

  if (after_bytes == 0 && after_chunks == 0) {
    uint32_t state = desc->retire_state.load(cpp::MemoryOrder::ACQUIRE);
    if (state == kStateLive)
      try_retire_inline(desc);
  }
  return static_cast<size_t>(after_bytes);
}

//===----------------------------------------------------------------------===//
// commit_chunk / decommit_chunk
//===----------------------------------------------------------------------===//

int commit_chunk(PartitionDescriptor *desc, void *chunk_base,
                 size_t chunk_bytes, DWORD page_prot,
                 alloc::VaChunkConsumer pagemap_tag,
                 uint32_t pagemap_slot_idx) {
  namespace nt_pal = ::LIBC_NAMESPACE::nt_pal;

  if (LIBC_UNLIKELY(desc == nullptr || chunk_base == nullptr ||
                     chunk_bytes == 0))
    return -EINVAL;
  // Pagemap stamps at `kPagemapChunkBytes` (64 KiB) granularity. The chunk
  // owner must have picked a chunk geometry that satisfies this.
  LIBC_ASSERT((chunk_bytes & (alloc::kPagemapChunkBytes - 1)) == 0 &&
              "commit_chunk: chunk_bytes must be 64 KiB-aligned");
  LIBC_ASSERT((reinterpret_cast<uintptr_t>(chunk_base) &
                (alloc::kPagemapChunkBytes - 1)) == 0 &&
              "commit_chunk: chunk_base must be 64 KiB-aligned");

  // Step 1: split a chunk-sized hole out of the partition's middle
  // placeholder. The kernel serialises racing splits on the same range --
  // only one thread succeeds; the rest get STATUS_CONFLICTING_ADDRESSES.
  if (!nt_pal::split_placeholder(chunk_base, chunk_bytes))
    return -EIO;

  // Step 2: commit pages, replacing the placeholder. Always passes
  // MEM_WRITE_WATCH so the hardware dirty bitmap is armed for fork CoW
  // preservation, quarantine sweep, and telemetry.
  NTSTATUS st = nt_pal::commit_replace(chunk_base, chunk_bytes, page_prot);
  if (!NT_SUCCESS(st)) {
    // Rollback step 1: best-effort coalesce. If coalesce fails the
    // placeholder leaks but the partition stays consistent (no committed
    // pages, no counter increment).
    (void)nt_pal::coalesce_placeholders(chunk_base, chunk_bytes);
    return -EIO;
  }

  // Step 3: partition-counter bookkeeping plus retire-state gate.
  int rc = commit_chunk_register(desc, chunk_base, chunk_bytes);
  if (rc != 0) {
    (void)nt_pal::decommit_preserve(chunk_base, chunk_bytes);
    (void)nt_pal::coalesce_placeholders(chunk_base, chunk_bytes);
    return rc;
  }

  // Step 4: upgrade the pagemap OS pages covering this chunk to
  // PAGE_READWRITE. Idempotent across overlapping ranges.
  int reg_rc = alloc::pagemap_register_range(chunk_base, chunk_bytes);
  if (reg_rc != 0) {
    (void)decommit_chunk_unregister(desc, chunk_base, chunk_bytes);
    (void)nt_pal::decommit_preserve(chunk_base, chunk_bytes);
    (void)nt_pal::coalesce_placeholders(chunk_base, chunk_bytes);
    return reg_rc;
  }

  // Step 5: publish the `(slot_idx, tag)` entry across every
  // `kPagemapChunkBytes` sub-chunk via the bulk-publish helper.
  alloc::pagemap_publish_range(chunk_base, chunk_bytes, pagemap_slot_idx,
                                pagemap_tag);
  return 0;
}

void decommit_chunk(PartitionDescriptor *desc, void *chunk_base,
                    size_t chunk_bytes) {
  namespace nt_pal = ::LIBC_NAMESPACE::nt_pal;

  if (LIBC_UNLIKELY(desc == nullptr || chunk_base == nullptr ||
                     chunk_bytes == 0))
    return;

  // Pagemap retire fires first so a concurrent fault classifier observes
  // "not tracked" before pages decommit and partition counters drop. The
  // reverse order would leave a window where a concurrent classifier sees
  // a still-tagged pagemap entry pointing at decommitted backing.
  alloc::pagemap_retire_range(chunk_base, chunk_bytes);
  (void)nt_pal::decommit_preserve(chunk_base, chunk_bytes);
  (void)decommit_chunk_unregister(desc, chunk_base, chunk_bytes);
}

//===----------------------------------------------------------------------===//
// partition_free_descriptor -- Crystalline-W free callback
//===----------------------------------------------------------------------===//

void partition_free_descriptor(PartitionDescriptor *desc) {
  if (LIBC_UNLIKELY(desc == nullptr))
    __builtin_trap();

  // Canary mismatch is tamper detection: one of the canary inputs
  // (`process_cookie`, `partition_secret`, `base`, `descriptor_seq`) has
  // been corrupted between init and reclamation.
  uint64_t expected_canary = compute_canary(desc->base, desc->descriptor_seq);
  if (LIBC_UNLIKELY(desc->canary != expected_canary))
    __builtin_trap();

  // A non-zero counter at the free-callback point means a chunk is
  // committed against a retiring partition -- a state-machine violation
  // that must be loud, not silent.
  if (LIBC_UNLIKELY(
          desc->bytes_committed.load(cpp::MemoryOrder::ACQUIRE) != 0 ||
          desc->active_chunks.load(cpp::MemoryOrder::ACQUIRE) != 0))
    __builtin_trap();

  // Coalesce the leading guard + middle + trailing guard placeholders back
  // into a single 4 GiB placeholder ahead of the placeholder release.
  // Precondition: chunk owners decommitted via `decommit_preserve` (not
  // bare `MEM_DECOMMIT`) so the entire span `[base, base + bytes)` is
  // placeholders. `release_partition_guards` traps on coalesce failure
  // (chunk-owner contract violation).
  void *base = desc->base;
  size_t bytes = desc->bytes;
  release_partition_guards(base, bytes);

  // The MEM_RELEASE returning the 4 GiB VA to the OS happens here. This
  // is the linearization point for "this partition's VA is reusable".
  if (!::LIBC_NAMESPACE::nt_pal::free_placeholder(base))
    __builtin_trap();

  desc_pool_free(desc);
}

//===----------------------------------------------------------------------===//
// partition_init_fn -- Tier A bootstrap
//===----------------------------------------------------------------------===//

uint32_t partition_init_fn(::LIBC_NAMESPACE::internal::Receipt *out,
                            uint32_t cap) {
  // Tier A is single-threaded; a second entry would be a structural bug.
  if (!g_partition_init.try_begin())
    __builtin_trap();

  // (1) Seed `partition_secret` via ProcessPrng. Fail-closed on PRNG
  // unavailability -- the canary needs entropy from a CSPRNG and there is
  // no acceptable fallback.
  ::LIBC_NAMESPACE::internal::alloc_primitives::SingleCanarySeed seed{};
  ::LIBC_NAMESPACE::internal::alloc_primitives::init_seed_or_trap(seed);
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_partition_secret(seed.seed);

  // (2) Reserve coarse pagemap backing. `kCoarseBytes`; the OS commits
  // pages lazily on first store. Plain `reserve_placeholder` plus
  // `commit_replace` is used (rather than going through a higher-level
  // allocator) so the header fields can be written from this function's
  // stack via the returned VA.
  void *coarse_va =
      ::LIBC_NAMESPACE::nt_pal::reserve_placeholder(kCoarseBytes);
  if (coarse_va == nullptr)
    __builtin_trap();
  NTSTATUS st1 = ::LIBC_NAMESPACE::nt_pal::commit_replace(
      coarse_va, kCoarseBytes, PAGE_READWRITE);
  if (!NT_SUCCESS(st1))
    __builtin_trap();
  auto *coarse = static_cast<CoarsePagemap *>(coarse_va);
  // Coverage spans from `min_address` rounded up to partition stride to
  // `max_address` rounded down. `reserve_placeholder_numa` is trusted to
  // return VA inside this range.
  uintptr_t min_va = reinterpret_cast<uintptr_t>(
      ::LIBC_NAMESPACE::g_pcb.zone0.min_address());
  uintptr_t max_va = reinterpret_cast<uintptr_t>(
      ::LIBC_NAMESPACE::g_pcb.zone0.max_address());
  uintptr_t coverage_base_addr =
      (min_va + kPartitionBytes - 1) & ~(kPartitionBytes - 1);
  uintptr_t coverage_end_addr = max_va & ~(kPartitionBytes - 1);
  coarse->coverage_base = reinterpret_cast<void *>(coverage_base_addr);
  coarse->coverage_bytes = (coverage_end_addr > coverage_base_addr)
                                ? (coverage_end_addr - coverage_base_addr)
                                : 0;
  size_t covered_entries = coarse->coverage_bytes >> kPartitionShift;
  if (covered_entries > kCoarseMaxEntries)
    __builtin_trap();
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_partition_coarse_pagemap(
      coarse_va);
  publish_sealed_va_range(SealedKind::PartitionCoarsePagemap, coarse_va,
                           kCoarseBytes);

  // (3) Reserve and commit the descriptor pool. `kPartitionDescPoolCapacity`
  // descriptors at 128 B each, rounded up to NT's 64 KiB allocation
  // granularity so the placeholder size matches what `commit_replace`
  // (MEM_REPLACE_PLACEHOLDER) sees.
  size_t pool_bytes =
      (kPartitionDescPoolCapacity * sizeof(PartitionDescriptor) + 0xFFFF) &
      ~static_cast<size_t>(0xFFFF);
  void *pool_va =
      ::LIBC_NAMESPACE::nt_pal::reserve_placeholder(pool_bytes);
  if (pool_va == nullptr)
    __builtin_trap();
  NTSTATUS st2 = ::LIBC_NAMESPACE::nt_pal::commit_replace(
      pool_va, pool_bytes, PAGE_READWRITE);
  if (!NT_SUCCESS(st2))
    __builtin_trap();
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_partition_desc_pool_base(
      pool_va);
  publish_sealed_va_range(SealedKind::PartitionDescPool, pool_va, pool_bytes);

  // (4) Stamp the Zone 0 reserve-table pointer. The table itself lives at
  // file scope as `g_reserve_table` and is CoW-shared via libc.dll BSS.
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_partition_reserve_table(
      &g_reserve_table);

  // (5) Initialise the Crystalline-W domain.
  g_partition_domain.init_registration();

  g_partition_init.publish_ready();

  // (6) Eager-reserve the core libc-internal partitions on `kNodeAgnostic`.
  // Each gets stamped PINNED so the empty-transition retire path cannot
  // reap them: these are guaranteed-needed for process lifetime.
  static constexpr PartitionClass kCorePartitions[] = {
      // Core libc-internal partitions.
      PartitionClass::ThreadScratchPool, PartitionClass::FdTable,
      PartitionClass::IoringStorage,
      PartitionClass::SkiplistNode,
      PartitionClass::ArtNode,
      PartitionClass::RegionDesc,
      PartitionClass::OpenFileDescription, PartitionClass::AioCb,
      PartitionClass::TimerNode,         PartitionClass::ChildEntry,
      PartitionClass::EpollNode,         PartitionClass::PkeyRange,
      // Fault-classifier (va_tracker) partitions.
      PartitionClass::VaTrackerSkiplist1_2,
      PartitionClass::VaTrackerSkiplist3_4,
      PartitionClass::VaTrackerSkiplist5_8,
      PartitionClass::VaTrackerSkiplist9_16,
      PartitionClass::VaTrackerArtNode4,
      PartitionClass::VaTrackerArtNode16,
      PartitionClass::VaTrackerArtNode48,
      PartitionClass::VaTrackerArtNode256,
      PartitionClass::VaTrackerRegionDesc,
      PartitionClass::VaTrackerArena,
  };

  uint32_t emitted = 0;
  // Emit receipts for the coarse pagemap reservation, the descriptor pool
  // reservation, and each core partition. The substrate registry stamps
  // these as LIBC_INTERNAL so the va_inventory classifier skips them.
  if (cap > emitted) {
    out[emitted] = {coarse_va, kCoarseBytes,
                    ::LIBC_NAMESPACE::internal::InternalKind::Partition};
    ++emitted;
  }
  if (cap > emitted) {
    out[emitted] = {pool_va, pool_bytes,
                    ::LIBC_NAMESPACE::internal::InternalKind::Partition};
    ++emitted;
  }

  for (PartitionClass cls : kCorePartitions) {
    PartitionDescriptor *desc = reserve_or_grow(cls, kNodeAgnostic);
    if (desc == nullptr) {
      // Tier A failure on a core partition: libc cannot bring up. Falling
      // back to a smaller reservation would break the 32-bit compact-
      // pointer contract for slab metadata.
      __builtin_trap();
    }
    // Route the fresh 4 GiB reservation through the sealed-VA publisher.
    // The publisher validates disjointness against every previously-
    // published sealed range (pagemap, buddy partition + tree + desc pool,
    // partition coarse pagemap + desc pool, earlier partitions in this
    // loop). Trap on overlap or sub-1 MiB gap.
    publish_sealed_va_range(SealedKind::Partition, desc->base, desc->bytes);

    // Stamp PINNED. Plain store because Tier A is single-threaded.
    desc->retire_state.store(kStatePinned, cpp::MemoryOrder::RELEASE);

    if (cap > emitted) {
      out[emitted] = {desc->base, desc->bytes,
                      ::LIBC_NAMESPACE::internal::InternalKind::Partition};
      ++emitted;
    }
  }
  return emitted;
}

//===----------------------------------------------------------------------===//
// partition_fork_reinit
//===----------------------------------------------------------------------===//

void partition_fork_reinit() {
  // Most partition state is structurally valid in the child: the coarse
  // pagemap, the reserve table, the descriptor pool, and every partition's
  // VA are CoW-inherited from the parent. The Crystalline-W
  // `g_partition_domain` is reset by `crystalline_fork_reinit_all` at an
  // earlier priority (eras zeroed, retire batches discarded).
  //
  // What must be re-derived: every descriptor canary, because the
  // `process_cookie` has just been re-probed by `pal_fork_reinit` and
  // `partition_secret` is re-rolled below. Both inputs live in Zone 0b,
  // which `libc_fork_reinit_impl` holds unsealed across this priority
  // band; the `set_partition_secret` write below is therefore a hardware-
  // legal write into the otherwise-sealed page.
  //
  // What must be reset: the init latch, in case fork happened mid-init.

  g_partition_init.fork_reinit();

  // Re-roll `partition_secret` in the Zone 0b unseal window.
  ::LIBC_NAMESPACE::internal::alloc_primitives::SingleCanarySeed seed{};
  ::LIBC_NAMESPACE::internal::alloc_primitives::init_seed_or_trap(seed);
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_partition_secret(seed.seed);

  // Recompute every live descriptor's canary against the new
  // `process_cookie` and new `partition_secret`.
  ReserveTable *table = mutable_reserve_table();
  if (table == nullptr)
    return;
  for (uint32_t i = 0; i < kReserveTableSize; ++i) {
    PartitionDescriptor *desc =
        table->slots[i].descriptor.load(cpp::MemoryOrder::RELAXED);
    if (desc == nullptr)
      continue;
    desc->canary = compute_canary(desc->base, desc->descriptor_seq);
  }
}

//===----------------------------------------------------------------------===//
// Diagnostics
//===----------------------------------------------------------------------===//

PartitionStats partition_stats_snapshot() {
  PartitionStats s = {};
  s.descriptor_pool_used = static_cast<uint32_t>(
      g_desc_pool_occupancy.popcount());
  ReserveTable *table = mutable_reserve_table();
  if (table == nullptr)
    return s;
  for (uint32_t i = 0; i < kReserveTableSize; ++i) {
    PartitionDescriptor *desc =
        table->slots[i].descriptor.load(cpp::MemoryOrder::RELAXED);
    if (desc == nullptr)
      continue;
    ++s.total_partitions_live;
    s.total_va_reserved_bytes += desc->bytes;
    s.total_committed_bytes +=
        desc->bytes_committed.load(cpp::MemoryOrder::RELAXED);
  }
  return s;
}

} // namespace partition
} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

//===----------------------------------------------------------------------===//
// Section registry hooks
//===----------------------------------------------------------------------===//
//
// Must expand at namespace scope. The Tier A initialiser runs at memory
// phase 5; the fork reinit runs at `kForkPrioPartition`.

LIBC_REGISTER_MEMORY_PRIMITIVE(
    partition, 5,
    &::LIBC_NAMESPACE::windows::alloc::partition::partition_init_fn)

LIBC_REGISTER_FORK_REINIT(
    partition, ::LIBC_NAMESPACE::internal::kForkPrioPartition,
    &::LIBC_NAMESPACE::windows::alloc::partition::partition_fork_reinit)
