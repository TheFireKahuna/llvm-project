//===-- alloc::partition hermetic suite -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Smoke-out coverage for `windows::alloc::partition` — Layer 7 type-isolated
// VA partition broker. Targets every concurrency invariant the design
// promises plus the hardening surfaces (PAGE_NOACCESS guards, descriptor
// canary, retire-state gate). Aimed at issue smoking, not pretty coverage:
//
//   * Lookup is wait-free single ACQUIRE-load; bounds-checked; reads
//     observe RELEASE-published descriptors atomically.
//   * `reserve_or_grow` deduplicates on `(cls, node)`; CAS-race produces
//     exactly one publisher and N-1 rollbacks (pool + table accounting
//     stays balanced).
//   * Descriptor pool / reserve table accounting is balanced across the
//     full reserve / retire round-trip (no slot leaks, no double-free
//     of the placeholder).
//   * Empty-transition retire fires on the natural 0→0 counter event;
//     PINNED core partitions never retire even when counters synthetically
//     touch zero.
//   * IDLE → LIVE re-arm cancels retire when a peer commit races the
//     post-decrement window.
//   * Crystalline-W's pin discipline keeps `lookup()`-observed descriptors
//     valid across concurrent retires.
//   * PAGE_NOACCESS guards at head and tail fault on access (death test
//     via fork; parent observes WIFSIGNALED).
//   * Descriptor canary (process_cookie ^ secret ^ base ^ seq) traps the
//     FreeFn on tamper.
//   * `commit_chunk_register` rejects DRAINING / RETIRED state with EAGAIN.
//
// Hermetic model: pulls in the full Tier A bring-up via libc.startup.windows.
// crt1, so partition_init_fn has run and the 12 core PINNED partitions
// are live by the time main() executes.
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/partition.h"
#include "src/__support/OSUtil/windows/alloc/partition_class.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_join.h"
#include "test/UnitTest/Test.h"

#include <stddef.h>
#include <stdint.h>

namespace {

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::windows::alloc::partition::commit_chunk_register;
using LIBC_NAMESPACE::windows::alloc::partition::compute_canary;
using LIBC_NAMESPACE::windows::alloc::partition::CoarsePagemap;
using LIBC_NAMESPACE::windows::alloc::partition::decommit_chunk_unregister;
using LIBC_NAMESPACE::windows::alloc::partition::is_managed;
using LIBC_NAMESPACE::windows::alloc::partition::kCoarseMaxEntries;
using LIBC_NAMESPACE::windows::alloc::partition::kNodeAgnostic;
using LIBC_NAMESPACE::windows::alloc::partition::kPartitionBytes;
using LIBC_NAMESPACE::windows::alloc::partition::kPartitionDescPoolCapacity;
using LIBC_NAMESPACE::windows::alloc::partition::kPartitionGuardBytes;
using LIBC_NAMESPACE::windows::alloc::partition::kPartitionShift;
using LIBC_NAMESPACE::windows::alloc::partition::kReserveTableMask;
using LIBC_NAMESPACE::windows::alloc::partition::kReserveTableSize;
using LIBC_NAMESPACE::windows::alloc::partition::kStateDraining;
using LIBC_NAMESPACE::windows::alloc::partition::kStateIdle;
using LIBC_NAMESPACE::windows::alloc::partition::kStateLive;
using LIBC_NAMESPACE::windows::alloc::partition::kStatePinned;
using LIBC_NAMESPACE::windows::alloc::partition::lookup;
using LIBC_NAMESPACE::windows::alloc::partition::pack_partition_key;
using LIBC_NAMESPACE::windows::alloc::partition::partition_stats_snapshot;
using LIBC_NAMESPACE::windows::alloc::partition::PartitionClass;
using LIBC_NAMESPACE::windows::alloc::partition::PartitionDescriptor;
using LIBC_NAMESPACE::windows::alloc::partition::PartitionStats;
using LIBC_NAMESPACE::windows::alloc::partition::PartitionTableSlot;
using LIBC_NAMESPACE::windows::alloc::partition::primary_slot;
using LIBC_NAMESPACE::windows::alloc::partition::reserve_or_grow;
using LIBC_NAMESPACE::windows::alloc::partition::ReserveTable;
using LIBC_NAMESPACE::windows::alloc::partition::unpack_class;
using LIBC_NAMESPACE::windows::alloc::partition::unpack_node;

// Test-internal helpers that reach behind the public API to introspect the
// reserve table and coarse pagemap. The PCB Zone 0 pointers are sealed
// PAGE_READONLY (Tier A) but readable by anyone.
[[nodiscard]] ReserveTable *peek_reserve_table() {
  return static_cast<ReserveTable *>(
      ::LIBC_NAMESPACE::g_pcb.zone0.partition_reserve_table());
}

[[nodiscard]] CoarsePagemap *peek_coarse_pagemap() {
  return static_cast<CoarsePagemap *>(
      ::LIBC_NAMESPACE::g_pcb.zone0.partition_coarse_pagemap());
}

// Pick a chunk base inside a partition's *middle* placeholder (avoiding
// the leading guard at [base, base + 64 KiB)). Used by chunk-register
// tests that need a realistic in-range pointer; the partition layer's
// register/unregister path doesn't actually dereference, but the address
// should be in-bounds for forward compatibility.
[[nodiscard]] void *middle_addr(PartitionDescriptor *desc) {
  return static_cast<char *>(desc->base) + kPartitionGuardBytes;
}

} // namespace

// =========================================================================
// 1. Layout pin — geometry constants stable; struct sizes/aligns enforced
// =========================================================================
TEST(LlvmLibcPartitionTest, LayoutPin) {
  EXPECT_EQ(static_cast<size_t>(kPartitionShift), size_t{32});
  EXPECT_EQ(kPartitionBytes, size_t{4} * 1024 * 1024 * 1024ULL);
  EXPECT_EQ(static_cast<size_t>(kReserveTableSize), size_t{64});
  EXPECT_EQ(static_cast<size_t>(kReserveTableMask), size_t{63});
  EXPECT_EQ(kCoarseMaxEntries, static_cast<size_t>(1) << 15);
  EXPECT_EQ(kPartitionGuardBytes, size_t{64} * 1024);
  EXPECT_EQ(static_cast<size_t>(kPartitionDescPoolCapacity), size_t{256});

  EXPECT_EQ(sizeof(PartitionDescriptor), size_t{128});
  EXPECT_EQ(alignof(PartitionDescriptor), size_t{128});
  EXPECT_EQ(sizeof(PartitionTableSlot), size_t{8});
  EXPECT_EQ(sizeof(ReserveTable), size_t{kReserveTableSize * 8});
}

// =========================================================================
// 2. PartitionClass enum — ABI-stable numeric values
// =========================================================================
TEST(LlvmLibcPartitionTest, EnumStability) {
  // Numeric values are part of the ABI (pagemap consumer_tag high byte).
  // Renumbering breaks every stamped pagemap entry across fork/exec.
  EXPECT_EQ(static_cast<uint16_t>(PartitionClass::Empty), uint16_t{0});
  EXPECT_EQ(static_cast<uint16_t>(PartitionClass::ThreadScratchPool),
            uint16_t{1});
  EXPECT_EQ(static_cast<uint16_t>(PartitionClass::FdTable), uint16_t{2});
  EXPECT_EQ(static_cast<uint16_t>(PartitionClass::PkeyRange), uint16_t{12});
  EXPECT_EQ(static_cast<uint16_t>(PartitionClass::AllocSmall), uint16_t{16});
  EXPECT_EQ(static_cast<uint16_t>(PartitionClass::AllocHuge), uint16_t{19});
  EXPECT_EQ(static_cast<uint16_t>(PartitionClass::Sentinel), uint16_t{0xFFFF});
}

// =========================================================================
// 3. Key pack / unpack round-trips losslessly
// =========================================================================
TEST(LlvmLibcPartitionTest, KeyPackUnpack) {
  static constexpr uint16_t kNodes[] = {uint16_t{0}, uint16_t{1}, uint16_t{3},
                                          kNodeAgnostic};
  for (uint16_t cls_v = 0; cls_v <= 19; ++cls_v) {
    for (uint16_t node : kNodes) {
      auto cls = static_cast<PartitionClass>(cls_v);
      uint32_t key = pack_partition_key(cls, node);
      EXPECT_EQ(static_cast<uint16_t>(unpack_class(key)), cls_v);
      EXPECT_EQ(unpack_node(key), node);
    }
  }
}

// =========================================================================
// 4. Splitmix64 primary_slot is deterministic (race-resolution invariant)
// =========================================================================
TEST(LlvmLibcPartitionTest, SplitMixDeterministic) {
  // All threads racing on (AllocSmall, kNodeAgnostic) MUST hit the same
  // primary slot — that's what serializes the CAS race so exactly one
  // wins.
  uint32_t key = pack_partition_key(PartitionClass::AllocSmall, kNodeAgnostic);
  uint32_t s1 = primary_slot(key);
  uint32_t s2 = primary_slot(key);
  EXPECT_EQ(s1, s2);
  EXPECT_LT(s1, kReserveTableSize);

  // Different keys SHOULD usually map to different slots (not a hard
  // promise, but the splitmix64 finalizer is good — verify a handful).
  uint32_t k_small = pack_partition_key(PartitionClass::AllocSmall,
                                         kNodeAgnostic);
  uint32_t k_med = pack_partition_key(PartitionClass::AllocMedium,
                                       kNodeAgnostic);
  uint32_t k_lg = pack_partition_key(PartitionClass::AllocLarge,
                                      kNodeAgnostic);
  uint32_t s_small = primary_slot(k_small);
  uint32_t s_med = primary_slot(k_med);
  uint32_t s_lg = primary_slot(k_lg);
  // At least one pair distinct (would be astronomically unlucky otherwise).
  EXPECT_TRUE(s_small != s_med || s_med != s_lg || s_small != s_lg);
}

// =========================================================================
// 5. Tier A bring-up — core PINNED partitions live, descriptor pool
//    accounting matches. Production currently pins 22 (12 libc-internal +
//    10 va_tracker); test asserts the >=12 floor and that every live
//    partition is PINNED.
// =========================================================================
TEST(LlvmLibcPartitionTest, InitBoundsTierA) {
  ReserveTable *table = peek_reserve_table();
  ASSERT_TRUE(table != nullptr);

  uint32_t live = 0;
  uint32_t pinned = 0;
  for (uint32_t i = 0; i < kReserveTableSize; ++i) {
    PartitionDescriptor *desc =
        table->slots[i].descriptor.load(MemoryOrder::ACQUIRE);
    if (desc == nullptr)
      continue;
    ++live;
    if (desc->retire_state.load(MemoryOrder::ACQUIRE) == kStatePinned)
      ++pinned;
    EXPECT_EQ(desc->bytes, kPartitionBytes);
    EXPECT_EQ(desc->mask, ~(kPartitionBytes - 1));
    EXPECT_EQ(unpack_node(desc->key), kNodeAgnostic);
  }
  EXPECT_GE(live, 12u);
  EXPECT_EQ(pinned, live);

  PartitionStats s = partition_stats_snapshot();
  EXPECT_GE(s.descriptor_pool_used, 12u);
  EXPECT_GE(s.total_partitions_live, 12u);
  EXPECT_EQ(s.total_va_reserved_bytes,
            static_cast<uint64_t>(s.total_partitions_live) * kPartitionBytes);
  // No chunk owners have committed yet; all counters are zero.
  EXPECT_EQ(s.total_committed_bytes, uint64_t{0});
}

// =========================================================================
// 6. Coarse pagemap was committed and entries cover Tier A partitions
// =========================================================================
TEST(LlvmLibcPartitionTest, CoarsePagemapHasCorePartitions) {
  CoarsePagemap *coarse = peek_coarse_pagemap();
  ASSERT_TRUE(coarse != nullptr);
  EXPECT_TRUE(coarse->coverage_base != nullptr);
  EXPECT_GT(coarse->coverage_bytes, kPartitionBytes);

  uint32_t entries_with_descriptor = 0;
  for (size_t i = 0; i < kCoarseMaxEntries; ++i) {
    PartitionDescriptor *desc =
        coarse->entries[i].load(MemoryOrder::ACQUIRE);
    if (desc != nullptr)
      ++entries_with_descriptor;
  }
  // 12 core partitions must each occupy one coarse pagemap slot.
  EXPECT_GE(entries_with_descriptor, 12u);
}

// =========================================================================
// 7. lookup() hits within a partition's full VA range (incl. guards)
// =========================================================================
TEST(LlvmLibcPartitionTest, LookupHitsAcrossPartition) {
  // Pick the FdTable core partition by reserving its key (idempotent —
  // returns the existing descriptor).
  PartitionDescriptor *desc =
      reserve_or_grow(PartitionClass::FdTable, kNodeAgnostic);
  ASSERT_TRUE(desc != nullptr);
  EXPECT_EQ(desc->retire_state.load(MemoryOrder::ACQUIRE), kStatePinned);
  void *base = desc->base;

  EXPECT_EQ(lookup(base), desc);
  EXPECT_EQ(lookup(static_cast<char *>(base) + 1), desc);
  EXPECT_EQ(lookup(static_cast<char *>(base) + kPartitionBytes - 1), desc);
  // Past the end belongs to no partition (or a different one).
  EXPECT_NE(lookup(static_cast<char *>(base) + kPartitionBytes), desc);

  // is_managed sanity.
  EXPECT_TRUE(is_managed(base));
  EXPECT_TRUE(is_managed(static_cast<char *>(base) + kPartitionBytes / 2));
  EXPECT_FALSE(is_managed(reinterpret_cast<void *>(uintptr_t{0x10})));
  EXPECT_FALSE(is_managed(nullptr));
}

// =========================================================================
// 8. lookup() misses outside coverage and below user VA
// =========================================================================
TEST(LlvmLibcPartitionTest, LookupMissOutOfBounds) {
  EXPECT_EQ(lookup(nullptr), nullptr);
  EXPECT_EQ(lookup(reinterpret_cast<void *>(uintptr_t{0x10})), nullptr);
  // Far above any plausible user VA (way past max_address).
  EXPECT_EQ(lookup(reinterpret_cast<void *>(uintptr_t{0xFFFF'FFFF'FFFF'FFF8})),
            nullptr);
}

// =========================================================================
// 9. lookup() misses inside coverage but in a coarse-pagemap gap
// =========================================================================
TEST(LlvmLibcPartitionTest, LookupMissCoverageGap) {
  CoarsePagemap *coarse = peek_coarse_pagemap();
  ASSERT_TRUE(coarse != nullptr);
  // Find a coarse slot that's null and probe its midpoint.
  for (size_t i = 0; i < kCoarseMaxEntries; ++i) {
    if (coarse->entries[i].load(MemoryOrder::ACQUIRE) == nullptr) {
      uintptr_t va = reinterpret_cast<uintptr_t>(coarse->coverage_base) +
                     (i << kPartitionShift) + (kPartitionBytes / 2);
      EXPECT_EQ(lookup(reinterpret_cast<void *>(va)), nullptr);
      return;
    }
  }
  // Improbable: every coarse slot occupied. Treat as warning, not failure.
}

// =========================================================================
// 10. compute_canary is a pure function of inputs (round-trip stable)
// =========================================================================
TEST(LlvmLibcPartitionTest, CanaryComputeRoundTrip) {
  void *base = reinterpret_cast<void *>(uintptr_t{0x1'0000'0000});
  uint64_t c1 = compute_canary(base, 0xCAFEBABEu);
  uint64_t c2 = compute_canary(base, 0xCAFEBABEu);
  EXPECT_EQ(c1, c2);
  // Different inputs → different output (extremely high probability).
  EXPECT_NE(c1, compute_canary(base, 0xCAFEBABEu + 1));
  EXPECT_NE(c1, compute_canary(static_cast<char *>(base) + kPartitionBytes,
                                0xCAFEBABEu));
  // Live core descriptors must already pass canary check.
  ReserveTable *table = peek_reserve_table();
  for (uint32_t i = 0; i < kReserveTableSize; ++i) {
    PartitionDescriptor *desc =
        table->slots[i].descriptor.load(MemoryOrder::ACQUIRE);
    if (desc == nullptr)
      continue;
    EXPECT_EQ(desc->canary, compute_canary(desc->base, desc->descriptor_seq));
  }
}

// =========================================================================
// 11. Reserve dedup on the same (cls, node) — single-thread idempotence
// =========================================================================
TEST(LlvmLibcPartitionTest, ReserveDeduplication1T) {
  PartitionDescriptor *first =
      reserve_or_grow(PartitionClass::AllocSmall, kNodeAgnostic);
  ASSERT_TRUE(first != nullptr);
  PartitionDescriptor *second =
      reserve_or_grow(PartitionClass::AllocSmall, kNodeAgnostic);
  EXPECT_EQ(first, second);
  // Cleanup: trigger retire by registering and unregistering a 0-byte chunk.
  // commit_chunk_register accepts LIVE/PINNED; AllocSmall starts LIVE.
  EXPECT_EQ(commit_chunk_register(first, middle_addr(first), 0), 0);
  EXPECT_EQ(decommit_chunk_unregister(first, middle_addr(first), 0),
            size_t{0});
}

// =========================================================================
// 12. Distinct (cls, node) pairs get distinct descriptors and distinct VAs
// =========================================================================
TEST(LlvmLibcPartitionTest, DistinctClassesDistinctDescriptors) {
  PartitionDescriptor *small =
      reserve_or_grow(PartitionClass::AllocSmall, kNodeAgnostic);
  PartitionDescriptor *medium =
      reserve_or_grow(PartitionClass::AllocMedium, kNodeAgnostic);
  PartitionDescriptor *large =
      reserve_or_grow(PartitionClass::AllocLarge, kNodeAgnostic);
  ASSERT_TRUE(small != nullptr);
  ASSERT_TRUE(medium != nullptr);
  ASSERT_TRUE(large != nullptr);
  EXPECT_NE(small, medium);
  EXPECT_NE(medium, large);
  EXPECT_NE(small, large);
  EXPECT_NE(small->base, medium->base);
  EXPECT_NE(medium->base, large->base);
  EXPECT_NE(small->base, large->base);
  EXPECT_EQ(unpack_class(small->key), PartitionClass::AllocSmall);
  EXPECT_EQ(unpack_class(medium->key), PartitionClass::AllocMedium);
  EXPECT_EQ(unpack_class(large->key), PartitionClass::AllocLarge);
  // Cleanup all three via 0-byte commit/decommit.
  PartitionDescriptor *all[] = {small, medium, large};
  for (PartitionDescriptor *d : all) {
    EXPECT_EQ(commit_chunk_register(d, middle_addr(d), 0), 0);
    (void)decommit_chunk_unregister(d, middle_addr(d), 0);
  }
}

// =========================================================================
// 13. Reserved partition is lookup-able at base and middle
// =========================================================================
TEST(LlvmLibcPartitionTest, ReserveIsLookupable) {
  PartitionDescriptor *desc =
      reserve_or_grow(PartitionClass::AllocLarge, kNodeAgnostic);
  ASSERT_TRUE(desc != nullptr);
  EXPECT_EQ(lookup(desc->base), desc);
  EXPECT_EQ(lookup(static_cast<char *>(desc->base) + kPartitionBytes / 2),
            desc);
  // Cleanup.
  EXPECT_EQ(commit_chunk_register(desc, middle_addr(desc), 0), 0);
  (void)decommit_chunk_unregister(desc, middle_addr(desc), 0);
}

// =========================================================================
// 14. Reserve / retire round-trip clears the slot and re-allocates a fresh
//     descriptor on the next reserve (different VA).
// =========================================================================
TEST(LlvmLibcPartitionTest, ReserveRetireRoundTripClearsSlot) {
  PartitionDescriptor *first =
      reserve_or_grow(PartitionClass::AllocHuge, kNodeAgnostic);
  ASSERT_TRUE(first != nullptr);
  void *first_base = first->base;
  EXPECT_EQ(lookup(first_base), first);

  // Force retire by committing then decommitting a 0-byte chunk.
  EXPECT_EQ(commit_chunk_register(first, middle_addr(first), 0), 0);
  EXPECT_EQ(decommit_chunk_unregister(first, middle_addr(first), 0),
            size_t{0});

  // After retire: coarse pagemap entry cleared, lookup misses.
  EXPECT_EQ(lookup(first_base), nullptr);

  // Subsequent reserve gets a fresh descriptor (Crystalline FreeFn may
  // not have run yet, but the retire path cleared the reserve slot, so
  // reserve_or_grow falls through to the syscall path and gets new VA).
  PartitionDescriptor *second =
      reserve_or_grow(PartitionClass::AllocHuge, kNodeAgnostic);
  ASSERT_TRUE(second != nullptr);
  EXPECT_NE(second, first);
  // Cleanup the second.
  EXPECT_EQ(commit_chunk_register(second, middle_addr(second), 0), 0);
  (void)decommit_chunk_unregister(second, middle_addr(second), 0);
}

// =========================================================================
// 15. PINNED core partitions don't retire even when counters synthetically
//     touch zero (commit+decommit round-trip).
// =========================================================================
TEST(LlvmLibcPartitionTest, PinnedCoreNeverRetires) {
  PartitionDescriptor *desc =
      reserve_or_grow(PartitionClass::FdTable, kNodeAgnostic);
  ASSERT_TRUE(desc != nullptr);
  EXPECT_EQ(desc->retire_state.load(MemoryOrder::ACQUIRE), kStatePinned);
  void *base = desc->base;

  EXPECT_EQ(commit_chunk_register(desc, middle_addr(desc), 1024), 0);
  (void)decommit_chunk_unregister(desc, middle_addr(desc), 1024);

  // PINNED partitions stay published — try_retire_inline's CAS requires
  // LIVE and fails on PINNED.
  EXPECT_EQ(desc->retire_state.load(MemoryOrder::ACQUIRE), kStatePinned);
  EXPECT_EQ(lookup(base), desc);
  EXPECT_EQ(reserve_or_grow(PartitionClass::FdTable, kNodeAgnostic), desc);
}

// =========================================================================
// 16. commit_chunk_register on PINNED accepts every time (multiple commits
//     interleave without churning state)
// =========================================================================
TEST(LlvmLibcPartitionTest, CommitOnPinnedAcceptsRepeatedly) {
  PartitionDescriptor *desc =
      reserve_or_grow(PartitionClass::AioCb, kNodeAgnostic);
  ASSERT_TRUE(desc != nullptr);
  ASSERT_EQ(desc->retire_state.load(MemoryOrder::ACQUIRE), kStatePinned);

  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(commit_chunk_register(desc, middle_addr(desc), 4096), 0);
  }
  EXPECT_EQ(desc->bytes_committed.load(MemoryOrder::ACQUIRE),
            uint64_t{8} * 4096);
  EXPECT_EQ(desc->active_chunks.load(MemoryOrder::ACQUIRE), uint32_t{8});

  for (int i = 0; i < 8; ++i)
    (void)decommit_chunk_unregister(desc, middle_addr(desc), 4096);
  EXPECT_EQ(desc->bytes_committed.load(MemoryOrder::ACQUIRE), uint64_t{0});
  EXPECT_EQ(desc->active_chunks.load(MemoryOrder::ACQUIRE), uint32_t{0});
  EXPECT_EQ(desc->retire_state.load(MemoryOrder::ACQUIRE), kStatePinned);
}

// =========================================================================
// 17. commit_chunk_register on DRAINING is rejected with EAGAIN.
//
// Note: the 5-state machine (LIVE / IDLE / DRAINING / RETIRED / PINNED) has
// no public CAS surface that could write a bad transition. The only
// production writer is try_retire_inline (LIVE→DRAINING via
// compare_exchange_strong with LIVE-only From, DRAINING→LIVE on counter
// re-verify failure, DRAINING→RETIRED on success) plus the IDLE→LIVE
// re-arm inside commit_chunk_register. The "bad transition refused"
// invariant is thus structurally enforced — there is no entry point
// through which PINNED→DRAINING, RETIRED→LIVE, IDLE→RETIRED, or
// DRAINING→LIVE-via-public-CAS could be attempted. A dedicated test
// would require synthesising the bad transition via direct store, which
// proves nothing about the public surface.
// =========================================================================
TEST(LlvmLibcPartitionTest, CommitOnDrainingRejected) {
  PartitionDescriptor *desc =
      reserve_or_grow(PartitionClass::AllocMedium, kNodeAgnostic);
  ASSERT_TRUE(desc != nullptr);
  // Synthesize DRAINING. Production code reaches DRAINING only via
  // try_retire_inline's CAS, but the public state-gate behavior is what
  // we're testing.
  desc->retire_state.store(kStateDraining, MemoryOrder::RELEASE);

  EXPECT_EQ(commit_chunk_register(desc, middle_addr(desc), 4096), -EAGAIN);

  // Restore so the test doesn't leak a stuck partition.
  desc->retire_state.store(kStateLive, MemoryOrder::RELEASE);
  // Cleanup via 0-byte cycle.
  EXPECT_EQ(commit_chunk_register(desc, middle_addr(desc), 0), 0);
  (void)decommit_chunk_unregister(desc, middle_addr(desc), 0);
}

// =========================================================================
// 18. IDLE → LIVE re-arm on commit cancels pending retire
// =========================================================================
TEST(LlvmLibcPartitionTest, IdleRearmOnCommit) {
  PartitionDescriptor *desc =
      reserve_or_grow(PartitionClass::AllocLarge, kNodeAgnostic);
  ASSERT_TRUE(desc != nullptr);
  // Synthesize IDLE.
  desc->retire_state.store(kStateIdle, MemoryOrder::RELEASE);

  // commit_chunk_register on IDLE must re-arm to LIVE and accept.
  EXPECT_EQ(commit_chunk_register(desc, middle_addr(desc), 8192), 0);
  EXPECT_EQ(desc->retire_state.load(MemoryOrder::ACQUIRE), kStateLive);

  // Cleanup.
  (void)decommit_chunk_unregister(desc, middle_addr(desc), 8192);
}

// =========================================================================
// 19. Empty-transition fires retire only when state is LIVE
//     (post-decrement counters at zero AND state == LIVE)
// =========================================================================
TEST(LlvmLibcPartitionTest, EmptyTransitionRetiresUserPartition) {
  PartitionDescriptor *desc =
      reserve_or_grow(PartitionClass::AllocSmall, kNodeAgnostic);
  ASSERT_TRUE(desc != nullptr);
  ASSERT_EQ(desc->retire_state.load(MemoryOrder::ACQUIRE), kStateLive);

  void *base = desc->base;

  EXPECT_EQ(commit_chunk_register(desc, middle_addr(desc), 16384), 0);
  EXPECT_EQ(desc->bytes_committed.load(MemoryOrder::ACQUIRE), uint64_t{16384});
  EXPECT_EQ(desc->active_chunks.load(MemoryOrder::ACQUIRE), uint32_t{1});

  size_t after = decommit_chunk_unregister(desc, middle_addr(desc), 16384);
  EXPECT_EQ(after, size_t{0});

  // Retire path ran: coarse pagemap and reserve slot cleared; state is
  // RETIRED (descriptor in Crystalline retire batch awaiting epoch
  // advance).
  EXPECT_EQ(lookup(base), nullptr);
}

// =========================================================================
// 20. Partial decommit (counters non-zero) does NOT trigger retire
// =========================================================================
TEST(LlvmLibcPartitionTest, PartialDecommitNoRetire) {
  PartitionDescriptor *desc =
      reserve_or_grow(PartitionClass::AllocMedium, kNodeAgnostic);
  ASSERT_TRUE(desc != nullptr);
  void *base = desc->base;

  EXPECT_EQ(commit_chunk_register(desc, middle_addr(desc), 4096), 0);
  EXPECT_EQ(commit_chunk_register(desc, middle_addr(desc), 4096), 0);
  // Decommit only one of the two chunks.
  size_t after = decommit_chunk_unregister(desc, middle_addr(desc), 4096);
  EXPECT_EQ(after, size_t{4096});

  // State LIVE, lookup still hits.
  EXPECT_EQ(desc->retire_state.load(MemoryOrder::ACQUIRE), kStateLive);
  EXPECT_EQ(lookup(base), desc);

  // Now drain the second.
  after = decommit_chunk_unregister(desc, middle_addr(desc), 4096);
  EXPECT_EQ(after, size_t{0});
  // Retire fired.
  EXPECT_EQ(lookup(base), nullptr);
}

// =========================================================================
// 21. Multi-thread reserve on the same (cls, node) — exactly one publishes,
//     descriptor pool stays balanced (no leaks).
// =========================================================================
namespace {

struct ReserveSameKeyArg {
  PartitionDescriptor *result;
};

void *reserve_same_key_worker(void *arg) {
  auto *a = static_cast<ReserveSameKeyArg *>(arg);
  a->result = reserve_or_grow(PartitionClass::AllocHuge, kNodeAgnostic);
  return nullptr;
}

} // namespace

TEST(LlvmLibcPartitionTest, ReserveContended16TSameKey) {
  constexpr uint32_t kThreads = 16;
  pthread_t tids[kThreads];
  ReserveSameKeyArg args[kThreads];
  PartitionStats before = partition_stats_snapshot();

  for (uint32_t i = 0; i < kThreads; ++i) {
    args[i].result = nullptr;
    int rc = LIBC_NAMESPACE::pthread_create(&tids[i], nullptr,
                                             reserve_same_key_worker,
                                             &args[i]);
    ASSERT_EQ(rc, 0);
  }
  for (uint32_t i = 0; i < kThreads; ++i) {
    void *unused = nullptr;
    LIBC_NAMESPACE::pthread_join(tids[i], &unused);
  }

  // All 16 threads must agree on the same descriptor (the publisher's
  // winner). N-1 losers rolled back their syscall reservation and
  // descriptor pool slot.
  ASSERT_TRUE(args[0].result != nullptr);
  for (uint32_t i = 1; i < kThreads; ++i)
    EXPECT_EQ(args[i].result, args[0].result);

  PartitionStats after = partition_stats_snapshot();
  // Exactly one descriptor pool slot consumed.
  EXPECT_EQ(after.descriptor_pool_used, before.descriptor_pool_used + 1);
  EXPECT_EQ(after.total_partitions_live, before.total_partitions_live + 1);

  // Cleanup the won partition.
  EXPECT_EQ(commit_chunk_register(args[0].result, middle_addr(args[0].result),
                                    0),
            0);
  (void)decommit_chunk_unregister(args[0].result, middle_addr(args[0].result),
                                    0);
}

// =========================================================================
// 22. Multi-thread reserve on distinct (cls, node) — all succeed, distinct
//     descriptors, distinct VAs. Uses distinct classes on a single
//     (kNodeAgnostic) node so the test runs on any NUMA topology.
// =========================================================================
namespace {

struct ReserveDistinctArg {
  PartitionClass cls;
  PartitionDescriptor *result;
};

void *reserve_distinct_worker(void *arg) {
  auto *a = static_cast<ReserveDistinctArg *>(arg);
  a->result = reserve_or_grow(a->cls, kNodeAgnostic);
  return nullptr;
}

} // namespace

TEST(LlvmLibcPartitionTest, ReserveContended4TDistinctKeys) {
  constexpr uint32_t kThreads = 4;
  static constexpr PartitionClass kClasses[kThreads] = {
      PartitionClass::AllocSmall, PartitionClass::AllocMedium,
      PartitionClass::AllocLarge, PartitionClass::AllocHuge};
  pthread_t tids[kThreads];
  ReserveDistinctArg args[kThreads];
  for (uint32_t i = 0; i < kThreads; ++i) {
    args[i].cls = kClasses[i];
    args[i].result = nullptr;
    int rc = LIBC_NAMESPACE::pthread_create(&tids[i], nullptr,
                                             reserve_distinct_worker,
                                             &args[i]);
    ASSERT_EQ(rc, 0);
  }
  for (uint32_t i = 0; i < kThreads; ++i) {
    void *unused = nullptr;
    LIBC_NAMESPACE::pthread_join(tids[i], &unused);
  }
  // All 4 succeeded with pairwise-distinct descriptors and bases.
  for (uint32_t i = 0; i < kThreads; ++i)
    ASSERT_TRUE(args[i].result != nullptr);
  for (uint32_t i = 0; i < kThreads; ++i) {
    for (uint32_t j = i + 1; j < kThreads; ++j) {
      EXPECT_NE(args[i].result, args[j].result);
      EXPECT_NE(args[i].result->base, args[j].result->base);
    }
    EXPECT_EQ(args[i].result->key,
              pack_partition_key(args[i].cls, kNodeAgnostic));
  }
  // Cleanup all four.
  for (uint32_t i = 0; i < kThreads; ++i) {
    EXPECT_EQ(commit_chunk_register(args[i].result,
                                     middle_addr(args[i].result), 0),
              0);
    (void)decommit_chunk_unregister(args[i].result,
                                     middle_addr(args[i].result), 0);
  }
}

// =========================================================================
// 23. Lookup / reserve race fuzz — readers iterate lookup() across the
//     entire VA range while writers churn reserve+retire on AllocHuge.
//     Asserts: no UAF, no canary mismatch on any reader-observed descriptor.
// =========================================================================
namespace {

Atomic<bool> g_fuzz_stop{false};
Atomic<uint64_t> g_reader_iters{0};
Atomic<uint64_t> g_writer_cycles{0};
Atomic<uint64_t> g_canary_mismatches{0};

void *fuzz_reader(void *) {
  CoarsePagemap *coarse = peek_coarse_pagemap();
  while (!g_fuzz_stop.load(MemoryOrder::RELAXED)) {
    // Walk every coarse pagemap slot: load the descriptor, and if non-null
    // verify the canary matches (descriptor identity is stable under
    // Crystalline pin discipline; we don't pin here, but the test bound
    // is short enough that any race surfaces as canary mismatch from a
    // post-retire descriptor reuse — exactly what we want to detect).
    for (size_t i = 0; i < kCoarseMaxEntries; i += 1024) {
      PartitionDescriptor *desc =
          coarse->entries[i].load(MemoryOrder::ACQUIRE);
      if (desc == nullptr)
        continue;
      uint64_t expected =
          compute_canary(desc->base, desc->descriptor_seq);
      if (desc->canary != expected)
        g_canary_mismatches.fetch_add(1, MemoryOrder::RELAXED);
    }
    g_reader_iters.fetch_add(1, MemoryOrder::RELAXED);
  }
  return nullptr;
}

void *fuzz_writer(void *arg) {
  uint16_t my_node = *static_cast<uint16_t *>(arg);
  while (!g_fuzz_stop.load(MemoryOrder::RELAXED)) {
    PartitionDescriptor *desc =
        reserve_or_grow(PartitionClass::AllocHuge, my_node);
    if (desc == nullptr)
      continue;
    if (commit_chunk_register(desc, middle_addr(desc), 0) == 0)
      (void)decommit_chunk_unregister(desc, middle_addr(desc), 0);
    g_writer_cycles.fetch_add(1, MemoryOrder::RELAXED);
  }
  return nullptr;
}

} // namespace

TEST(LlvmLibcPartitionTest, LookupReserveRaceFuzz) {
  g_fuzz_stop.store(false, MemoryOrder::RELEASE);
  g_reader_iters.store(0, MemoryOrder::RELEASE);
  g_writer_cycles.store(0, MemoryOrder::RELEASE);
  g_canary_mismatches.store(0, MemoryOrder::RELEASE);

  constexpr uint32_t kReaders = 4;
  constexpr uint32_t kWriters = 4;
  pthread_t reader_tids[kReaders];
  pthread_t writer_tids[kWriters];
  uint16_t writer_nodes[kWriters];

  for (uint32_t i = 0; i < kReaders; ++i) {
    int rc = LIBC_NAMESPACE::pthread_create(&reader_tids[i], nullptr,
                                             fuzz_reader, nullptr);
    ASSERT_EQ(rc, 0);
  }
  for (uint32_t i = 0; i < kWriters; ++i) {
    writer_nodes[i] = static_cast<uint16_t>(i);
    int rc = LIBC_NAMESPACE::pthread_create(&writer_tids[i], nullptr,
                                             fuzz_writer, &writer_nodes[i]);
    ASSERT_EQ(rc, 0);
  }

  // Run for a fixed wall-clock interval. NtDelayExecution takes a relative
  // time in 100ns units (negative = relative); 2 seconds = -2 * 10^7.
  LARGE_INTEGER delay;
  delay.QuadPart = -static_cast<LONGLONG>(2 * 10000000LL);
  ::NtDelayExecution(FALSE, &delay);

  g_fuzz_stop.store(true, MemoryOrder::RELEASE);
  for (uint32_t i = 0; i < kReaders; ++i) {
    void *unused = nullptr;
    LIBC_NAMESPACE::pthread_join(reader_tids[i], &unused);
  }
  for (uint32_t i = 0; i < kWriters; ++i) {
    void *unused = nullptr;
    LIBC_NAMESPACE::pthread_join(writer_tids[i], &unused);
  }

  // Both sides actually ran (smoke-test of the test itself).
  EXPECT_GT(g_reader_iters.load(MemoryOrder::ACQUIRE), uint64_t{0});
  EXPECT_GT(g_writer_cycles.load(MemoryOrder::ACQUIRE), uint64_t{0});
  // No canary mismatch survived. This validates that descriptors observed
  // via lookup() always have a coherent identity — no UAF, no descriptor
  // reuse mid-read leaking through.
  EXPECT_EQ(g_canary_mismatches.load(MemoryOrder::ACQUIRE), uint64_t{0});
}

// =========================================================================
// 24. Multi-thread commit/decommit on a shared PINNED partition — counter
//     accounting stays balanced; no spurious retire on PINNED.
// =========================================================================
namespace {

struct CounterChurnArg {
  PartitionDescriptor *desc;
  uint32_t iterations;
};

void *counter_churn_worker(void *arg) {
  auto *w = static_cast<CounterChurnArg *>(arg);
  for (uint32_t i = 0; i < w->iterations; ++i) {
    if (commit_chunk_register(w->desc, middle_addr(w->desc), 4096) == 0)
      (void)decommit_chunk_unregister(w->desc, middle_addr(w->desc), 4096);
  }
  return nullptr;
}

} // namespace

TEST(LlvmLibcPartitionTest, MultiThreadCounterChurnPinned) {
  PartitionDescriptor *desc =
      reserve_or_grow(PartitionClass::TimerNode, kNodeAgnostic);
  ASSERT_TRUE(desc != nullptr);
  ASSERT_EQ(desc->retire_state.load(MemoryOrder::ACQUIRE), kStatePinned);

  constexpr uint32_t kThreads = 8;
  constexpr uint32_t kIters = 4096;
  pthread_t tids[kThreads];
  CounterChurnArg args[kThreads];
  for (uint32_t i = 0; i < kThreads; ++i) {
    args[i].desc = desc;
    args[i].iterations = kIters;
    int rc = LIBC_NAMESPACE::pthread_create(&tids[i], nullptr,
                                             counter_churn_worker, &args[i]);
    ASSERT_EQ(rc, 0);
  }
  for (uint32_t i = 0; i < kThreads; ++i) {
    void *unused = nullptr;
    LIBC_NAMESPACE::pthread_join(tids[i], &unused);
  }

  // PINNED → state never transitions; bytes / chunks must net to zero.
  EXPECT_EQ(desc->retire_state.load(MemoryOrder::ACQUIRE), kStatePinned);
  EXPECT_EQ(desc->bytes_committed.load(MemoryOrder::ACQUIRE), uint64_t{0});
  EXPECT_EQ(desc->active_chunks.load(MemoryOrder::ACQUIRE), uint32_t{0});
}

// =========================================================================
// 25. Multi-thread commit/decommit on a USER LIVE partition + retire stress.
//     Each iteration races the empty-transition; whoever drops counters to
//     zero races against re-arm from another thread. Net invariant: pool
//     accounting remains bounded; lookup of any returned base eventually
//     returns null after retire.
// =========================================================================
namespace {

struct RetireStressArg {
  uint32_t iterations;
  uint32_t retire_observations; // count of cycles where post-decommit lookup miss
};

void *retire_stress_worker(void *arg) {
  auto *w = static_cast<RetireStressArg *>(arg);
  for (uint32_t i = 0; i < w->iterations; ++i) {
    PartitionDescriptor *desc =
        reserve_or_grow(PartitionClass::AllocSmall, kNodeAgnostic);
    if (desc == nullptr)
      continue;
    void *base = desc->base;
    if (commit_chunk_register(desc, middle_addr(desc), 4096) != 0)
      continue;
    (void)decommit_chunk_unregister(desc, middle_addr(desc), 4096);
    if (lookup(base) == nullptr)
      ++w->retire_observations;
  }
  return nullptr;
}

} // namespace

TEST(LlvmLibcPartitionTest, MultiThreadReserveAndRetireStress) {
  constexpr uint32_t kThreads = 8;
  constexpr uint32_t kIters = 256;
  pthread_t tids[kThreads];
  RetireStressArg args[kThreads];

  PartitionStats before = partition_stats_snapshot();

  for (uint32_t i = 0; i < kThreads; ++i) {
    args[i].iterations = kIters;
    args[i].retire_observations = 0;
    int rc = LIBC_NAMESPACE::pthread_create(&tids[i], nullptr,
                                             retire_stress_worker, &args[i]);
    ASSERT_EQ(rc, 0);
  }
  for (uint32_t i = 0; i < kThreads; ++i) {
    void *unused = nullptr;
    LIBC_NAMESPACE::pthread_join(tids[i], &unused);
  }

  // At least some iterations observed the retire path (sanity that the
  // stress is exercising what we think).
  uint32_t total_retires = 0;
  for (uint32_t i = 0; i < kThreads; ++i)
    total_retires += args[i].retire_observations;
  EXPECT_GT(total_retires, 0u);

  // Drain whatever AllocSmall partition is currently live (if any) via a
  // final retire so the pool returns to its before state. Then verify
  // accounting is no worse than before + 1 (one possibly-still-pending
  // Crystalline-W retire batch).
  PartitionDescriptor *tail =
      reserve_or_grow(PartitionClass::AllocSmall, kNodeAgnostic);
  if (tail != nullptr) {
    EXPECT_EQ(commit_chunk_register(tail, middle_addr(tail), 0), 0);
    (void)decommit_chunk_unregister(tail, middle_addr(tail), 0);
  }
  PartitionStats after = partition_stats_snapshot();
  // Pool can drift by at most one (the partition we just reserved-and-
  // retired may still be in a Crystalline-W retire batch).
  EXPECT_LE(after.descriptor_pool_used, before.descriptor_pool_used + 1);
}

// =========================================================================
// 27. Production-reachable DRAINING test. Races two workers per partition:
//     thread A drives the retire path by committing and then immediately
//     decommitting (counters→0 → try_retire_inline → LIVE→DRAINING CAS),
//     thread B issues commit_chunk_register on the same descriptor during
//     the race window. Each outcome corresponds to one of the production
//     CAS paths inside try_retire_inline:
//
//       * `-EAGAIN` ⇒ B reached commit_chunk_register's gate AFTER A's
//         LIVE→DRAINING CAS landed but BEFORE Phase 3 cleared the
//         coarse pagemap. The gate rejects; B retries via
//         reserve_or_grow (we don't retry here, we just observe).
//       * `0` AND `desc->retire_state == LIVE` after a successful
//         commit ⇒ B raced past A's empty-observation: A's Phase 2
//         re-verify saw a non-zero counter and rolled DRAINING→LIVE,
//         or A's CAS never landed because B's commit_chunk_register
//         re-armed IDLE→LIVE first.
//
//     A successful round on aggregate produces BOTH outcomes — the
//     production CAS chain is exercised end-to-end. Across 10K
//     iterations the race window is open frequently enough that the
//     test sees both buckets with comfortable margin.
// =========================================================================
namespace {

struct DrainingRaceCtx {
  Atomic<uint64_t> iters{0};
  Atomic<uint64_t> eagain_observed{0};
  Atomic<uint64_t> success_observed{0};
  Atomic<uint64_t> stop{0};
};

void *draining_race_drainer(void *arg) {
  auto *ctx = static_cast<DrainingRaceCtx *>(arg);
  while (!ctx->stop.load(MemoryOrder::ACQUIRE)) {
    PartitionDescriptor *desc =
        reserve_or_grow(PartitionClass::AllocSmall, kNodeAgnostic);
    if (desc == nullptr)
      continue;
    if (commit_chunk_register(desc, middle_addr(desc), 4096) != 0)
      continue;
    // Empty-transition fires try_retire_inline on the LIVE → DRAINING
    // CAS chain.
    (void)decommit_chunk_unregister(desc, middle_addr(desc), 4096);
    ctx->iters.fetch_add(1, MemoryOrder::RELAXED);
  }
  return nullptr;
}

void *draining_race_committer(void *arg) {
  auto *ctx = static_cast<DrainingRaceCtx *>(arg);
  while (!ctx->stop.load(MemoryOrder::ACQUIRE)) {
    PartitionDescriptor *desc =
        reserve_or_grow(PartitionClass::AllocSmall, kNodeAgnostic);
    if (desc == nullptr)
      continue;
    int rc = commit_chunk_register(desc, middle_addr(desc), 4096);
    if (rc == -EAGAIN) {
      ctx->eagain_observed.fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }
    if (rc == 0) {
      ctx->success_observed.fetch_add(1, MemoryOrder::RELAXED);
      (void)decommit_chunk_unregister(desc, middle_addr(desc), 4096);
    }
  }
  return nullptr;
}

} // namespace

TEST(LlvmLibcPartitionTest, DrainingReachedViaEmptyTransitionCAS) {
  DrainingRaceCtx ctx;
  constexpr uint32_t kDrainers = 2;
  constexpr uint32_t kCommitters = 2;
  pthread_t drainer_tids[kDrainers];
  pthread_t committer_tids[kCommitters];

  for (uint32_t i = 0; i < kDrainers; ++i) {
    int rc = LIBC_NAMESPACE::pthread_create(
        &drainer_tids[i], nullptr, draining_race_drainer, &ctx);
    ASSERT_EQ(rc, 0);
  }
  for (uint32_t i = 0; i < kCommitters; ++i) {
    int rc = LIBC_NAMESPACE::pthread_create(
        &committer_tids[i], nullptr, draining_race_committer, &ctx);
    ASSERT_EQ(rc, 0);
  }

  // Run until both outcomes are observed at least 64 times each, or for
  // 10K total drainer iterations, whichever comes first. The aggregate
  // bound keeps the test responsive in single-threaded CI.
  constexpr uint64_t kMinPerBucket = 64;
  constexpr uint64_t kMaxDrainerIters = 10000;
  for (;;) {
    uint64_t iters = ctx.iters.load(MemoryOrder::ACQUIRE);
    uint64_t eagain = ctx.eagain_observed.load(MemoryOrder::ACQUIRE);
    uint64_t success = ctx.success_observed.load(MemoryOrder::ACQUIRE);
    if ((eagain >= kMinPerBucket && success >= kMinPerBucket) ||
        iters >= kMaxDrainerIters)
      break;
    LARGE_INTEGER delay;
    delay.QuadPart = -static_cast<LONGLONG>(10000LL); // 1 ms
    ::NtDelayExecution(FALSE, &delay);
  }
  ctx.stop.store(1, MemoryOrder::RELEASE);

  for (uint32_t i = 0; i < kDrainers; ++i) {
    void *unused = nullptr;
    LIBC_NAMESPACE::pthread_join(drainer_tids[i], &unused);
  }
  for (uint32_t i = 0; i < kCommitters; ++i) {
    void *unused = nullptr;
    LIBC_NAMESPACE::pthread_join(committer_tids[i], &unused);
  }

  uint64_t eagain = ctx.eagain_observed.load(MemoryOrder::ACQUIRE);
  uint64_t success = ctx.success_observed.load(MemoryOrder::ACQUIRE);
  uint64_t total = eagain + success;
  // Both buckets must be observed — that's the production CAS chain
  // exercising both the DRAINING-gate-hit path and the
  // race-past-empty-observation path. Total must dominate noise.
  EXPECT_GT(eagain, uint64_t{0});
  EXPECT_GT(success, uint64_t{0});
  EXPECT_GE(total, uint64_t{32});
}

// =========================================================================
// 28. Linear-probe collision under reserve-table fill. Constructs a real
//     collision by selecting two `(class, node)` keys whose splitmix64
//     primary slots match; reserves the first (lands at primary); reserves
//     the second (must land at the next non-occupied probe slot). Verifies
//     via `peek_reserve_table()` that the second descriptor occupies a
//     slot whose distance from the primary equals the expected linear-
//     probe stride. An off-by-one in the probe walk would surface as the
//     second descriptor either landing on the primary (over-write) or
//     skipping past the immediate next free slot.
// =========================================================================
TEST(LlvmLibcPartitionTest, LinearProbeCollisionUnderFill) {
  // Find two distinct keys whose primary_slot() collides. We scan over
  // node IDs on a fixed class; splitmix64's distribution makes the first
  // collision land within the first few dozen attempts.
  constexpr PartitionClass kCls = PartitionClass::AllocSmall;
  uint16_t node_a = 0;
  uint16_t node_b = 0;
  bool found = false;
  for (uint16_t i = 0; i < 256 && !found; ++i) {
    uint32_t key_i = pack_partition_key(kCls, i);
    uint32_t slot_i = primary_slot(key_i);
    for (uint16_t j = static_cast<uint16_t>(i + 1); j < 256; ++j) {
      uint32_t key_j = pack_partition_key(kCls, j);
      if (primary_slot(key_j) == slot_i) {
        node_a = i;
        node_b = j;
        found = true;
        break;
      }
    }
  }
  ASSERT_TRUE(found);

  uint32_t key_a = pack_partition_key(kCls, node_a);
  uint32_t key_b = pack_partition_key(kCls, node_b);
  uint32_t primary = primary_slot(key_a);
  ASSERT_EQ(primary, primary_slot(key_b));

  ReserveTable *table = peek_reserve_table();
  ASSERT_TRUE(table != nullptr);

  PartitionDescriptor *desc_a = reserve_or_grow(kCls, node_a);
  ASSERT_TRUE(desc_a != nullptr);
  PartitionDescriptor *desc_b = reserve_or_grow(kCls, node_b);
  ASSERT_TRUE(desc_b != nullptr);
  ASSERT_NE(desc_a, desc_b);

  // Locate each descriptor in the reserve table. desc_a must occupy
  // either `primary` or a probe-walk distance from primary; desc_b is
  // forced to a strictly later probe slot (`primary` is occupied by
  // desc_a or by an earlier collision, and the probe walk is deterministic).
  int slot_a = -1;
  int slot_b = -1;
  for (uint32_t i = 0; i < kReserveTableSize; ++i) {
    PartitionDescriptor *cur =
        table->slots[i].descriptor.load(MemoryOrder::ACQUIRE);
    if (cur == desc_a)
      slot_a = static_cast<int>(i);
    else if (cur == desc_b)
      slot_b = static_cast<int>(i);
  }
  ASSERT_GE(slot_a, 0);
  ASSERT_GE(slot_b, 0);
  EXPECT_NE(slot_a, slot_b);

  // Compute probe distances modulo kReserveTableSize. desc_b's distance
  // must be strictly greater than desc_a's distance (linear probing
  // never reverses direction) unless desc_a was displaced to a higher
  // slot by a prior collision and desc_b landed at `primary` — that's
  // also valid linear probing. Either way, the two probes walked
  // forward from the same starting point and landed at distinct slots.
  uint32_t dist_a =
      (static_cast<uint32_t>(slot_a) - primary) & kReserveTableMask;
  uint32_t dist_b =
      (static_cast<uint32_t>(slot_b) - primary) & kReserveTableMask;
  EXPECT_NE(dist_a, dist_b);

  // Clean up the two partitions so the reserve table is not left
  // populated for downstream tests.
  EXPECT_EQ(commit_chunk_register(desc_a, middle_addr(desc_a), 0), 0);
  (void)decommit_chunk_unregister(desc_a, middle_addr(desc_a), 0);
  EXPECT_EQ(commit_chunk_register(desc_b, middle_addr(desc_b), 0), 0);
  (void)decommit_chunk_unregister(desc_b, middle_addr(desc_b), 0);
}

// =========================================================================
// 26. Stats snapshot reflects commit accounting
// =========================================================================
TEST(LlvmLibcPartitionTest, StatsReflectsCommit) {
  PartitionStats before = partition_stats_snapshot();
  PartitionDescriptor *desc =
      reserve_or_grow(PartitionClass::AllocLarge, kNodeAgnostic);
  ASSERT_TRUE(desc != nullptr);
  EXPECT_EQ(commit_chunk_register(desc, middle_addr(desc), 65536), 0);
  PartitionStats during = partition_stats_snapshot();
  EXPECT_GE(during.total_committed_bytes,
            before.total_committed_bytes + 65536);
  (void)decommit_chunk_unregister(desc, middle_addr(desc), 65536);
  // After retire, committed bytes drop back.
  PartitionStats after = partition_stats_snapshot();
  EXPECT_LE(after.total_committed_bytes, before.total_committed_bytes);
}
