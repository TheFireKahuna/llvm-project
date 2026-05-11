//===-- VaSubstrate implementation --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/alloc/legacy/va_substrate.h"

#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/memory/legacy/mapping_table.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/process_control_block_access.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

// ============================================================================
// Compile-time layout constants — one block per class.
// ============================================================================
//
// Layout invariant, enforced by static_assert below, for every class C:
//
//   [ 0              .. 4096              )   leading guard  (uncommitted)
//   [ 4096           .. 8192              )   ArenaHeader    (committed META)
//   [ 8192           .. 12288             )   inter guard    (uncommitted)
//   [ 12288          .. 12288+N*slot_size )   N subslots     (uncommitted;
//                                              each committed on demand)
//   [ last 4 KB of arena                  )   trailing guard (uncommitted)
//
// The header_offset is 4 KB; the slot region starts at 12 KB. Between the
// slot region and the trailing guard the arena may carry slack if the slot
// region doesn't exactly fill the remaining arena. The slot region size
// is N × slot_size and is asserted to fit entirely before the trailing
// guard.
//
// Arena size is a power of 2 so `arena_base = ptr & ~(arena_size - 1)`
// is one AND. Reservation requests this alignment via
// MEM_ADDRESS_REQUIREMENTS.Alignment; NT honours power-of-2 alignment
// ≥ allocation granularity.

// kGuardBytes / kHeaderPageBytes / kLeadingBytes / kTrailingGuardBytes
// live in va_substrate.h so ArenaHeader's inline accessors can use them
// without pulling this TU's internals.

template <SubSlotClass C> struct ClassLayout;

template <> struct ClassLayout<SubSlotClass::Small> {
  static constexpr size_t kArenaSize = size_t{1} << 20;   // 1 MB
  static constexpr size_t kSlotSize = size_t{16} << 10;   // 16 KB
  static constexpr unsigned kSlotsPerArena = 60;          // 60 × 16 KB = 960 KB
  static constexpr size_t kHeaderOffset = kGuardBytes;
  static constexpr size_t kSlotRegionOffset = kLeadingBytes;
  static_assert(kSlotRegionOffset + kSlotsPerArena * kSlotSize +
                        kTrailingGuardBytes <=
                    kArenaSize,
                "Small arena overflows its reserved VA");
  static_assert(kSlotsPerArena <= kMaxSlotsPerArena,
                "Small: slot count exceeds header bitmap capacity");
  static_assert((kArenaSize & (kArenaSize - 1)) == 0,
                "Small arena size must be power of 2");
};

template <> struct ClassLayout<SubSlotClass::Medium> {
  static constexpr size_t kArenaSize = size_t{2} << 20;   // 2 MB
  static constexpr size_t kSlotSize = size_t{64} << 10;   // 64 KB
  // 30 × 64 KB = 1920 KB. Down from 31 to make room for a slot-sized
  // leading region (see kSlotRegionOffset). 60 KB of slack remains
  // between the slot region's end and the trailing guard.
  static constexpr unsigned kSlotsPerArena = 30;
  static constexpr size_t kHeaderOffset = kGuardBytes;
  // SlabPool xthread-offset encoding (slab_pool.h xthread_pack /
  // xthread_unpack_node) recovers the slab base from a node pointer
  // via `node & ~(kSlotSize - 1)` and rebuilds the node via
  // `slab | offset`. Both require slot bases to be aligned to kSlotSize.
  // The default kLeadingBytes (12 KB) is page-aligned but NOT slot-
  // aligned, so a substrate-served Medium slab fails the invariant.
  // Round the leading region up to a full slot. The 4 KB header + two
  // 4 KB guards still live in the first 12 KB; the next 52 KB is
  // unused slack.
  static constexpr size_t kSlotRegionOffset = kSlotSize;  // 64 KB
  static_assert(kSlotRegionOffset + kSlotsPerArena * kSlotSize +
                        kTrailingGuardBytes <=
                    kArenaSize,
                "Medium arena overflows its reserved VA");
  static_assert(kSlotsPerArena <= kMaxSlotsPerArena,
                "Medium: slot count exceeds header bitmap capacity");
  static_assert((kArenaSize & (kArenaSize - 1)) == 0,
                "Medium arena size must be power of 2");
  // Tripwire: SlabPool's xthread offset encoding requires slot bases
  // to be multiples of kSlotSize. Trips at compile time if the layout
  // is reverted. Future SlabPool-backed classes must satisfy the same
  // invariant on their own ClassLayout.
  static_assert((kSlotRegionOffset & (kSlotSize - 1)) == 0,
                "kSlotRegionOffset must be a multiple of kSlotSize so "
                "SlabPool's xthread offset encoding can recover the slab "
                "base via node & ~(kSlotSize-1)");
};

template <> struct ClassLayout<SubSlotClass::Large> {
  static constexpr size_t kArenaSize = size_t{2} << 20;   // 2 MB
  static constexpr size_t kSlotSize = size_t{128} << 10;  // 128 KB
  static constexpr unsigned kSlotsPerArena = 15;          // 15 × 128 KB = 1920 KB
  static constexpr size_t kHeaderOffset = kGuardBytes;
  static constexpr size_t kSlotRegionOffset = kLeadingBytes;
  static_assert(kSlotRegionOffset + kSlotsPerArena * kSlotSize +
                        kTrailingGuardBytes <=
                    kArenaSize,
                "Large arena overflows its reserved VA");
  static_assert(kSlotsPerArena <= kMaxSlotsPerArena,
                "Large: slot count exceeds header bitmap capacity");
  static_assert((kArenaSize & (kArenaSize - 1)) == 0,
                "Large arena size must be power of 2");
};

template <> struct ClassLayout<SubSlotClass::XLarge> {
  static constexpr size_t kArenaSize = size_t{4} << 20;   // 4 MB
  static constexpr size_t kSlotSize = size_t{256} << 10;  // 256 KB
  static constexpr unsigned kSlotsPerArena = 15;          // 15 × 256 KB = 3840 KB
  static constexpr size_t kHeaderOffset = kGuardBytes;
  static constexpr size_t kSlotRegionOffset = kLeadingBytes;
  static_assert(kSlotRegionOffset + kSlotsPerArena * kSlotSize +
                        kTrailingGuardBytes <=
                    kArenaSize,
                "XLarge arena overflows its reserved VA");
  static_assert(kSlotsPerArena <= kMaxSlotsPerArena,
                "XLarge: slot count exceeds header bitmap capacity");
  static_assert((kArenaSize & (kArenaSize - 1)) == 0,
                "XLarge arena size must be power of 2");
};

template <> struct ClassLayout<SubSlotClass::Huge> {
  static constexpr size_t kArenaSize = size_t{8} << 20;   // 8 MB
  static constexpr size_t kSlotSize = size_t{1} << 20;    // 1 MB
  static constexpr unsigned kSlotsPerArena = 7;           // 7 × 1 MB = 7 MB
  static constexpr size_t kHeaderOffset = kGuardBytes;
  static constexpr size_t kSlotRegionOffset = kLeadingBytes;
  static_assert(kSlotRegionOffset + kSlotsPerArena * kSlotSize +
                        kTrailingGuardBytes <=
                    kArenaSize,
                "Huge arena overflows its reserved VA");
  static_assert(kSlotsPerArena <= kMaxSlotsPerArena,
                "Huge: slot count exceeds header bitmap capacity");
  static_assert((kArenaSize & (kArenaSize - 1)) == 0,
                "Huge arena size must be power of 2");
};

// ArenaHeader must fit comfortably inside its 4 KB committed header page.
// kMaxSlotsPerArena drives both the AtomicBitmap words and the
// SubSlotMeta array — raising it requires reverifying this assertion.
static_assert(sizeof(ArenaHeader) <= kHeaderPageBytes,
              "ArenaHeader outgrew the 4 KB header page budget — reduce "
              "kMaxSlotsPerArena or slim the shadow SubSlotMeta.");

// ============================================================================
// layout_of()
// ============================================================================

SubSlotLayout layout_of(SubSlotClass c) {
  switch (c) {
  case SubSlotClass::Small:
    return {ClassLayout<SubSlotClass::Small>::kArenaSize,
            ClassLayout<SubSlotClass::Small>::kSlotSize,
            ClassLayout<SubSlotClass::Small>::kHeaderOffset,
            ClassLayout<SubSlotClass::Small>::kSlotRegionOffset,
            ClassLayout<SubSlotClass::Small>::kSlotsPerArena,
            SubSlotClass::Small};
  case SubSlotClass::Medium:
    return {ClassLayout<SubSlotClass::Medium>::kArenaSize,
            ClassLayout<SubSlotClass::Medium>::kSlotSize,
            ClassLayout<SubSlotClass::Medium>::kHeaderOffset,
            ClassLayout<SubSlotClass::Medium>::kSlotRegionOffset,
            ClassLayout<SubSlotClass::Medium>::kSlotsPerArena,
            SubSlotClass::Medium};
  case SubSlotClass::Large:
    return {ClassLayout<SubSlotClass::Large>::kArenaSize,
            ClassLayout<SubSlotClass::Large>::kSlotSize,
            ClassLayout<SubSlotClass::Large>::kHeaderOffset,
            ClassLayout<SubSlotClass::Large>::kSlotRegionOffset,
            ClassLayout<SubSlotClass::Large>::kSlotsPerArena,
            SubSlotClass::Large};
  case SubSlotClass::XLarge:
    return {ClassLayout<SubSlotClass::XLarge>::kArenaSize,
            ClassLayout<SubSlotClass::XLarge>::kSlotSize,
            ClassLayout<SubSlotClass::XLarge>::kHeaderOffset,
            ClassLayout<SubSlotClass::XLarge>::kSlotRegionOffset,
            ClassLayout<SubSlotClass::XLarge>::kSlotsPerArena,
            SubSlotClass::XLarge};
  case SubSlotClass::Huge:
    return {ClassLayout<SubSlotClass::Huge>::kArenaSize,
            ClassLayout<SubSlotClass::Huge>::kSlotSize,
            ClassLayout<SubSlotClass::Huge>::kHeaderOffset,
            ClassLayout<SubSlotClass::Huge>::kSlotRegionOffset,
            ClassLayout<SubSlotClass::Huge>::kSlotsPerArena,
            SubSlotClass::Huge};
  default:
    break;
  }
  __builtin_trap();
}

// ============================================================================
// ArenaHeader method definitions
// ============================================================================

uintptr_t ArenaHeader::slot_region_base() const {
  // Per-class offset. Most classes use kLeadingBytes (12 KB = guard +
  // header + guard); SlabPool-backed classes round up to kSlotSize so
  // slot bases satisfy SlabPool's xthread-offset-encoding alignment
  // contract (slab_pool.h xthread_pack / xthread_unpack_node). Switch
  // on class_id rather than calling layout_of so this stays inlinable.
  switch (static_cast<SubSlotClass>(class_id)) {
  case SubSlotClass::Small:
    return arena_base() + ClassLayout<SubSlotClass::Small>::kSlotRegionOffset;
  case SubSlotClass::Medium:
    return arena_base() + ClassLayout<SubSlotClass::Medium>::kSlotRegionOffset;
  case SubSlotClass::Large:
    return arena_base() + ClassLayout<SubSlotClass::Large>::kSlotRegionOffset;
  case SubSlotClass::XLarge:
    return arena_base() + ClassLayout<SubSlotClass::XLarge>::kSlotRegionOffset;
  case SubSlotClass::Huge:
    return arena_base() + ClassLayout<SubSlotClass::Huge>::kSlotRegionOffset;
  case SubSlotClass::Count:
    break;
  }
  __builtin_trap();
}

void *ArenaHeader::slot_base_of(unsigned idx) const {
  LIBC_ASSERT(idx < slots_per_arena &&
              "ArenaHeader::slot_base_of: idx out of range");
  return reinterpret_cast<void *>(slot_region_base() +
                                  static_cast<uintptr_t>(idx) *
                                      static_cast<uintptr_t>(slot_size));
}

// ============================================================================
// Globals
// ============================================================================

VaSubstrate g_substrate;

// `g_mapping_table_ready` lives in memory_primitives_bootstrap.cpp now,
// flipped at the end of the walker's Pass 2 — after every receipt has
// been stamped and the mapping table is fully INIT_READY. Substrate
// reads it via `internal::is_mapping_table_ready()` to decide between
// inline `register_mapping_internal` (post-Tier-A) and deferred-receipt
// stamping (Tier A bring-up). See the declaration in
// memory_primitives_bootstrap.h for the contract and rationale.

// ============================================================================
// substrate_free_arena — Crystalline FreeFn for retired arenas
// ============================================================================
//
// Receives an arena guaranteed by Crystalline's reservation accounting to
// have no live readers. Performs the full teardown sequence in one shot:
//
//   1. mapping_table.remove(base) — drop the LIBC_INTERNAL stamp so a
//      future MAP_FIXED probe / reconcile pass does not mis-route.
//   2. registry.remove_range(base, size) — drop the substrate-membership
//      bitmap so any forged pointer into this VA fails the contains()
//      check at release time.
//   3. all_remove(arena) — unlink from the per-pool all_head_ list so
//      destroy() / fork_reinit() walks no longer see it.
//   4. arena_count_.fetch_sub — keep introspection consistent.
//   5. page_free(base) — MEM_RELEASE the entire reservation.
//
// Permanent arenas never reach this function. They short-circuit
// `try_retire_arena` and the drained-pop branch of
// `pop_abandoned_filtered`; their VA is owned by their respective
// callers (process for seeds, ThreadScratch for the bootstrap-tier
// synthetic header).
//
// Order rationale: mapping table → registry → all_head_ → page_free. The
// table's `remove` is a sentinel CAS on a single L3 entry, robust to
// stale readers. The registry's `remove_range` has its own RW
// semantics. all_head_ uses the per-pool spinlock. page_free is the
// final hand-back to NT.
static void substrate_free_arena(ArenaHeader *a);

// Crystalline retire-publish cadence for the substrate domain.
//
// Crystalline-W's `try_retire` runs Phase A (claim slot per non-anchor
// batch node) then Phase B (publish + refs adjustment). When Phase A's
// scan reaches `last == refs` (no more non-anchor nodes available) it
// returns from the function, bypassing Phase B — so the anchor's refs
// is only adjusted on calls where the batch chain is large enough to
// outlast the eligible-slot scan. With Freq=1, every batch is a single
// node where `last == refs` from the start; Phase B never runs and the
// batch is leaked permanently. Freq must be at least one greater than
// the maximum number of eligible reservation slots seen by Phase A;
// 8 matches `SlabPoolT::slab_retire_domain_` and gives Phase A enough
// chain length under the typical thread counts this libc targets.
//
// Tradeoff: substrate retires are rare (one per arena drain). The first
// Freq-1 retires from a thread sit in its per-thread batch unpublished;
// thread_flush_trampoline publishes the residual at thread exit. For
// long-lived threads with very few arena retires, that residual lingers
// until exit — acceptable, since the VA in question is reserved (not
// committed) and the retired-arena count per thread is bounded.
//
// help_read's per-init_node scan is amortized — the early-out at
// `slow_counter_.load() == 0` makes the common case O(1).
inline constexpr uint32_t kSubstrateRetireFreq = 8;

::LIBC_NAMESPACE::concurrent::CrystallineDomain<ArenaHeader,
                                                &substrate_free_arena,
                                                kSubstrateRetireFreq>
    g_substrate_domain;

} // namespace alloc
} // namespace windows

namespace windows {
namespace alloc {

static void substrate_free_arena(ArenaHeader *a) {
  if (LIBC_UNLIKELY(a == nullptr))
    __builtin_trap();
  // Permanent arenas must never reach Crystalline retire. Defense-in-
  // depth: if one ever does (a future bug in `try_retire_arena`), refuse
  // to free its VA — the OS-side reclaim of seed VA is process exit, and
  // ThreadScratch's bootstrap-tier path owns the bootstrap synth's
  // page_free. Trapping a corrupted retire is the safer failure mode.
  if (LIBC_UNLIKELY(a->is_permanent != 0))
    __builtin_trap();

  // Resolve the owning pool from the class_id stamped at
  // initialize_arena_header time.
  const auto klass = static_cast<SubSlotClass>(a->class_id);
  if (LIBC_UNLIKELY(static_cast<unsigned>(klass) >=
                    static_cast<unsigned>(SubSlotClass::Count)))
    __builtin_trap();

  const SubSlotLayout layout = layout_of(klass);
  const uintptr_t base = a->arena_base();
  const size_t size = layout.arena_size;

  if (::LIBC_NAMESPACE::internal::is_mapping_table_ready())
    g_mapping_table.remove(reinterpret_cast<void *>(base));
  g_substrate.registry().remove_range(base, size);
  // Pool-level bookkeeping (all_remove + arena_count_ decrement) is
  // performed via VaSubstrate's pool dispatcher so substrate_free_arena
  // doesn't need access to ArenaPool's private state. The dispatcher
  // matches the class_id we just validated.
  g_substrate.detach_arena_from_pool(klass, a);
  internal::page_free(reinterpret_cast<void *>(base));
}

// ============================================================================
// Helpers
// ============================================================================

namespace {

// Reserve a power-of-2-aligned VA range. NT's NtAllocateVirtualMemoryEx
// honours MEM_ADDRESS_REQUIREMENTS.Alignment for any power-of-2 value
// ≥ allocation granularity (64 KB); we rely on this so
// `arena_base = ptr & ~(arena_size - 1)` is a single AND on the release
// hot path.
//
// We use plain MEM_RESERVE (no MEM_RESERVE_PLACEHOLDER) so that later
// sub-region commits / decommits are direct NT calls. Placeholder
// semantics would require a split-before-commit dance (NtFree with
// MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER, then commit with
// MEM_REPLACE_PLACEHOLDER) — unnecessary for the substrate's use case
// where guards are just uncommitted VA inside a larger reservation.
void *reserve_aligned(size_t size, size_t alignment) {
  MEM_ADDRESS_REQUIREMENTS reqs = {};
  reqs.Alignment = alignment;

  MEM_EXTENDED_PARAMETER param = {};
  param.Type = MemExtendedParameterAddressRequirements;
  param.Pointer = &reqs;

  PVOID base = nullptr;
  SIZE_T sz = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &sz, MEM_RESERVE, PAGE_NOACCESS, &param, 1);
  if (!NT_SUCCESS(st))
    return nullptr;
  LIBC_ASSERT((reinterpret_cast<uintptr_t>(base) & (alignment - 1)) == 0 &&
              "NT returned a reservation below requested alignment");
  return base;
}

// Try to acquire a clear bit in the arena's occupancy bitmap via a
// tzcnt/blsr walk over the first `words` words. Returns a valid index
// on success, or -1 if every candidate bit lost its try_acquire race
// (caller retries). RELAXED load matches AtomicBitmap's RMW ordering.
int try_claim_free_slot(ArenaHeader *a, unsigned slot_count) {
  const size_t words =
      internal::alloc_primitives::occupancy_words(slot_count);
  for (size_t w = 0; w < words; ++w) {
    uint64_t bits =
        ~a->occupancy
             .word_at<cpp::MemoryOrder::RELAXED>(w);
    // Mask off bits past the logical slot count so we never claim an
    // out-of-range index in the last word.
    if (w == words - 1) {
      const unsigned valid_in_last = slot_count - static_cast<unsigned>(w) * 64U;
      if (valid_in_last < 64U) {
        const uint64_t last_mask =
            (uint64_t{1} << valid_in_last) - 1ULL;
        bits &= last_mask;
      }
    }
    while (bits) {
      const unsigned bit =
          static_cast<unsigned>(__builtin_ctzll(bits));
      const size_t idx = w * 64U + bit;
      if (a->occupancy.try_acquire(idx))
        return static_cast<int>(idx);
      bits &= bits - 1; // blsr — move to next candidate
    }
  }
  return -1;
}

// Spinlock helpers for the cold all_slabs-style list. Matches the
// SlabPool pattern (slab_pool.h:940-965). Used only on arena create/
// release (reservation rate), never on acquire/release hot paths.
//
// TODO(futex-backing): If/when WaitSlot init is formalised as a Phase
// 0a-pre dependency of the substrate, this can swap to a Futex-backed
// mutex for less CPU-burn on contention. Precondition: WaitSlot has
// its own CommitRegion (wait_slot.cpp:45), so the cycle is structural
// ly absent — just needs the bootstrap ordering to be declared and a
// two-line swap here. Not urgent: contention on this lock is near-
// zero (arena create/retire only).
void all_lock_take(cpp::Atomic<int> &lock) {
  int expected = 0;
  if (LIBC_LIKELY(lock.compare_exchange_weak(expected, 1,
                                             cpp::MemoryOrder::ACQUIRE,
                                             cpp::MemoryOrder::RELAXED)))
    return;
  for (;;) {
    spin_wait::spin_on_raw(&lock.val, 1);
    if (lock.load(cpp::MemoryOrder::RELAXED) != 0)
      continue;
    expected = 0;
    if (lock.compare_exchange_weak(expected, 1,
                                   cpp::MemoryOrder::ACQUIRE,
                                   cpp::MemoryOrder::RELAXED))
      return;
  }
}

void all_lock_release(cpp::Atomic<int> &lock) {
  lock.store(0, cpp::MemoryOrder::RELEASE);
}

} // namespace

// ============================================================================
// ArenaPool — state transitions
// ============================================================================

void ArenaPool::configure_class_only(SubSlotClass c) {
  // Record per-class geometry without touching VA. The first acquire()
  // from this pool lazily reserves an arena via slow_acquire_arena.
  layout_ = layout_of(c);
}

// ---- Acquire ----------------------------------------------------------------

SubSlotHandle ArenaPool::acquire(SubstrateRegistry &registry, ConsumerTag tag,
                                  ArenaHeader *affinity_hint) {
  return acquire_impl(registry, /*commit_slot=*/true, tag, affinity_hint);
}

SubSlotHandle ArenaPool::acquire_uncommitted(SubstrateRegistry &registry,
                                              ConsumerTag tag,
                                              ArenaHeader *affinity_hint) {
  return acquire_impl(registry, /*commit_slot=*/false, tag, affinity_hint);
}

ArenaPool::SlotClaim ArenaPool::claim_slot_in_arena(ArenaHeader *a) {
  // [A2] Reserve one slot via live_count CAS. Rejects kQuarantineSentinel
  // and full arenas in a single RMW.
  uint32_t n = a->live_count.load(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    if (n == kQuarantineSentinel ||
        n >= static_cast<uint32_t>(a->slots_per_arena))
      return {nullptr, 0, 0};
    if (a->live_count.compare_exchange_weak(n, n + 1,
                                             cpp::MemoryOrder::ACQ_REL,
                                             cpp::MemoryOrder::ACQUIRE))
      break;
  }

  // [A3] Claim a specific slot in the occupancy bitmap. Rare-path
  // failure (bitmap momentarily shows no free slot despite the
  // reservation) rolls back the reservation; caller retries.
  const int idx = try_claim_free_slot(a, a->slots_per_arena);
  if (idx < 0) {
    a->live_count.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
    return {nullptr, 0, 0};
  }

  // [A4] Stamp shadow metadata BEFORE any subsequent commit. The slot
  // is not reachable by any other release() call (consumer doesn't have
  // the handle yet), so plain stores are safe. The acquire_seq bump
  // is single-writer here (the bitmap-claim CAS at [A3] fenced off
  // other claimants), so a RELAXED RMW is sufficient.
  void *slot_base = a->slot_base_of(static_cast<unsigned>(idx));
  const uintptr_t token_key = g_pcb.zone0.substrate_token_key();
  const uintptr_t canary =
      internal::alloc_primitives::derive_canary(
          a->arena_secret, slot_base,
          static_cast<size_t>(a->slot_size));
  // Bump the per-slot acquire counter and fold it into the token's
  // authenticity hash so each acquisition of the same slot mints a
  // distinct token, defeating same-slot replay by a buggy consumer
  // that retains a previously-released token. Wrap-around at 2^32 is
  // not a concern in practice (~4 billion acquisitions per slot).
  const uint32_t seq =
      a->slot_meta[idx].acquire_seq.fetch_add(1,
                                               cpp::MemoryOrder::RELAXED) +
      1;
  // Token layout: class_id in top 3 bits, XOR-authenticity in low 61.
  // 3 bits fits 8 classes (Count=5 today, headroom for 3 more); 61 bits
  // of XOR-mixed authenticity (secret ^ slot_base ^ generation ^ seq)
  // remain infeasible to forge without the Zone-0 secret.
  constexpr uint64_t kAuthMask = (uint64_t{1} << 61) - 1ULL;
  const uint64_t auth =
      static_cast<uint64_t>(token_key ^
                            reinterpret_cast<uintptr_t>(slot_base) ^
                            static_cast<uintptr_t>(a->generation) ^
                            static_cast<uintptr_t>(seq)) &
      kAuthMask;
  const uint64_t token =
      (static_cast<uint64_t>(layout_.klass) << 61) | auth;
  a->slot_meta[idx].canary.store(static_cast<uint64_t>(canary),
                                 cpp::MemoryOrder::RELAXED);
  a->slot_meta[idx].owner_token.store(token, cpp::MemoryOrder::RELAXED);

  return {slot_base, token, static_cast<unsigned>(idx)};
}

void ArenaPool::claim_rollback(ArenaHeader *a, unsigned idx) {
  // Reverse the [A4] meta stamp, [A3] bitmap mark, and [A2] live_count
  // increment in that order so a concurrent retire CAS that observes
  // live_count == 0 cannot fire while our shadow metadata is still set.
  a->slot_meta[idx].owner_token.store(0, cpp::MemoryOrder::RELAXED);
  a->slot_meta[idx].canary.store(0, cpp::MemoryOrder::RELAXED);
  a->occupancy.mark_dead(static_cast<size_t>(idx));
  a->live_count.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
}

SubSlotHandle ArenaPool::acquire_impl(SubstrateRegistry &registry,
                                      bool commit_slot, ConsumerTag tag,
                                      ArenaHeader *affinity_hint) {
  // ThreadScratch is no longer a substrate consumer — its per-thread
  // scratch arenas come straight from `page_reserve`, not from any
  // pool here. The substrate-bootstrap CrystallineThreadRegion swap-in
  // that used to live at the top of this function (covering the
  // chicken-and-egg case where a new thread's first
  // `substrate_acquire_uncommitted` call needed Crystalline state
  // before its own scratch existed) is therefore unreachable: every
  // thread that reaches this function has already either created its
  // own scratch or never needed one. No guard is required.

  // ---- Affinity fast path ------------------------------------------------
  //
  // Four relaxed loads on the same cache line validate the hint:
  //   1. consumer_tag matches the requested tag (rejects cross-consumer
  //      affinity AND the _Seed sentinel — seed arenas are tagged _Seed,
  //      so a SlabPool hint into a seed arena trips check 1)
  //   2. class_id matches this pool's class (cross-class hint rejected)
  //   3. live_count is not the quarantine sentinel
  //   4. is_permanent == 0 (seed arenas + bootstrap synth are excluded
  //      from affinity even when their tag and class would otherwise
  //      match — substrate-served arenas only). Without this check,
  //      affinity correctness would silently depend on the invariant
  //      that every permanent arena is either tagged `_Seed` or has
  //      live_count==slots_per_arena. Cheap to enforce explicitly.
  // Any failure: silently fall through to the active_ loop.
  if (affinity_hint != nullptr) {
    const uint8_t hint_tag =
        affinity_hint->consumer_tag;
    const uint16_t hint_class =
        affinity_hint->class_id;
    const uint32_t hint_live =
        affinity_hint->live_count.load(cpp::MemoryOrder::RELAXED);
    const uint8_t hint_permanent = affinity_hint->is_permanent;
    if (hint_tag == static_cast<uint8_t>(tag) &&
        hint_class == static_cast<uint16_t>(layout_.klass) &&
        hint_live != kQuarantineSentinel && hint_permanent == 0) {
      SlotClaim claim = claim_slot_in_arena(affinity_hint);
      if (claim.slot_base != nullptr) {
        if (commit_slot) {
          if (LIBC_UNLIKELY(!internal::page_commit(
                  claim.slot_base,
                  static_cast<size_t>(affinity_hint->slot_size)))) {
            claim_rollback(affinity_hint, claim.idx);
            // Commit failure on the hint is unrecoverable for this
            // request; do NOT fall through (a transient OOM / paging
            // problem will repeat against active_ too).
            return {};
          }
        }
        return SubSlotHandle{claim.slot_base, claim.token};
      }
      // claim failed (full / quarantined / bitmap race) — fall through.
    }
  }

  // ---- Active_-loop path -------------------------------------------------

  for (;;) {
    // Crystalline pin: read(active_, idx=0, nullptr) returns the current
    // active pointer while holding a reservation that defers reclamation
    // for as long as the thread's slot 0 keeps a snapshot of `a`.
    // `active_` is guaranteed to hold either nullptr or a real
    // ArenaHeader* (no sentinel) — Crystalline's slow_path may
    // dereference the loaded value during the helping protocol's epoch
    // reconciliation.
    ArenaHeader *a = g_substrate_domain.protect(active_, /*index=*/0,
                                                /*parent=*/nullptr);
    if (!a) {
      a = slow_acquire_arena(registry, tag);
      if (!a)
        return {};
    }

    SlotClaim claim = claim_slot_in_arena(a);
    if (claim.slot_base == nullptr) {
      // Distinguish "full/quarantined" (demote+swap) from "bitmap race"
      // (retry same active_). Re-read live_count: if at sentinel or full,
      // demote. Otherwise the helper rolled back live_count already, so
      // we just continue and the next iteration re-reads active_.
      const uint32_t n = a->live_count.load(cpp::MemoryOrder::RELAXED);
      if (n == kQuarantineSentinel ||
          n >= static_cast<uint32_t>(a->slots_per_arena)) {
        ArenaHeader *candidate = pop_abandoned_filtered();
        if (candidate) {
          install_as_active(candidate);
        } else {
          ArenaHeader *expected = a;
          if (active_.compare_exchange_strong(expected, nullptr,
                                              cpp::MemoryOrder::ACQ_REL,
                                              cpp::MemoryOrder::ACQUIRE))
            push_abandoned(a);
        }
      }
      continue;
    }

    // [A5] Commit — single NT syscall. Full kernel fence.
    //
    // Skipped for the uncommitted variant: the slot stays as reserved,
    // PAGE_NOACCESS VA. The caller is responsible for `page_commit`
    // before any read or write. Release's `page_decommit(slot_size)` is
    // idempotent on uncommitted ranges, so the cleanup path is unchanged
    // regardless of how the caller carved up the slot.
    if (commit_slot) {
      if (LIBC_UNLIKELY(!internal::page_commit(
              claim.slot_base, static_cast<size_t>(a->slot_size)))) {
        // Commit failure is rare (OOM / paging) but recoverable. Roll
        // back every effect in reverse order.
        claim_rollback(a, claim.idx);
        return {};
      }
    }

    return SubSlotHandle{claim.slot_base, claim.token};
  }
}

// ---- Release ----------------------------------------------------------------
//
// LOAD-BEARING INVARIANT — read before editing this function.
//
// Release runs WITHOUT a Crystalline pin. The arena `a` cannot be retired
// while we touch its fields because the caller holds the SubSlotHandle —
// which represents an outstanding slot allocation — and that slot's
// contribution to `a->live_count` is still counted. Concretely:
//
//   - `try_retire_arena` is the ONLY path that drives `a` into the
//     retired state, and it does so with a CAS `live_count: 0 →
//     kQuarantineSentinel`. The CAS only succeeds if live_count is
//     exactly 0 at the moment of the swap.
//   - The handle we are about to release contributes +1 to live_count
//     (stamped at acquire-time by the N → N+1 CAS). Until our
//     `live_count.fetch_sub` runs at [R8], live_count is at minimum 1
//     — so try_retire_arena's 0 → SENTINEL CAS can never succeed
//     concurrent with our work here.
//   - `g_substrate_domain.retire(a)` is invoked only from
//     try_retire_arena, which only runs after the SENTINEL transition
//     succeeds. Therefore Crystalline cannot put `a` on a retire batch
//     while we hold the handle.
//   - `substrate_free_arena(a)` runs only from Crystalline's free_list
//     path, which fires only after `g_substrate_domain.retire(a)` has
//     been called. Therefore `page_free(arena_base)` cannot run before
//     our [R8] decrement.
//
// After [R8] (`live_count.fetch_sub`), arena-derived dereferences are
// only safe under one of two conditions:
//
//   (a) We hold a Crystalline pin spanning the access; OR
//   (b) We can prove `a` is unreachable via every pool list (active_
//       and abandoned_head_) at the moment of the access. With `a` off
//       both lists, no peer can find it to feed it to try_retire_arena
//       (which is the only retire driver, and it runs only on arenas
//       popped from abandoned_head_).
//
// The demotion path at [R9] uses (b): a successful
// `active_.compare_exchange(a, nullptr)` proves `a` was on active_ at
// the moment of the swap, hence not on abandoned_head_ at that moment
// — and the swap simultaneously removes it from active_, leaving a
// brief window where `a` is on neither list. push_abandoned consumes
// this window by writing `a->next_abandoned` and splicing onto
// abandoned_head_; once published, `a` is reachable again, but by then
// our write is complete.
//
// If you add any arena-derived dereference AFTER [R8], you break this
// invariant — provide explicit unreachability proof (as [R9] does), or
// restore a Crystalline pin.

void ArenaPool::release(SubSlotHandle handle) {
  // [R1] O(1) membership check against the substrate registry. Executed
  // BEFORE any dereference derived from handle.ptr. Three atomic loads.
  const uintptr_t p = reinterpret_cast<uintptr_t>(handle.ptr());
  if (LIBC_UNLIKELY(p == 0))
    __builtin_trap();
  const uintptr_t arena_base = p & ~(layout_.arena_size - 1);
  if (LIBC_UNLIKELY(!g_substrate.registry().contains(arena_base)))
    __builtin_trap();

  ArenaHeader *a = reinterpret_cast<ArenaHeader *>(arena_base +
                                                   layout_.header_offset);

  // [R2] Authenticity tag — unforgeable without the Zone-0-sealed
  // substrate_secret. Catches forged pointers, retired-then-reused
  // arenas, and arena header corruption.
  const uintptr_t secret = g_pcb.zone0.substrate_secret();
  const uintptr_t expected_tag =
      secret ^ arena_base ^ static_cast<uintptr_t>(a->generation);
  if (LIBC_UNLIKELY(a->arena_tag != expected_tag))
    __builtin_trap();

  // [R3] Bounds-check the slot index against the authenticated header.
  const uintptr_t slot_region = arena_base + layout_.slot_region_offset;
  if (LIBC_UNLIKELY(p < slot_region))
    __builtin_trap();
  const uintptr_t slot_off = p - slot_region;
  if (LIBC_UNLIKELY(slot_off % a->slot_size != 0))
    __builtin_trap();
  const unsigned idx =
      static_cast<unsigned>(slot_off / a->slot_size);
  if (LIBC_UNLIKELY(idx >= a->slots_per_arena))
    __builtin_trap();

  // [R4] Canary verify — a subslot-UAF write that reached the shadow
  // metadata corrupts this.
  const uintptr_t expected_canary =
      internal::alloc_primitives::derive_canary(
          a->arena_secret, handle.ptr(),
          static_cast<size_t>(a->slot_size));
  if (LIBC_UNLIKELY(a->slot_meta[idx].canary.load(
                        cpp::MemoryOrder::ACQUIRE) !=
                    static_cast<uint64_t>(expected_canary)))
    __builtin_trap();

  // [R5] Owner-token atomic consume. Check the embedded class_id first
  // (cheap — bit shift on the handle) so a mis-routed release traps
  // before the CAS; the CAS then atomically consumes the full 64-bit
  // token so concurrent double-releases resolve with exactly one winner.
  // Class field is in bits [63:61] (3 bits); mask explicitly so the
  // upper bit semantics are obvious to the reader.
  const unsigned handle_class =
      static_cast<unsigned>(handle.token() >> 61) & 0x7u;
  if (LIBC_UNLIKELY(handle_class != static_cast<unsigned>(layout_.klass)))
    __builtin_trap();
  uint64_t expected_tok = handle.token();
  if (LIBC_UNLIKELY(!a->slot_meta[idx].owner_token.compare_exchange_strong(
          expected_tok, uint64_t{0}, cpp::MemoryOrder::ACQ_REL,
          cpp::MemoryOrder::RELAXED)))
    __builtin_trap();
  a->slot_meta[idx].canary.store(0, cpp::MemoryOrder::RELAXED);

  // [R6] Decommit subslot — physical pages returned; VA stays reserved.
  // Any lingering reader on the slot now faults into the master VEH.
  (void)internal::page_decommit(handle.ptr(),
                                static_cast<size_t>(a->slot_size));

  // [R7] Mark subslot dead in the bitmap. trap_on_collision=true turns
  // a second mark_dead into a trap at zero extra cost.
  a->occupancy.mark_dead(static_cast<size_t>(idx));

  // Snapshot `active_` BEFORE [R8] so the demotion path below can tell
  // whether `a` was on active_ at the moment we still held a slot. The
  // load runs while live_count is still ≥ 1, so `a` is guaranteed alive.
  // If a peer install_as_active fires between this load and our CAS at
  // [R9], the CAS fails harmlessly and the demotion is skipped.
  ArenaHeader *active_at_release =
      active_.load(cpp::MemoryOrder::ACQUIRE);
  bool was_active = (active_at_release == a);

  // [R8] Decrement live_count.
  uint32_t prev_live = a->live_count.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);

  // [R9] Demotion of drained-while-active arenas. Without this, a pool
  // that stabilises on a single arena cycling full→drained→full never
  // releases its 2 MiB: the drained arena stays on active_ and the next
  // acquire re-feeds it via the 0 → 1 CAS in `acquire`. Trades a
  // slightly higher reacquire cost (next acquire takes the slow path
  // through pop_abandoned_filtered or reserve_new_arena) for actual VA
  // shrinkage on long-lived pools.
  //
  // Safety: see the load-bearing invariant block at the top of this
  // function. `was_active` is computed pre-[R8] so it observes a state
  // where retirement is impossible. The CAS active_→nullptr is the
  // unreachability witness — if it succeeds, `a` was on active_ and is
  // now on neither list, so no peer can find and retire it during the
  // push_abandoned that follows. The pushed arena is retired by the
  // next pop_abandoned_filtered observing live_count == 0.
  if (prev_live == 1 && was_active) {
    ArenaHeader *expected = a;
    if (active_.compare_exchange_strong(expected, nullptr,
                                         cpp::MemoryOrder::ACQ_REL,
                                         cpp::MemoryOrder::ACQUIRE)) {
      push_abandoned(a);
    }
    // CAS failure: a peer's install_as_active raced ahead of us and put
    // `a` on abandoned itself. Nothing more for us to do — the next
    // pop_abandoned_filtered will retire it.
  }
}

// ---- Retire (called only from pop_abandoned_filtered) ----------------------
//
// An arena can be retired only after it has been popped off abandoned_head_
// — that guarantees no list entry still references it when Crystalline
// later runs `substrate_free_arena(a)`. The caller (pop_abandoned_filtered)
// holds the sole reference to `a` by virtue of winning the CAS-pop.
//
// The post-CAS handoff to `g_substrate_domain.retire(a)` transfers
// ownership to Crystalline's helping protocol. Crystalline's `retire`
// stamps the anchor's birth_era, links the node onto the calling
// thread's batch chain, and (every Freq=1 retires) immediately publishes
// the batch across live thread slots. The eventual `substrate_free_arena`
// callback fires once every slot publication has been drained by reader
// do_updates — at which point no thread holds a reservation pinning `a`.

void ArenaPool::try_retire_arena(ArenaHeader *a) {
  // Permanent arenas are never retired — their VA outlives the substrate.
  // The only reachable caller (`pop_abandoned_filtered`) already filters
  // permanent arenas out before getting here, but a stray future caller
  // (or a defensive code path) must not be able to release a permanent
  // arena's VA. Push back onto abandoned so the next slow-path can find
  // it: dropping it on the floor would orphan a live arena. Crystalline
  // is also kept out — passing a permanent node to retire would leave
  // it on the batch chain forever (free_list never fires for nodes
  // whose anchor refs never wraps to zero).
  if (LIBC_UNLIKELY(a->is_permanent != 0)) {
    push_abandoned(a);
    return;
  }

  // [T1] 0 → kQuarantineSentinel. No concurrent acquirer can reach `a`
  // here — it is off every pool-wide list — so the CAS is uncontested in
  // the common case. A lost CAS means the arena's live_count was
  // non-zero at pop time (caller should have checked); defensively
  // re-publish to abandoned so no allocated slot is stranded.
  uint32_t expected = 0;
  if (!a->live_count.compare_exchange_strong(expected, kQuarantineSentinel,
                                             cpp::MemoryOrder::ACQ_REL,
                                             cpp::MemoryOrder::ACQUIRE)) {
    push_abandoned(a);
    return;
  }

  // [T2] Hand to Crystalline. The domain's `retire` does NOT block — it
  // appends to the calling thread's per-domain batch and (every Freq-th
  // call) attempts to publish the batch across live thread slots. Final
  // reclamation (substrate_free_arena) runs asynchronously when no
  // thread holds a reservation pinning `a`. We do NOT touch `a` after
  // this call returns — Crystalline owns it.
  g_substrate_domain.retire(a);
}

// ---- Abandoned-stack (lock-free Treiber, exchange-pop-all consumer) --------
//
// Producer side: standard Treiber CAS push (ABA-immune by construction —
// CAS only checks the head field; a successful CAS publishes a valid
// stack regardless of intermediate states).
//
// Consumer side: ONE atomic SWAP claims the entire chain into a
// thread-local snapshot. ABA is structurally absent — there is no
// compare, no stale read window. Side benefits over a per-element
// CAS-pop loop:
//   * No Crystalline `read(abandoned_head_, ...)` pin on the hot path.
//     Once the chain is in our local view, no other thread holds a
//     reference; UAF is impossible because `try_retire_arena` is only
//     invoked from this function and we own every arena on the chain.
//   * Per-element work (state classify, retire-if-drained, full-defer,
//     pick-the-first-eligible) operates on plain pointers — no atomics
//     beyond `live_count.load`.
//   * Re-push of the unexamined remainder + deferred-full skipped
//     arenas is a single `push_abandoned_chain` (one CAS-loop), not N
//     pushes that would each become the new head and force a re-pop.
//
// Cost shape vs. the prior CAS-per-pop:
//   * Hot path (chain non-empty): one SWAP + linear walk of the FULL
//     chain (every node classified once), then one push_abandoned_chain
//     to re-publish everything except the chosen result. With chain
//     length L, total atomics = 2 + R, where R is the number of drained
//     non-permanent arenas encountered (each does a single CAS in
//     try_retire_arena).
//   * Empty path: one SWAP returning nullptr.
//
// Why walk the full chain (not break at first eligible): the prior
// "break at first eligible" optimization was an acquire-fast-path
// micro-win that left drained-non-permanent arenas perpetually
// unreclaimed when partial arenas sat ahead of them in the chain. The
// failure mode: arenas are pushed full at displace time, drain to 0
// after release, but the next pop walk hits a partial first and breaks
// before reaching them — and re-push preserves the chain order, so
// drained arenas slide deeper while new partials get pushed at the
// head. With a contaminated pool (any prior workload that left
// partials), reclamation could stall indefinitely with arena_count
// growing unboundedly.
//
// Walking the full chain costs O(L) per pop_abandoned_filtered call,
// but the slow path only fires when an active arena fills (rare per
// acquire, rarer still per release). Trading microseconds on the
// already-rare slow path for a structurally complete reclamation
// invariant is the right tradeoff: drained-non-permanent arenas now
// retire within the very next pop after they enter the chain, with no
// extra cleanup pass and no separate drain API.
//
// Worst-case the consumer briefly hides every abandoned arena from
// concurrent contenders, but the substrate's slow_acquire_arena retry
// loop already tolerates a transiently-empty abandoned chain by
// falling through to fresh-arena reservation; consumer fairness across
// threads is a non-goal here.
ArenaHeader *ArenaPool::pop_abandoned_filtered() {
  ArenaHeader *chain =
      abandoned_head_.exchange(nullptr, cpp::MemoryOrder::ACQ_REL);
  if (chain == nullptr) {
    return nullptr;
  }

  ArenaHeader *result = nullptr;
  ArenaHeader *defer = nullptr;  // full arenas (re-pushed at end)
  ArenaHeader *extras = nullptr; // additional eligibles past the result

  while (chain != nullptr) {
    ArenaHeader *next = chain->next_abandoned;
    chain->next_abandoned = nullptr;

    const uint32_t state =
        chain->live_count.load(cpp::MemoryOrder::ACQUIRE);

    if (state == kQuarantineSentinel) {
      // Already retired (or in flight). Drop from chain — its
      // next_abandoned is already nullptr above, so it's not re-pushed
      // and Crystalline's free path won't AV on a stale chain link.
      chain = next;
      continue;
    }

    if (state == 0) {
      if (chain->is_permanent) {
        // Re-promotable seed/bootstrap arena — first one becomes the
        // returned result; further permanents go to extras for re-push.
        if (result == nullptr) {
          result = chain;
        } else {
          chain->next_abandoned = extras;
          extras = chain;
        }
      } else {
        // Drained substrate-managed arena — retire here. We own the
        // arena via the local chain; no concurrent reader can be
        // pinning it (it's off active_ by virtue of being on
        // abandoned). try_retire_arena's CAS 0→kQuarantineSentinel +
        // Crystalline hand-off is uncontested.
        try_retire_arena(chain);
      }
      chain = next;
      continue;
    }

    if (state >= static_cast<uint32_t>(chain->slots_per_arena)) {
      // Full arena — can't satisfy an acquire right now, but its
      // remaining slots will drain via release() and a future pop will
      // pick it up.
      chain->next_abandoned = defer;
      defer = chain;
      chain = next;
      continue;
    }

    // Partial — eligible for re-promotion. First wins; rest go to
    // extras for re-push so they remain available for future pops.
    if (result == nullptr) {
      result = chain;
#ifdef LIBC_SUBSTRATE_DIAG_COUNTERS
      g_diag_pop_returned_partial.fetch_add(1, cpp::MemoryOrder::RELAXED);
#endif
    } else {
      chain->next_abandoned = extras;
      extras = chain;
    }
    chain = next;
  }

  // Re-push extras + deferred-full as a single chain. Both lists are
  // entirely local (no concurrent producer is touching either), so the
  // splice is a plain pointer write; the actual abandoned_head_ store
  // is a single push_abandoned_chain CAS-loop.
  if (extras != nullptr || defer != nullptr) {
    if (extras == nullptr) {
      push_abandoned_chain(defer);
    } else {
      ArenaHeader *tail = extras;
      while (tail->next_abandoned != nullptr)
        tail = tail->next_abandoned;
      tail->next_abandoned = defer;
      push_abandoned_chain(extras);
    }
  }

  return result;
}

void ArenaPool::push_abandoned(ArenaHeader *a) {
  ArenaHeader *old = abandoned_head_.load(cpp::MemoryOrder::RELAXED);
  do {
    a->next_abandoned = old;
  } while (!abandoned_head_.compare_exchange_weak(
      old, a, cpp::MemoryOrder::RELEASE, cpp::MemoryOrder::RELAXED));
}

// Push an already-linked chain [head .. tail] onto abandoned_head_ in
// one CAS-loop. Caller has built the chain locally (e.g. the deferred-
// full list inside pop_abandoned_filtered or the unexamined remainder),
// so the chain's interior next_abandoned links are stable and only the
// tail's link needs to splice onto whatever the current head is.
void ArenaPool::push_abandoned_chain(ArenaHeader *head) {
  if (head == nullptr)
    return;
  ArenaHeader *tail = head;
  while (tail->next_abandoned != nullptr)
    tail = tail->next_abandoned;
  ArenaHeader *old = abandoned_head_.load(cpp::MemoryOrder::RELAXED);
  do {
    tail->next_abandoned = old;
  } while (!abandoned_head_.compare_exchange_weak(
      old, head, cpp::MemoryOrder::RELEASE, cpp::MemoryOrder::RELAXED));
}

void ArenaPool::install_as_active(ArenaHeader *a) {
  // Replace active_ with `a`. The previous active (if non-null) goes on
  // the abandoned stack so a later slow-path acquire can still pick it
  // up. With the reserving_ flag pattern, active_ never holds a
  // sentinel — it's always nullptr or a real ArenaHeader*.
  ArenaHeader *old = active_.exchange(a, cpp::MemoryOrder::ACQ_REL);
  if (old)
    push_abandoned(old);
}

// ---- Slow-path acquire (reservation election) -------------------------------

ArenaHeader *ArenaPool::slow_acquire_arena(SubstrateRegistry &registry,
                                            ConsumerTag tag) {
  // Retry loop: a contender may observe `active_ == nullptr` after the
  // reserver stored null (reservation failed) OR after another thread
  // demoted a full arena via `active_.compare_exchange(a, nullptr)` in
  // ArenaPool::acquire. In the demote case the pool is not exhausted —
  // another reservation will succeed — so we restart the election rather
  // than propagate null to the caller and trip the ensure_slot trap.
  for (;;) {
    // First preference: an abandoned arena with free slots.
    ArenaHeader *candidate = pop_abandoned_filtered();
    if (candidate) {
      install_as_active(candidate);
      return candidate;
    }

    // Otherwise, elect one thread via the reserving_ flag. The election
    // is a `false → true` CAS; losers spin on the flag until the winner
    // clears it. We deliberately keep `active_` clean of any sentinel —
    // Crystalline's read(active_, ...) may dereference the loaded value
    // in its slow_path, so storing a non-canonical sentinel into active_
    // would AV. The flag is on a separate cache line in ArenaPool's
    // layout so contention on it does not pollute the active_ hot line.
    bool expected = false;
    if (reserving_.compare_exchange_strong(expected, true,
                                           cpp::MemoryOrder::ACQ_REL,
                                           cpp::MemoryOrder::ACQUIRE)) {
      ArenaHeader *fresh = reserve_new_arena(registry, tag);
      // Publish BEFORE the mapping-table stamp: `register_mapping_internal`
      // may re-enter `ArenaPool::acquire` (via `ensure_slot` on the
      // mapping table's internal pools), and that re-entry must find a
      // slot in `fresh` rather than spin in slow_acquire_arena's wait
      // loop. The reserving_ flag stays held across the table stamp so a
      // second contender does not start a duplicate reservation.
      //
      // Atomic publish via exchange. While reserving_ is held no other
      // thread starts a fresh reservation, but a contender that entered
      // slow_acquire_arena BEFORE seeing reserving_=true can still pop an
      // abandoned arena and `install_as_active` it during our reservation
      // window. A plain `active_.store(fresh)` would silently overwrite
      // that install and orphan the candidate (off `active_`, off
      // `abandoned_head_`, and never re-promotable since pop_abandoned is
      // the sole `try_retire_arena` driver). The exchange returns whatever
      // the contender installed; we re-publish it onto `abandoned_head_`
      // so a future pop finds it. If reservation failed (fresh==nullptr)
      // we leave `active_` alone — overwriting with null would wipe the
      // contender's install just as badly.
      if (fresh) {
        ArenaHeader *displaced =
            active_.exchange(fresh, cpp::MemoryOrder::ACQ_REL);
        if (displaced)
          push_abandoned(displaced);
        // Double-checked dispatch: `g_mapping_table_ready` is monotonic
        // (false → true, never reverses within a process lifetime). The
        // lock-free outer check is the post-Tier-A hot path — every fresh
        // arena reservation past mapping-table bring-up takes ZERO locks
        // here. The locked branch is reached only during Tier A's narrow
        // bring-up window (substrate seeds, ensure_init's L3 page allocs,
        // Pass 2-driven recursive substrate reserves), where it pairs
        // with the walker's `mapping_table_finalize_init` (which holds
        // the same lock across [latch ready=true + drain]) to guarantee
        // every pre-INIT_READY arena ends up registered.
        //
        // Soundness of the outer check: a thread that observes
        // is_mapping_table_ready() == true via this ACQUIRE load is past
        // the RELEASE store in `mark_mapping_table_ready` — `ready=true`
        // is observable for the rest of the process. No serialization is
        // needed; an inline stamp races only with other inline stamps on
        // distinct VAs, and `register_sentinel_live`'s slot-CAS is the
        // serialization point for the table itself.
        //
        // register_mapping_internal runs OUTSIDE the lock either way, so
        // its potential transitive substrate re-entry (via ensure_slot)
        // can take the same lock without deadlock.
        const uintptr_t base = fresh->arena_base();
        if (LIBC_LIKELY(::LIBC_NAMESPACE::internal::is_mapping_table_ready())) {
          if (LIBC_UNLIKELY(!g_mapping_table.register_mapping_internal(
                  reinterpret_cast<void *>(base), layout_.arena_size)))
            __builtin_trap();
        } else {
          // Bring-up window. Take the lock and recheck — the walker's
          // finalize may have latched ready=true between our outer load
          // and the lock acquire, in which case we fall through to the
          // inline-stamp branch.
          ::LIBC_NAMESPACE::internal::scratch_detail::
              pending_internal_receipts_lock();
          const bool ready_now =
              ::LIBC_NAMESPACE::internal::is_mapping_table_ready();
          if (!ready_now) {
            ::LIBC_NAMESPACE::internal::scratch_detail::
                enqueue_pending_internal_receipt_locked(
                    reinterpret_cast<void *>(base), layout_.arena_size,
                    ::LIBC_NAMESPACE::internal::InternalKind::SubstrateArena);
          }
          ::LIBC_NAMESPACE::internal::scratch_detail::
              pending_internal_receipts_unlock();
          if (ready_now) {
            if (LIBC_UNLIKELY(!g_mapping_table.register_mapping_internal(
                    reinterpret_cast<void *>(base), layout_.arena_size)))
              __builtin_trap();
          }
        }
      }
      reserving_.store(false, cpp::MemoryOrder::RELEASE);
      return fresh;
    }

    // Contender: wait for the reservation in flight to complete.
    //
    // TODO(futex-backing): swap spin_until_changed for
    // `futex_addr::wait(&reserving_, true)` once Futex is a declared
    // substrate dependency. The reservation takes ~1 µs (single
    // NtAllocateVirtualMemoryEx), which fits inside spin_until_changed's
    // UMWAIT window on capable CPUs, so spinning is cheap today. Under
    // high-thread-count bursts where many threads hit the flag together,
    // a futex wait would stop the herd from burning cycles. The matching
    // notify site is `reserving_.store(false, RELEASE)` above (the
    // reserver's release), which would add
    // `futex_addr::wake(&reserving_, UINT32_MAX)` after the store.
    for (;;) {
      if (!reserving_.load(cpp::MemoryOrder::ACQUIRE)) {
        ArenaHeader *a = active_.load(cpp::MemoryOrder::ACQUIRE);
        if (a != nullptr)
          return a;
        // Observed reserver done but active_ still null: the reserver
        // failed OR another consumer demoted a full arena. Restart the
        // outer election — don't propagate null.
        break;
      }
      // reserving_ is `cpp::Atomic<bool>` whose `.val` is plain bool —
      // 1 byte. spin_on_raw's underlying UMONITOR/MWAITX monitors the
      // cache line containing this byte, waking the moment the reserver
      // stores false. Reinterpret as uint8_t for the size-tagged spin
      // instantiation.
      spin_wait::spin_on_raw(
          reinterpret_cast<uint8_t *>(&reserving_.val), uint8_t{1});
    }
  }
}

// ---- Fresh arena reservation ------------------------------------------------

ArenaHeader *ArenaPool::reserve_new_arena(SubstrateRegistry &registry,
                                           ConsumerTag tag, bool permanent) {
  // Defensive — surface an ordering bug before it trips NtAllocate with
  // a zero size or hands a zero-secret token out to a consumer.
  LIBC_ASSERT(layout_.arena_size != 0 &&
              "ArenaPool::reserve_new_arena: pool not configured");
  LIBC_ASSERT(g_pcb.zone0.substrate_secret() != 0 &&
              "ArenaPool::reserve_new_arena: Zone 0 secret not seeded");

  // Reserve the arena as a power-of-2-aligned plain reservation. Plain
  // MEM_RESERVE lets us MEM_COMMIT / MEM_DECOMMIT sub-regions directly
  // without the placeholder split-and-replace dance.
  void *raw = reserve_aligned(layout_.arena_size, layout_.arena_size);
  if (!raw)
    return nullptr;

  // Commit the header page only — everything else stays uncommitted
  // reserved VA (reads/writes fault with ACCESS_VIOLATION until the
  // acquire-time commit). Commit failure here releases the reservation
  // and fails the arena.
  void *header_page = static_cast<char *>(raw) + layout_.header_offset;
  if (!internal::page_commit(header_page, kHeaderPageBytes)) {
    internal::page_free(raw);
    return nullptr;
  }

  ArenaHeader *hdr = static_cast<ArenaHeader *>(header_page);
  // Every arena gets a fresh, process-globally-unique generation — a
  // stale handle from a retired arena whose base VA happened to be
  // reused by this reservation will mismatch the new arena_tag.
  initialize_arena_header(hdr, reinterpret_cast<uintptr_t>(raw),
                          g_substrate.next_generation(), tag);

  // Register in the O(1) substrate-membership bitmap so release() can
  // validate pointers before dereference.
  registry.insert_range(reinterpret_cast<uintptr_t>(raw),
                        layout_.arena_size);

  // Stamp permanence BEFORE the init_node decision. pre_init passes
  // `permanent=true` for the per-class seed reservations so init_node
  // is fully skipped — see the rationale on the `permanent` parameter.
  if (permanent)
    hdr->is_permanent = 1;

  // Track in the all-arenas list for fork/destroy walks.
  all_insert(hdr);
  arena_count_.fetch_add(1, cpp::MemoryOrder::ACQ_REL);

  // Crystalline-stamp non-permanent arenas. init_node sets birth_era
  // (used by the helping protocol's epoch comparisons) and clears the
  // batch_link. Skipped for permanent arenas — they never retire, so
  // their birth_era is unread, and skipping init_node also avoids
  // its `my_thread()` lookup, which would otherwise re-enter
  // get_thread_scratch and trigger ThreadScratch's create_thread_state
  // recursively on threads that haven't yet allocated their scratch.
  // For pre_init's seed reservations on the main thread, that
  // recursion would in turn enqueue a bootstrap-tier receipt before
  // collect_seed_receipts runs, double-stamping the bootstrap synth's
  // VA range. Eager is_permanent stamping above prevents both effects.
  //
  // Stamp the Crystalline-W codec serial here too: the serial is the
  // arena's stable 31-bit identity used by `BatchLinkCodec<ArenaHeader>`.
  // Permanent arenas never retire, so they don't need a serial.
  // Publish into the per-substrate serial_table so decode is O(1).
  if (!hdr->is_permanent) {
    hdr->arena_serial = g_substrate.next_arena_serial();
    g_substrate.publish_arena_serial(hdr->arena_serial, hdr);
    g_substrate_domain.init_node(hdr);
  }

  // NOTE: mapping-table stamping is intentionally NOT done here. It was,
  // but `register_mapping_internal → ensure_slot → substrate_acquire`
  // re-enters `ArenaPool::acquire` on this (or another) pool. The caller
  // (`slow_acquire_arena`) publishes `active_ = fresh` first, then
  // stamps — that way re-entry finds a slot in the fresh arena instead
  // of spinning on the reserving_ flag.

  return hdr;
}

void ArenaPool::initialize_arena_header(ArenaHeader *hdr, uintptr_t base,
                                        uint32_t generation,
                                        ConsumerTag tag) {
  // Zero the header page deterministically. Fresh commits return zero-
  // filled memory, but a recycled arena's header could have stale
  // contents — memset is cheap (4 KB) and uniform across both paths.
  __builtin_memset(hdr, 0, kHeaderPageBytes);

  const uintptr_t secret = g_pcb.zone0.substrate_secret();
  internal::alloc_primitives::SingleCanarySeed arena_seed;
  internal::alloc_primitives::init_seed_or_trap(arena_seed);

  hdr->arena_tag = secret ^ base ^ static_cast<uintptr_t>(generation);
  hdr->arena_secret = arena_seed.seed;
  hdr->reservation_base = reinterpret_cast<void *>(base);
  hdr->generation = generation;
  hdr->class_id = static_cast<uint16_t>(layout_.klass);
  hdr->slots_per_arena =
      static_cast<uint16_t>(layout_.slots_per_arena);
  hdr->slot_size = static_cast<uint32_t>(layout_.slot_size);
  hdr->consumer_tag = static_cast<uint8_t>(tag);

  hdr->live_count.store(0, cpp::MemoryOrder::RELAXED);
  hdr->next_abandoned = nullptr;
  hdr->all_next = nullptr;
  hdr->all_prev = nullptr;
  // Default: substrate-managed arena. `pre_init` flips this to 1 on each
  // class's seed; ThreadScratch's bootstrap-tier path flips it on the
  // synthetic header it embeds in its own scratch page. Already zero
  // from the memset above — explicit assignment makes the contract
  // visible at the initialization site.
  hdr->is_permanent = 0;
  hdr->occupancy.clear_all();
  // slot_meta[] was zeroed by the memset above.
  // CrystallineNode fields (next/slot/birth_era union, batch_link,
  // refs/batch_next union) are also zeroed by the memset — that is
  // the valid "freshly allocated, never retired" state. `init_node`
  // (called from the non-permanent reserve path) stamps birth_era
  // before publication; permanent arenas never call init_node and
  // never retire, so their CrystallineNode fields stay zero forever.
}

// ---- all_head_ list (cold, spinlocked) --------------------------------------

void ArenaPool::all_insert(ArenaHeader *a) {
  all_lock_take(all_lock_);
  a->all_prev = nullptr;
  a->all_next = all_head_;
  if (all_head_)
    all_head_->all_prev = a;
  all_head_ = a;
  all_lock_release(all_lock_);
}

void ArenaPool::all_remove(ArenaHeader *a) {
  all_lock_take(all_lock_);
  if (a->all_prev)
    a->all_prev->all_next = a->all_next;
  else
    all_head_ = a->all_next;
  if (a->all_next)
    a->all_next->all_prev = a->all_prev;
  all_lock_release(all_lock_);
}

// ---- Fork / destroy ---------------------------------------------------------

void ArenaPool::fork_reinit(SubstrateRegistry &registry) {
  // Single-threaded child; no concurrent readers. Clear hot per-thread
  // state (active_, abandoned_head_, reserving_). Surviving arenas
  // stay on all_head_ and keep their live_count / occupancy /
  // slot_meta — the consumer above will re-wire its own view in its
  // own fork_reinit.
  //
  // Crystalline-side fork-reinit MUST have already run (see
  // libc_fork_reinit_impl.cpp's ordering comment). That call zeroed
  // every surviving thread's per-domain region — wiping any retire
  // batches and slot publications inherited from the parent — so any
  // arena that was mid-retire in the parent is effectively dropped
  // in the child. The arena's storage CoW-survives; we treat it as
  // "live but unreachable from any retire batch", which is consistent
  // with rebuilding active_/abandoned_head_ from all_head_ below.
  active_.store(nullptr, cpp::MemoryOrder::RELAXED);
  abandoned_head_.store(nullptr, cpp::MemoryOrder::RELAXED);
  reserving_.store(false, cpp::MemoryOrder::RELAXED);

  // Defensive: an arena marked QUARANTINED in the parent (live_count
  // == kQuarantineSentinel) is one whose retire-CAS succeeded but
  // whose Crystalline reclaim hadn't fired yet at the fork instant.
  // Its CoW copy is still on all_head_; without intervention here,
  // the survivor below would re-promote it to active_ even though its
  // live_count is the sentinel — and the first acquire would observe
  // SENTINEL and continually demote it. Worse, no reclaim path will
  // fire (Crystalline batches were wiped above). Detach it here and
  // free the VA outright. mapping_table.remove is no-op if absent.
  (void)registry;
  for (ArenaHeader *a = all_head_; a;) {
    ArenaHeader *next = a->all_next;
    if (LIBC_UNLIKELY(a->live_count.load(cpp::MemoryOrder::RELAXED) ==
                          kQuarantineSentinel &&
                      !a->is_permanent)) {
      const uintptr_t base = a->arena_base();
      if (::LIBC_NAMESPACE::internal::is_mapping_table_ready())
        g_mapping_table.remove(reinterpret_cast<void *>(base));
      g_substrate.registry().remove_range(base, layout_.arena_size);
      all_remove(a);
      arena_count_.fetch_sub(1, cpp::MemoryOrder::RELAXED);
      internal::page_free(reinterpret_cast<void *>(base));
    }
    a = next;
  }

  // Rebuild active_ / abandoned_head_ from the surviving arenas on
  // all_head_. Without this, any arena that was on abandoned_head_ in
  // the parent would be unreachable in the child: not active, not on
  // abandoned. Promote the first NON-PERMANENT survivor to active_;
  // permanent arenas (seed + bootstrap synth) have live_count pinned
  // at slots_per_arena, so promoting them as active_ would force the
  // first acquire down a wasted slow-path round trip (claim_slot_in_arena
  // sees full → demote → pop_abandoned_filtered). Permanent arenas
  // still go on abandoned_head_ where pop_abandoned_filtered's drained-
  // permanent re-promotion path can correctly handle them once they
  // drain. Single-threaded: no lock or CAS needed.
  ArenaHeader *promoted = nullptr;
  for (ArenaHeader *a = all_head_; a; a = a->all_next) {
    if (!promoted && !a->is_permanent) {
      promoted = a;
      continue;
    }
    a->next_abandoned = abandoned_head_.load(cpp::MemoryOrder::RELAXED);
    abandoned_head_.store(a, cpp::MemoryOrder::RELAXED);
  }
  if (promoted)
    active_.store(promoted, cpp::MemoryOrder::RELAXED);
}

void ArenaPool::destroy(SubstrateRegistry &registry) {
  // Process fini. Release every substrate-owned arena. Not lock-free;
  // no concurrent readers at this point.
  //
  // Permanent arenas have a separate VA owner (seed arenas → OS reclaims
  // on process exit anyway; bootstrap-tier scratch → ThreadScratch's
  // cleanup owns the `page_free`). For both we still drop them out of
  // the substrate's bookkeeping (registry + all_head_) so a post-destroy
  // observer sees consistent state, but we do NOT call `page_free` —
  // double-freeing the bootstrap-tier scratch would corrupt
  // ThreadScratch's owning thread, and a `page_free` on the seed VA
  // before process teardown finishes could race a still-decommissioning
  // mapping-table consumer.
  active_.store(nullptr, cpp::MemoryOrder::RELAXED);
  abandoned_head_.store(nullptr, cpp::MemoryOrder::RELAXED);
  reserving_.store(false, cpp::MemoryOrder::RELAXED);

  all_lock_take(all_lock_);
  ArenaHeader *a = all_head_;
  all_head_ = nullptr;
  all_lock_release(all_lock_);

  while (a) {
    ArenaHeader *next = a->all_next;
    const uintptr_t base = a->arena_base();
    registry.remove_range(base, layout_.arena_size);
    if (!a->is_permanent)
      internal::page_free(reinterpret_cast<void *>(base));
    a = next;
  }
  arena_count_.store(0, cpp::MemoryOrder::RELAXED);
}

// ============================================================================
// VaSubstrate — dispatch and lifecycle
// ============================================================================

// Header-inline now. Keep consistency static_asserts so the ClassLayout
// table and the header-literal copies cannot drift apart. Both the
// slot_size and arena_size constexpr ladders in va_substrate.h must
// match the per-class ClassLayout values exactly.
static_assert(VaSubstrate::slot_size(SubSlotClass::Small) ==
                  ClassLayout<SubSlotClass::Small>::kSlotSize,
              "slot_size(Small) header literal must match ClassLayout");
static_assert(VaSubstrate::slot_size(SubSlotClass::Medium) ==
                  ClassLayout<SubSlotClass::Medium>::kSlotSize,
              "slot_size(Medium) header literal must match ClassLayout");
static_assert(VaSubstrate::slot_size(SubSlotClass::Large) ==
                  ClassLayout<SubSlotClass::Large>::kSlotSize,
              "slot_size(Large) header literal must match ClassLayout");
static_assert(VaSubstrate::slot_size(SubSlotClass::XLarge) ==
                  ClassLayout<SubSlotClass::XLarge>::kSlotSize,
              "slot_size(XLarge) header literal must match ClassLayout");
static_assert(VaSubstrate::slot_size(SubSlotClass::Huge) ==
                  ClassLayout<SubSlotClass::Huge>::kSlotSize,
              "slot_size(Huge) header literal must match ClassLayout");
static_assert(VaSubstrate::arena_size(SubSlotClass::Small) ==
                  ClassLayout<SubSlotClass::Small>::kArenaSize,
              "arena_size(Small) header literal must match ClassLayout");
static_assert(VaSubstrate::arena_size(SubSlotClass::Medium) ==
                  ClassLayout<SubSlotClass::Medium>::kArenaSize,
              "arena_size(Medium) header literal must match ClassLayout");
static_assert(VaSubstrate::arena_size(SubSlotClass::Large) ==
                  ClassLayout<SubSlotClass::Large>::kArenaSize,
              "arena_size(Large) header literal must match ClassLayout");
static_assert(VaSubstrate::arena_size(SubSlotClass::XLarge) ==
                  ClassLayout<SubSlotClass::XLarge>::kArenaSize,
              "arena_size(XLarge) header literal must match ClassLayout");
static_assert(VaSubstrate::arena_size(SubSlotClass::Huge) ==
                  ClassLayout<SubSlotClass::Huge>::kArenaSize,
              "arena_size(Huge) header literal must match ClassLayout");

void VaSubstrate::pre_init() {
  if (!init_latch_.try_begin()) {
    init_latch_.wait_ready();
    return;
  }

  // Phase 0a is single-threaded under the PcbInitAccess gate. Ordering:
  //   1. Seed Zone 0 secrets + root. Every downstream step reads these.
  //   2. Configure per-pool layout constants. reserve_new_arena depends
  //      on layout_.arena_size.
  //   3. Eager-init the SubstrateRegistry (allocates the L1 directory
  //      once). Avoids a first-insert branch on every later slow_acquire
  //      and surfaces registry-reserve failure at bootstrap instead of
  //      the first consumer allocation.
  //   4. Pre-warm one arena per class so the first acquire from each
  //      class is pure hot path. The mapping table is not yet up —
  //      reserve_new_arena detects this via `is_mapping_table_ready()`
  //      and defers sentinel stamping; the walker's Pass 2 stamps the
  //      seed arenas after the table reaches INIT_READY.
  //
  // All four steps avoid any PCB accessor other than the two Zone 0
  // substrate_* fields this function just set. `reserve_aligned` calls
  // NtAllocateVirtualMemoryEx directly; `required_l1_size()` reads
  // pcb_max_address() which pcb_startup_init publishes before dispatching
  // here.
  internal::alloc_primitives::CanarySeed seeds;
  internal::alloc_primitives::init_seed_or_trap(seeds);

  internal::PcbInitAccess::set_substrate_secret(seeds.primary);
  internal::PcbInitAccess::set_substrate_token_key(seeds.secondary);
  internal::PcbInitAccess::set_substrate_root(this);

  for (unsigned c = 0; c < static_cast<unsigned>(SubSlotClass::Count); ++c)
    pools_[c].configure_class_only(static_cast<SubSlotClass>(c));

  registry_.ensure_init();

  for (unsigned c = 0; c < static_cast<unsigned>(SubSlotClass::Count); ++c) {
    // Pass `permanent=true` so reserve_new_arena stamps is_permanent=1
    // BEFORE attempting the Crystalline init_node call. init_node is
    // skipped entirely for permanent arenas, which avoids triggering
    // ThreadScratch's create_thread_state on the main thread mid-
    // pre_init — see the contract on `permanent` for the full rationale.
    ArenaHeader *seed =
        pools_[c].reserve_new_arena(registry_, ConsumerTag::_Seed,
                                     /*permanent=*/true);
    // Bootstrap VA exhaustion is unrecoverable — if we cannot reserve a
    // single 1-2 MB range here, the process has no path forward.
    if (LIBC_UNLIKELY(!seed))
      __builtin_trap();
    // Seeds are permanent: the Small seed will hold the mapping table's
    // L1 directory, and downstream code expects per-class fast-path
    // headroom from at least one always-live arena. The is_permanent
    // flag was set inside reserve_new_arena via the permanent=true
    // parameter; we don't need to re-stamp here.
    pools_[c].active_.store(seed, cpp::MemoryOrder::RELEASE);
  }

  init_latch_.publish_ready();
}

uint32_t VaSubstrate::collect_seed_receipts(
    ::LIBC_NAMESPACE::internal::Receipt *out, uint32_t cap) {
  // Walk every arena in every pool and emit one Receipt per arena into
  // the walker's stack-local buffer. The `.libcmem` walker's Pass 2
  // stamps LIBC_INTERNAL on every receipt we emit.
  //
  // The `g_mapping_table_ready` latch is NO LONGER set here — moved to
  // the very end of the walker's Pass 2 so it flips only after every
  // receipt has been stamped AND the mapping table is fully INIT_READY.
  // Latching here would advertise readiness while the table is still
  // INIT_IN_PROGRESS in Phase 2, allowing a reentrant
  // `register_mapping_internal` (e.g. via Crystalline → substrate
  // slow-path → reserve_new_arena once consumers migrate) to deadlock
  // on a same-thread `init_state_.wait(INIT_IN_PROGRESS)`.
  using ::LIBC_NAMESPACE::internal::InternalKind;
  static constexpr InternalKind kSeedKinds[] = {
      InternalKind::SubstrateSeedSmall,
      InternalKind::SubstrateSeedMedium,
      InternalKind::SubstrateSeedLarge,
      InternalKind::SubstrateSeedXLarge,
      InternalKind::SubstrateSeedHuge,
  };
  static_assert(sizeof(kSeedKinds) / sizeof(kSeedKinds[0]) ==
                    static_cast<size_t>(SubSlotClass::Count),
                "kSeedKinds must cover every SubSlotClass");

  uint32_t n = 0;
  for (unsigned c = 0; c < static_cast<unsigned>(SubSlotClass::Count); ++c) {
    ArenaPool &p = pools_[c];
    const SubSlotLayout &layout = p.layout();

    all_lock_take(p.all_lock_);
    ArenaHeader *a = p.all_head_;
    while (a) {
      if (LIBC_UNLIKELY(n >= cap)) {
        all_lock_release(p.all_lock_);
        __builtin_trap();
      }
      out[n].base = reinterpret_cast<void *>(a->arena_base());
      out[n].size = layout.arena_size;
      out[n].kind = kSeedKinds[c];
      ++n;
      a = a->all_next;
    }
    all_lock_release(p.all_lock_);
  }

  return n;
}

void VaSubstrate::fork_reinit() {
  // Single-threaded child context. Reuse of the substrate's secrets
  // across fork is safe — parent and child don't share address space
  // from the child's perspective, so no token replay concern.
  init_latch_.fork_reinit();
  for (unsigned c = 0; c < static_cast<unsigned>(SubSlotClass::Count); ++c)
    pools_[c].fork_reinit(registry_);
}

void VaSubstrate::destroy() {
  for (unsigned c = 0; c < static_cast<unsigned>(SubSlotClass::Count); ++c)
    pools_[c].destroy(registry_);
}

SubSlotHandle VaSubstrate::acquire(SubSlotClass c, ConsumerTag tag) {
  return pools_[static_cast<unsigned>(c)].acquire(registry_, tag,
                                                   /*affinity_hint=*/nullptr);
}

SubSlotHandle VaSubstrate::acquire(SubSlotClass c, ConsumerTag tag,
                                    ArenaHeader *affinity_hint) {
  return pools_[static_cast<unsigned>(c)].acquire(registry_, tag,
                                                   affinity_hint);
}

SubSlotHandle VaSubstrate::acquire_uncommitted(SubSlotClass c,
                                                ConsumerTag tag) {
  return pools_[static_cast<unsigned>(c)].acquire_uncommitted(
      registry_, tag, /*affinity_hint=*/nullptr);
}

SubSlotHandle VaSubstrate::acquire_uncommitted(SubSlotClass c,
                                                ConsumerTag tag,
                                                ArenaHeader *affinity_hint) {
  return pools_[static_cast<unsigned>(c)].acquire_uncommitted(
      registry_, tag, affinity_hint);
}

void VaSubstrate::release(SubSlotHandle handle) {
  if (LIBC_UNLIKELY(!handle))
    __builtin_trap();

  // The handle carries its owning class in the top 3 bits of the token
  // so we never dereference an arena header at a speculative class's
  // offset. Corruption in those top bits is detected downstream: the
  // full-token CAS inside ArenaPool::release will fail because the
  // token's low 61 bits (the authenticity hash) won't match the stored
  // owner_token for any class except the original.
  const unsigned c =
      static_cast<unsigned>(handle.token() >> 61) & 0x7u;
  if (LIBC_UNLIKELY(c >= static_cast<unsigned>(SubSlotClass::Count)))
    __builtin_trap();
  pools_[c].release(handle);
}

void VaSubstrate::try_reclaim() {
  // Crystalline auto-drives reclaim — this is just a "drop my pins"
  // hint. clear_all walks every slot on the calling thread for the
  // substrate domain and drains pending publications. If the calling
  // thread held the last reservation pinning a recently-retired arena,
  // its anchor's refs wraps to zero and `substrate_free_arena` fires
  // synchronously here.
  g_substrate_domain.clear_all();
}

void VaSubstrate::detach_arena_from_pool(SubSlotClass c, ArenaHeader *a) {
  // Called from `substrate_free_arena` (Crystalline FreeFn). The arena
  // has already been validated as non-permanent and class_id-coherent
  // by the caller. Simply unlink from the per-pool all_head_ list and
  // adjust arena_count_; the caller takes care of mapping-table /
  // registry / page_free.
  ArenaPool &p = pools_[static_cast<unsigned>(c)];
  p.all_remove(a);
  p.arena_count_.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
}

// ============================================================================
// Sub-slot commit helpers + ownership predicate + arena_of
// ============================================================================

namespace {
// Decode the SubSlotClass embedded in a token's top 3 bits. Traps on a
// value past Count — caller should only invoke this on a handle minted
// by substrate_acquire (or reconstructed via from_detached from a valid
// token), so an out-of-range class signals corruption.
[[nodiscard]] SubSlotClass class_from_token(uint64_t token) {
  const unsigned c = static_cast<unsigned>(token >> 61) & 0x7u;
  if (LIBC_UNLIKELY(c >= static_cast<unsigned>(SubSlotClass::Count)))
    __builtin_trap();
  return static_cast<SubSlotClass>(c);
}
} // namespace

bool substrate_commit_subrange(SubSlotHandle h, size_t offset,
                                size_t length) {
  if (LIBC_UNLIKELY(!h))
    __builtin_trap();
  if (LIBC_UNLIKELY(length == 0))
    __builtin_trap();
  const SubSlotClass c = class_from_token(h.token());
  const size_t slot = VaSubstrate::slot_size(c);
  if (LIBC_UNLIKELY(offset > slot || length > slot - offset))
    __builtin_trap();
  void *base = static_cast<char *>(h.ptr()) + offset;
  return internal::page_commit(base, length);
}

void substrate_decommit_subrange(SubSlotHandle h, size_t offset,
                                  size_t length) {
  if (LIBC_UNLIKELY(!h))
    __builtin_trap();
  if (LIBC_UNLIKELY(length == 0))
    __builtin_trap();
  const SubSlotClass c = class_from_token(h.token());
  const size_t slot = VaSubstrate::slot_size(c);
  if (LIBC_UNLIKELY(offset > slot || length > slot - offset))
    __builtin_trap();
  void *base = static_cast<char *>(h.ptr()) + offset;
  // page_decommit is best-effort; on already-uncommitted ranges it is a
  // no-op at the kernel level. We intentionally drop the return value.
  (void)internal::page_decommit(base, length);
}

bool is_substrate_owned(const void *p) {
  if (p == nullptr)
    return false;
  const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
  // Walk every class. If `p` lies inside any substrate-owned arena, the
  // arena_size-rounded base will hit the SubstrateRegistry. Iterating
  // by class avoids needing a per-page registry lookup; each class
  // probe is one mask + one bitmap lookup.
  for (unsigned i = 0; i < static_cast<unsigned>(SubSlotClass::Count); ++i) {
    const SubSlotClass c = static_cast<SubSlotClass>(i);
    const size_t arena = VaSubstrate::arena_size(c);
    const uintptr_t base = addr & ~(arena - 1);
    if (g_substrate.registry().contains(base))
      return true;
  }
  return false;
}

ArenaHeader *arena_of(const void *slot_ptr, SubSlotClass c) {
  const uintptr_t p = reinterpret_cast<uintptr_t>(slot_ptr);
  const size_t arena = VaSubstrate::arena_size(c);
  const uintptr_t base = p & ~(arena - 1);
  // Header lives at the per-class header_offset inside the arena. The
  // offset is the same for every class today (kGuardBytes = 4 KB) but
  // we read it via layout_of so a future per-class divergence picks up
  // automatically.
  const SubSlotLayout layout = layout_of(c);
  return reinterpret_cast<ArenaHeader *>(base + layout.header_offset);
}

} // namespace alloc
} // namespace windows

namespace internal {

// Bridges into the public substrate entry points under the
// `LIBC_NAMESPACE::internal` namespace the fork-reinit orchestrator uses.
void va_substrate_fork_reinit() {
  LIBC_NAMESPACE::windows::alloc::g_substrate.fork_reinit();
}

static void va_substrate_fini() {
  LIBC_NAMESPACE::windows::alloc::g_substrate.destroy();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Fini phase 1 — peer of va_inventory. Substrate destroy() performs only
// NT VM ops (page_free / MEM_RELEASE) and linked-list walks; no slab, no
// futex, no malloc. Sits below $P4 (mapping_table, pools) and $P2
// (posix_alloc) so every subsystem that might still reference substrate
// VA has already torn down by the time destroy() runs.
LIBC_REGISTER_FINI(1, va_substrate,
                   &::LIBC_NAMESPACE::internal::va_substrate_fini)

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// `.libcmem` handler. Runs at Tier A Phase 0c.5 Pass 1:
//   1. pre_init() seeds PCB Zone 0 secrets, configures pool layouts,
//      eager-inits the SubstrateRegistry, and reserves one seed arena
//      per class. The mapping table is not yet live — seed arenas are
//      stamped as LIBC_INTERNAL by the walker's Pass 2.
//   2. collect_seed_receipts() walks every pool's seed arenas and writes
//      one Receipt per arena into the walker's buffer. The walker
//      latches `g_mapping_table_ready` only after Pass 2 has finished
//      stamping every receipt, so until then post-Pass-1 reservations
//      keep using deferred-Receipt registration.
static uint32_t va_substrate_init_fn(Receipt *out, uint32_t cap) {
  // Crystalline domain registration runs first. CrystallineDomain<>
  // is trivially constructed at file scope (libc forbids static ctors),
  // so the descriptor must be self-installed here before any substrate
  // path can call read()/retire(). Tier A Phase 1 is single-threaded,
  // so this lands well before any other thread can observe the domain.
  ::LIBC_NAMESPACE::windows::alloc::g_substrate_domain.init_registration();
  ::LIBC_NAMESPACE::windows::alloc::g_substrate.pre_init();
  return ::LIBC_NAMESPACE::windows::alloc::g_substrate.collect_seed_receipts(
      out, cap);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Phase 1 — must run first. Seeds Zone 0 substrate secrets and reserves
// the seed arenas. Phase 2 (mapping_table.ensure_init) and later phases
// may consume the table via `g_mapping_table_ready` checks only after
// this handler returns; seeding secrets before the mapping table uses
// them is the load-bearing reason this is Phase 1.
LIBC_REGISTER_MEMORY_PRIMITIVE(va_substrate, 1,
                               &::LIBC_NAMESPACE::internal::va_substrate_init_fn)

LIBC_REGISTER_FORK_REINIT(va_substrate,
                          ::LIBC_NAMESPACE::internal::kForkPrioVaSubstrate,
                          &::LIBC_NAMESPACE::internal::va_substrate_fork_reinit)
