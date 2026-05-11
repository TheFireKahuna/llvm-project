//===-- Lock-free radix-tree mapping registry -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Sole source of truth for VA state across the libc mmap subsystem. Every
// logical `mmap` publishes one or more entries here; every downstream op
// (munmap, mprotect, mremap, fork, msync, madvise, shm_ops, VEH) consults
// the table first.
//
// Three-level radix tree keyed on `view_base >> 16`:
//   - L1: runtime-sized root (bits [N:20]) — sized from MaximumUserModeAddress
//   - L2: 1024 entries (bits [19:10]) — page_alloc on demand, 8KB
//   - L3: 1024 slots   (bits [9:0])  — page_alloc on demand, 64KB + occupancy
//
// Each slot stores one mapping's metadata. Slot position is uniquely
// determined by the address — no hashing, no probing, no tombstones. All
// state queries (insert, lookup, remove, extract, has_neighbor) are O(1)
// worst case.
//
// ---- State machine per slot (4-bit state encoded in key bits [3:0]) ----
//
//   0x0 FREE                      Empty / never used / cleared
//   0x1 WRITING                   Fields being modified, readers retry
//   0x2 REMAPPING                 VA mutation in progress, VEH stalls
//   0x3 WRITING+REMAPPING         Commit-in-progress on a REMAPPING slot
//   0x4 PLACEHOLDER               VA reserved, no view mapped
//   0x5 PLACEHOLDER+WRITING       Mutation of PLACEHOLDER fields
//   0x8 FOREIGN                   Not ours; cordon from merge / hint / release
//
// view_base is always >= 64KB aligned, so the low 16 bits of the address are
// zero — bits [3:0] of the key are available as state flags. Using 4 bits
// leaves room for extensions without reshaping the encoding.
//
// ---- Slot contents ----
//
// The slot stores a `region_id` (index into RegionPool) and an `alloc_id`
// (8-bit generation for ABA-safe pool reuse). Section and file handles
// live in the pool's RegionDesc, not the slot — one logical mmap call owns
// exactly one handle pair regardless of how many slots cover its VA.
//
// Readers obtain handles by first snapshotting the slot (seqlock-protected)
// and then resolving the snapshot's region_id through RegionPool::resolve(),
// which validates the alloc_id against the captured value.
//
// ---- VEH remap guard ----
//
// A separate pre-committed guard array holds Slot* pointers for in-progress
// REMAPPING operations, so the VEH handler can stall faulting threads
// without navigating the radix tree on the fault path. The fast path
// (active_remap_count_ == 0) skips the guard scan entirely.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MAPPING_TABLE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MAPPING_TABLE_H

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/legacy/va_substrate.h"
#include "src/__support/OSUtil/windows/memory/legacy/commit_region.h"
#include "src/__support/OSUtil/windows/memory/legacy/mapping_table_process_state.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_desc.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_pool.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/threads/windows/futex_utils.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// ============================================================================
// Per-mapping flags (distinct from region flags).
// ============================================================================
//
// Slot-level flags are state that varies across a region's slots. Region-
// level flags (see region_desc.h region_flag::*) are immutable for the
// region's life.
//
// Slot flags:
//   [0] NUMA_INTERLEAVE  — per-page NUMA rotation via VEH; cold union holds mask
//   [1] PROT_CHANGED     — mprotect touched this slot; VEH takes slow path
//   [2] FOREIGN_STALE    — FOREIGN stamp past TTL; re-probe on next touch
//
// VM_FLAG_SEC_RESERVE is deliberately NOT present: the SEC_RESERVE demand-
// commit distinction lives in the region's shape (FILE_VIEW_RESERVE /
// region_flag::NORESERVE), not in per-slot flags.

inline constexpr DWORD VM_FLAG_NUMA_INTERLEAVE = 0x1;
inline constexpr DWORD VM_FLAG_PROT_CHANGED    = 0x2;
inline constexpr DWORD VM_FLAG_FOREIGN_STALE   = 0x4;

// ============================================================================
// MappingEntry — returned by extract(). Ownership of the slot's region
// reference transfers to the caller, who is responsible for calling
// g_region_pool.release(region_id).
// ============================================================================

struct MappingEntry {
  void *view_base;
  SIZE_T view_size;
  uint32_t region_id;
  uint8_t alloc_id;
  DWORD view_prot;
  DWORD flags;
};

// ============================================================================
// SlotSnapshot — consistent point-in-time read of a slot plus its region.
// ============================================================================
//
// Returned by MappingTable::snapshot(). The region pointer is resolved
// once under the seqlock fence with alloc_id validation — callers may
// dereference fields (handles, shape, flags) directly without further
// synchronization for the duration of the snapshot's validity window.
//
// This eliminates three classes of concurrency bugs:
//   1. ABA:       version counter + alloc_id generation
//   2. Partial:   all fields captured in one atomic snapshot
//   3. UAF:       resolve() guarantees region is live during the window

struct SlotSnapshot {
  void *view_base;
  SIZE_T view_size;
  uint32_t region_id;
  uint8_t alloc_id;
  DWORD view_prot;
  DWORD flags;

  // Resolved region descriptor. Null only when the slot state is FOREIGN
  // with the NONE sentinel as its region_id. For LIVE / PLACEHOLDER slots
  // the snapshot loop guarantees a non-null pointer (see snapshot_at_slot
  // for the alloc-id retry contract). Callers dereference fields directly:
  // `region->section_handle`, `region->is_cow()`, `region->current_shape()`.
  //
  // Const because snapshot is a read-only window. Callers that legitimately
  // need to mutate (chunk-list updates, shape promotion) hold MmapLock
  // writer and re-resolve via g_region_pool.get_mutable(region_id).
  const memory::RegionDesc *region;

  // Convert to a MappingEntry for transfer-of-ownership callers (extract).
  // NOTE: the caller becomes responsible for the region reference that
  // was held by the slot — they must release it once done.
  LIBC_INLINE MappingEntry to_entry() const {
    return MappingEntry{view_base, view_size,  region_id, alloc_id,
                        view_prot, flags};
  }
};

// ============================================================================
// MappingTable — the radix-tree registry.
// ============================================================================

// Predicate for MappingTable::wait_for_remap_drain. Free function (not a
// capturing lambda) so its address has process-lifetime validity —
// PredicateFn is a plain `bool(*)(uint32_t, uint32_t)` read concurrently
// by wakers.
[[clang::always_inline]] LIBC_INLINE bool
remap_count_is_zero(uint32_t v, uint32_t /*arg*/) noexcept {
  return v == 0;
}

class MappingTable {
  // ---- Key encoding ------------------------------------------------------
  //
  // key layout (uintptr_t):
  //   bits [63:16]  view_base >> 16  (nonzero for any 64 KB-aligned address)
  //   bits [3:0]    state code       (see below)
  //   bits [15:4]   reserved
  //
  // State codes:
  //   0x0 FREE, 0x1 WRITING, 0x2 REMAPPING, 0x3 WRITING|REMAPPING
  //   0x4 PLACEHOLDER, 0x5 PLACEHOLDER|WRITING
  //   0x8 FOREIGN
  //
  // Using 4 bits of state leaves room for future additions. Readers mask
  // the state bits off to recover the address.

  static constexpr uintptr_t KEY_FREE         = 0;
  static constexpr uintptr_t WRITING_BIT      = 0x1;
  static constexpr uintptr_t REMAPPING_BIT    = 0x2;
  static constexpr uintptr_t PLACEHOLDER_BIT  = 0x4;
  static constexpr uintptr_t FOREIGN_BIT      = 0x8;
  static constexpr uintptr_t STATE_MASK       = 0xF;

  // ---- state_aux encoding for FOREIGN slots ------------------------------
  //
  // For FOREIGN slots only, the per-slot state_aux byte serves as a lock-
  // free probe coordinator:
  //
  //   bit 0       FOREIGN_PROBE_LOCK — exactly one thread at a time owns
  //               the MRI_Ex re-probe syscall. Lost-CAS waiters park on
  //               &slot->state_aux via futex_addr::wait; the probing
  //               thread broadcasts via futex_addr::wake when it clears
  //               the lock.
  //   bits [7:1]  stamp generation — bumped on every completed probe so
  //               TTL heuristics can reason about cordon freshness without
  //               re-querying NT. Wraps modulo 128.
  //
  // Other states leave state_aux at 0 (or use it for unrelated per-state
  // bookkeeping that does not collide with the lock bit).
  static constexpr uint8_t FOREIGN_PROBE_LOCK = 0x01;
  static constexpr uint8_t FOREIGN_AGE_SHIFT  = 1;
  static constexpr uint8_t FOREIGN_AGE_MASK   = 0xFE;

  // Compose a state key from address + state code.
  LIBC_INLINE static uintptr_t live_key(uintptr_t addr) { return addr; }
  LIBC_INLINE static uintptr_t placeholder_key(uintptr_t addr) {
    return addr | PLACEHOLDER_BIT;
  }
  LIBC_INLINE static uintptr_t foreign_key(uintptr_t addr) {
    return addr | FOREIGN_BIT;
  }

  // Query the state bits from a key.
  LIBC_INLINE static bool is_free(uintptr_t k) { return k == KEY_FREE; }
  LIBC_INLINE static bool is_live(uintptr_t k) {
    return k != KEY_FREE && (k & STATE_MASK) == 0;
  }
  LIBC_INLINE static bool is_placeholder(uintptr_t k) {
    return (k & STATE_MASK) == PLACEHOLDER_BIT;
  }
  LIBC_INLINE static bool is_foreign(uintptr_t k) {
    return (k & STATE_MASK) == FOREIGN_BIT;
  }
  LIBC_INLINE static bool is_writing(uintptr_t k) {
    return (k & WRITING_BIT) != 0;
  }
  LIBC_INLINE static bool is_remapping(uintptr_t k) {
    return (k & REMAPPING_BIT) != 0;
  }

  // ---- Slot struct (64 bytes, one cache line) ----------------------------
  //
  // offsets
  //   0    key             Atomic<uintptr_t>
  //   8    region_id       u32
  //   12   alloc_id        u8
  //   13   state_aux       u8   (per-state flags, e.g. foreign stamp age)
  //   14   reserved        u16
  //   16   view_prot       DWORD
  //   20   flags           Atomic<DWORD>
  //   24   extent          SIZE_T   (LIVE: view_size; REMAPPING: guarded range)
  //   32   cold_union      12 B     (REMAPPING owner / NUMA mask / WRITING owner)
  //   44   reserved        4 B
  //   48   cold_tail       8 B      (spare)
  //   56   reserved        4 B
  //   60   version         Atomic<u32>

  struct alignas(64) Slot {
    cpp::Atomic<uintptr_t> key{0};    // 0
    uint32_t region_id{0};            // 8
    uint8_t alloc_id{0};              // 12
    uint8_t state_aux{0};             // 13
    uint16_t reserved_0{0};           // 14
    DWORD view_prot{0};               // 16
    cpp::Atomic<DWORD> flags{0};      // 20
    SIZE_T extent{0};                 // 24

    // Cold union at offset 32, 12 bytes, dual-purposed by state.
#pragma pack(push, 4)
    union {
      struct { // REMAPPING: guard-array ownership tracking.
        HANDLE remap_owner_thread; // 32 (8 B)
        int remap_guard_index;     // 40 (4 B)
      };
      struct { // LIVE + VM_FLAG_NUMA_INTERLEAVE: per-page NUMA mask.
        DWORD64 numa_interleave_mask; // 32 (8 B)
        ULONG numa_node_count;        // 40 (4 B)
      };
      struct { // WRITING: owner-tracking for dead-owner recovery.
        DWORD writing_owner_tid;    // 32 (4 B)
        uint32_t writing_start_time;// 36 (4 B)
        int writing_pad_;           // 40 (4 B)
      };
    };
#pragma pack(pop)

    uint32_t reserved_1{0};         // 44
    uint64_t cold_tail{0};          // 48 — spare for future state-aux
    uint32_t reserved_2{0};         // 56
    cpp::Atomic<uint32_t> version{0}; // 60
  };

  static_assert(sizeof(Slot) == 64, "Slot must be exactly one cache line");
  static_assert(__builtin_offsetof(Slot, key) == 0);
  static_assert(__builtin_offsetof(Slot, region_id) == 8);
  static_assert(__builtin_offsetof(Slot, alloc_id) == 12);
  static_assert(__builtin_offsetof(Slot, state_aux) == 13);
  static_assert(__builtin_offsetof(Slot, view_prot) == 16);
  static_assert(__builtin_offsetof(Slot, flags) == 20);
  static_assert(__builtin_offsetof(Slot, extent) == 24);
  static_assert(__builtin_offsetof(Slot, version) == 60);

  // ---- Radix tree geometry -----------------------------------------------

  static constexpr int L2_BITS = 10;
  static constexpr int L3_BITS = 10;
  static constexpr int L2_SIZE = 1 << L2_BITS;
  static constexpr int L3_SIZE = 1 << L3_BITS;
  static constexpr int L2_MASK = L2_SIZE - 1;
  static constexpr int L3_MASK = L3_SIZE - 1;

  // L3: 1024 slots × 64 B = 64 KB, plus 128 B occupancy bitmap (16 × u64).
  // Atomic so concurrent register/remove in the same L3Page are safe.
  struct L3Page {
    Slot slots[L3_SIZE];
    cpp::Atomic<uint64_t> occupancy[16];
  };
  static_assert(sizeof(L3Page) == 65536 + 128);

  struct L2Page {
    cpp::Atomic<L3Page *> children[L2_SIZE];
  };
  static_assert(sizeof(L2Page) == 8192);

  // ---- Init state --------------------------------------------------------

  static constexpr FutexValueType INIT_UNINITIALIZED = 0;
  static constexpr FutexValueType INIT_IN_PROGRESS = 1;
  static constexpr FutexValueType INIT_READY = 2;
  static constexpr FutexValueType INIT_DESTROYED = 3;

  // ---- Timing constants --------------------------------------------------

  // 1ms sleep unit for slot waits (NT-relative 100ns units, negative).
  static constexpr LONGLONG SLOT_WAIT_100NS = -10000;
  // 10ms for remap wait.
  static constexpr LONGLONG REMAP_WAIT_TIMEOUT_100NS = -100000;

  // ---- Data members ------------------------------------------------------
  //
  // MappingTable carries no state of its own. The table's runtime-mutable
  // bookkeeping lives in PCB Zone 1 as `g_pcb.mapping_table`
  // (MappingTableProcessState); the sealed-for-life handles (l1 / l1_size
  // / max_view_base / remap_guards) live in PCB Zone 0 as
  // `g_pcb.zone0.mapping_table_*`. The accessors below resolve both
  // halves to direct PCB loads — zero-indirection, source-compatible
  // with the old in-class members.

  // Single accessor for the whole Zone 1 state struct. Callsites read
  // fields via `state().<field>_` — matches how the class used to read
  // its own non-static members and keeps per-field references short.
  LIBC_INLINE static memory::MappingTableProcessState &state() {
    return g_pcb.mapping_table;
  }

  // Remap guard array (demand-committed). Each entry stores a Slot* (as
  // uintptr_t). 0 = free; nonzero = REMAPPING slot pointer.
  LIBC_INLINE static uint32_t guards_per_page() {
    return static_cast<uint32_t>(get_cached_page_size() /
                                 sizeof(cpp::Atomic<uintptr_t>));
  }
  static constexpr SIZE_T GUARD_RESERVE_BYTES = 64 * 1024;
  static constexpr uint32_t MAX_GUARDS =
      GUARD_RESERVE_BYTES / sizeof(cpp::Atomic<uintptr_t>);

  // ---- PCB-resident handle accessors -------------------------------------
  //
  // These resolve to direct loads from g_pcb.zone0 (a fixed VA in the
  // sealed PCB section). After Phase 0c.5 the values never change for the
  // process lifetime, so callers may treat them as plain reads — no
  // memory-order constraints are implied.
  LIBC_INLINE static cpp::Atomic<L2Page *> *pcb_l1() {
    return static_cast<cpp::Atomic<L2Page *> *>(
        g_pcb.zone0.mapping_table_l1());
  }
  LIBC_INLINE static size_t pcb_l1_size() {
    return g_pcb.zone0.mapping_table_l1_size();
  }
  LIBC_INLINE static uintptr_t pcb_max_view_base() {
    return g_pcb.zone0.mapping_table_max_view_base();
  }
  LIBC_INLINE static cpp::Atomic<uintptr_t> *pcb_remap_guards() {
    return static_cast<cpp::Atomic<uintptr_t> *>(
        g_pcb.zone0.mapping_table_remap_guards());
  }

  // ---- Radix key decomposition ------------------------------------------

  LIBC_INLINE static uintptr_t radix_key(uintptr_t view_base) {
    return view_base >> 16;
  }
  LIBC_INLINE static size_t l1_index(uintptr_t key) {
    return static_cast<size_t>(key >> (L2_BITS + L3_BITS));
  }
  LIBC_INLINE static int l2_index(uintptr_t key) {
    return static_cast<int>((key >> L3_BITS) & L2_MASK);
  }
  LIBC_INLINE static int l3_index(uintptr_t key) {
    return static_cast<int>(key & L3_MASK);
  }

  LIBC_INLINE static uintptr_t runtime_max_view_base() {
    // Source MaximumUserModeAddress / AllocationGranularity directly from
    // the kernel via NtQuerySystemInformation, bypassing the PCB. This
    // function runs in Tier A Phase 0c.5 (right after pcb_startup_init);
    // the PCB-cached values would also be valid here, but going directly
    // to the kernel keeps the dependency on PCB to a minimum and lets a
    // future hoist of mapping-table init pre-PCB stay source-stable.
    //
    // TODO(MmHighestUserAddress trust): pcb_startup_init validates the
    // SYSTEM_BASIC_INFORMATION layout against KUSER_SHARED_DATA before
    // accepting the values into PCB. This function bypasses that check —
    // a kernel that misreports MaximumUserModeAddress is now believed
    // without the PCB-mediated cross-check. Fine on the supported NT
    // floor, but if PCB validation grows beyond a layout sanity check
    // (e.g. SCG / CFG-style policy bits) we'll need an early validator
    // path here too.
    SYSTEM_BASIC_INFORMATION sbi;
    uintptr_t max_address;
    uintptr_t alloc_gran;
    if (NT_SUCCESS(::NtQuerySystemInformation(SystemBasicInformation, &sbi,
                                              sizeof(sbi), nullptr))) {
      max_address = static_cast<uintptr_t>(sbi.MaximumUserModeAddress);
      alloc_gran = static_cast<uintptr_t>(sbi.AllocationGranularity);
    } else {
      // Match app_init.cpp's x64 fallback. Only reached if ntdll is
      // catastrophically broken.
      max_address = 0x7FFFFFFEFFFFULL;
      alloc_gran = 65536;
    }
    if (LIBC_UNLIKELY(max_address < alloc_gran))
      __builtin_trap();
    return max_address & ~(alloc_gran - 1);
  }

  LIBC_INLINE static size_t required_l1_size(uintptr_t max_view_base) {
    return l1_index(radix_key(max_view_base)) + 1;
  }

  LIBC_INLINE static size_t l1_bytes_for(size_t l1_size) {
    if (LIBC_UNLIKELY(l1_size == 0 ||
                      l1_size > SIZE_MAX / sizeof(cpp::Atomic<L2Page *>)))
      __builtin_trap();
    return l1_size * sizeof(cpp::Atomic<L2Page *>);
  }

  // Decrement the in-flight remap counter. On transition to zero we
  // wake any reconciliation thread parked in wait_for_remap_drain() and
  // run try_shrink_guards() to release the guard array's tail.
  //
  // SEQ_CST on the RMW so the count change is globally ordered against
  // the VEH's ACQUIRE load — a VEH that observes count == 0 must not see
  // a stale REMAPPING slot from before the decrement.
  LIBC_INLINE void release_remap_count() {
    if (state().active_remap_count_.fetch_sub(1, cpp::MemoryOrder::SEQ_CST) == 1) {
      try_shrink_guards();
      state().active_remap_count_.notify_all();
    }
  }

  // Reset the mutable bookkeeping. The PCB-resident handles
  // (mapping_table_l1_ / l1_size_ / max_view_base_ / remap_guards_) live
  // in sealed Zone 0 and CANNOT be cleared here — Zone 0 is
  // PAGE_READONLY for the process lifetime, so any write would AV. The
  // table is not re-initialised in normal operation: fork inherits via
  // CoW, exec preserves the VAs, and destroy() runs at process fini
  // when the surviving stale pointer is irrelevant.
  LIBC_INLINE void reset_root_state() {
    state().guard_region_.destroy();
    state().active_remap_count_.store(0, cpp::MemoryOrder::RELAXED);
    state().forced_write_recoveries_.store(0, cpp::MemoryOrder::RELAXED);
    state().guard_high_water_.store(0, cpp::MemoryOrder::RELAXED);
    state().alloc_cursor_.store(0, cpp::MemoryOrder::RELAXED);
  }

  // ---- Radix navigation --------------------------------------------------

  LIBC_INLINE Slot *find_slot(uintptr_t view_base) {
    if (LIBC_UNLIKELY(!ensure_init() || view_base > pcb_max_view_base()))
      return nullptr;
    uintptr_t k = radix_key(view_base);
    L2Page *l2 = pcb_l1()[l1_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l2))
      return nullptr;
    L3Page *l3 = l2->children[l2_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l3))
      return nullptr;
    return &l3->slots[l3_index(k)];
  }

  // find_slot + L3 page pointer, for callers that want occupancy-bit ops.
  LIBC_INLINE Slot *find_slot_with_l3(uintptr_t view_base, L3Page **l3_out,
                                      unsigned *i3_out) {
    if (LIBC_UNLIKELY(!ensure_init() || view_base > pcb_max_view_base()))
      return nullptr;
    uintptr_t k = radix_key(view_base);
    L2Page *l2 = pcb_l1()[l1_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l2))
      return nullptr;
    L3Page *l3 = l2->children[l2_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l3))
      return nullptr;
    unsigned i3 = static_cast<unsigned>(l3_index(k));
    *l3_out = l3;
    *i3_out = i3;
    return &l3->slots[i3];
  }

  // Radix backing allocations — L1 / L2 / L3 pages are carved out of
  // the VA substrate's placeholder-backed sub-slot pools:
  //
  //   L1 directory      (l1_bytes_for(l1_size), up to 16 KB)  → Small
  //   L2 radix page     (sizeof(L2Page) = 8 KB)               → Small
  //   L3 radix page     (sizeof(L3Page) = 64 KB + 128 B)      → Large
  //
  // The substrate seed arenas are registered as LIBC_INTERNAL in the
  // mapping table by the walker's Pass 2 (see memory_primitives_
  // bootstrap.cpp). Every later substrate arena auto-registers itself
  // via `reserve_new_arena`'s `g_mapping_table_ready` branch, so any
  // L2/L3 allocated post-bootstrap lives inside a table-covered arena
  // without the mapping-table code having to chase it. The guard
  // region below uses CommitRegion directly — it is a separate
  // reserve-then-commit VA region with its own growth policy, not a
  // substrate consumer.

  LIBC_INLINE Slot *ensure_slot(uintptr_t view_base) {
    if (LIBC_UNLIKELY(!ensure_init() || view_base > pcb_max_view_base()))
      __builtin_trap();
    uintptr_t k = radix_key(view_base);
    size_t i1 = l1_index(k);
    int i2 = l2_index(k), i3 = l3_index(k);

    // L2 radix page (8 KB) lives in a substrate Small slot (16 KB). The
    // slot-size waste is acceptable; L2 page count scales with populated
    // L1 directory fanout, bounded in practice. Losing the publish CAS
    // releases the slot back to the substrate so concurrent contention
    // does not leak arena capacity.
    L2Page *l2 = pcb_l1()[i1].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l2)) {
      alloc::SubSlotHandle l2_handle =
          alloc::substrate_acquire(alloc::SubSlotClass::Small,
                                    alloc::ConsumerTag::MappingTable);
      if (LIBC_UNLIKELY(!l2_handle))
        __builtin_trap();
      auto *fresh = static_cast<L2Page *>(l2_handle.ptr());
      L2Page *expected = nullptr;
      if (!pcb_l1()[i1].compare_exchange_strong(expected, fresh,
                                                cpp::MemoryOrder::RELEASE,
                                                cpp::MemoryOrder::ACQUIRE)) {
        alloc::substrate_release(l2_handle);
        l2 = expected;
      } else {
        // Winner: keep the slot for the process lifetime. The handle
        // is deliberately discarded — substrate::destroy() reclaims the
        // whole arena at fini.
        l2 = fresh;
      }
    }

    // L3 radix page (65664 B = 64 KB + 128 B occupancy bitmap) lives in
    // a substrate Large slot (128 KB). Medium (64 KB exact) is 128 B
    // short of L3Page's size; Large is the smallest class that fits.
    L3Page *l3 = l2->children[i2].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l3)) {
      alloc::SubSlotHandle l3_handle =
          alloc::substrate_acquire(alloc::SubSlotClass::Large,
                                    alloc::ConsumerTag::MappingTable);
      if (LIBC_UNLIKELY(!l3_handle))
        __builtin_trap();
      auto *fresh = static_cast<L3Page *>(l3_handle.ptr());
      L3Page *expected = nullptr;
      if (!l2->children[i2].compare_exchange_strong(expected, fresh,
                                                    cpp::MemoryOrder::RELEASE,
                                                    cpp::MemoryOrder::ACQUIRE)) {
        alloc::substrate_release(l3_handle);
        l3 = expected;
      } else {
        l3 = fresh;
      }
    }

    return &l3->slots[i3];
  }

  // ---- Init ---------------------------------------------------------------
  //
  // Public so the Tier A bootstrap (mapping_table_startup_init in
  // mapping_table.cpp) can drive eager initialisation. Internal lazy
  // call sites (find_slot, ensure_slot, walk_range, ...) re-enter
  // through the same idempotent CAS gate; the eager Tier A call wins
  // the gate first and every later caller takes the INIT_READY fast
  // path.

public:
  // -----------------------------------------------------------------------
  // Radix-page accessors.
  // -----------------------------------------------------------------------
  //
  // `L2Page_size()` / `L3Page_size()` / `guard_reserve_bytes()` surface
  // internal layout so external callers can reason about radix-page
  // dimensions without naming the private `L2Page` / `L3Page` types.
  // `meta_stitch_l2` / `meta_stitch_l3` remain public so a future
  // integrated RadixStore can publish L2/L3 pages without going through
  // `ensure_slot` — no current caller.

  LIBC_INLINE static size_t L2Page_size() { return sizeof(L2Page); }
  LIBC_INLINE static size_t L3Page_size() { return sizeof(L3Page); }
  LIBC_INLINE static SIZE_T guard_reserve_bytes() {
    return GUARD_RESERVE_BYTES;
  }

  /// Install `embedded_l2` at `L1[i1(chunk_base)]` if null. Returns the
  /// L2Page pointer now in that slot (the caller's `embedded_l2` on
  /// publish, or the existing L2 on CAS miss). `embedded_l2` must be a
  /// zero-initialized `L2Page`-sized buffer at the chunk's in-range
  /// offset.
  LIBC_INLINE void *meta_stitch_l2(uintptr_t chunk_base, void *embedded_l2) {
    auto *slot_ptr =
        pcb_l1() + l1_index(radix_key(chunk_base));
    auto *emb = static_cast<L2Page *>(embedded_l2);
    L2Page *existing = slot_ptr->load(cpp::MemoryOrder::ACQUIRE);
    if (existing != nullptr)
      return existing;
    L2Page *expected = nullptr;
    if (slot_ptr->compare_exchange_strong(expected, emb,
                                          cpp::MemoryOrder::RELEASE,
                                          cpp::MemoryOrder::ACQUIRE))
      return emb;
    return expected;
  }

  /// Install `embedded_l3` at the appropriate L2 child slot if null.
  /// `l2` must be the L2Page the caller just resolved via
  /// `meta_stitch_l2(chunk_base, ...)`.
  LIBC_INLINE void *meta_stitch_l3(uintptr_t chunk_base, void *l2,
                                   void *embedded_l3) {
    auto *l2_page = static_cast<L2Page *>(l2);
    auto *emb = static_cast<L3Page *>(embedded_l3);
    const int i2 = l2_index(radix_key(chunk_base));
    L3Page *existing =
        l2_page->children[i2].load(cpp::MemoryOrder::ACQUIRE);
    if (existing != nullptr)
      return existing;
    L3Page *expected = nullptr;
    if (l2_page->children[i2].compare_exchange_strong(
            expected, emb, cpp::MemoryOrder::RELEASE,
            cpp::MemoryOrder::ACQUIRE))
      return emb;
    return expected;
  }

  /// Cheap read-only check: true iff the table has reached INIT_READY and
  /// is not destroyed. Lets callers (e.g. ThreadScratch's bootstrap-tier
  /// path) gate inline `register_mapping_internal` calls against the
  /// risk of self-deadlocking on a same-thread reentrant `ensure_init`
  /// when the table is INIT_IN_PROGRESS. ACQUIRE so a true return
  /// transitively makes the table's published state visible.
  [[nodiscard]] LIBC_INLINE bool is_init_ready() const {
    return state().init_state_.load(cpp::MemoryOrder::ACQUIRE) == INIT_READY;
  }

  LIBC_INLINE bool ensure_init() {
    FutexValueType cur = state().init_state_.load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_LIKELY(cur == INIT_READY))
      return true;
    if (LIBC_UNLIKELY(cur == INIT_DESTROYED))
      return false;
    if (cur == INIT_UNINITIALIZED &&
        state().init_state_.compare_exchange_strong(cur, INIT_IN_PROGRESS,
                                            cpp::MemoryOrder::ACQUIRE)) {
      uintptr_t max_view_base = runtime_max_view_base();
      size_t l1_size = required_l1_size(max_view_base);

      // Publish the geometry + L1 root through the Zone 0 write gate.
      // Order matters: l1_size_ / max_view_base_ are read by callers
      // gated on init_state_ == INIT_READY, so they must be visible
      // before the publish below. The PCB writes happen pre-seal under
      // single-thread bring-up — no atomic ordering is required between
      // them, only the store_and_notify_all(INIT_READY) at the bottom
      // RELEASEs the whole batch.
      //
      // L1 backing comes from the VA substrate (class Small, 16 KB slot).
      // l1_bytes_for(l1_size) = 2048 * 8 = 16 KB on x86-64 with NT's
      // MaximumUserModeAddress (0x7FFFFFFEFFFF) — fits a Small slot
      // exactly. The substrate seed arenas are guaranteed alive at this
      // point: va_substrate's `.libcmem$P1` handler ran before
      // mapping_table's `$P2` handler. The handle is intentionally
      // discarded — the L1 directory lives for the entire process
      // lifetime; at fini, va_substrate::destroy() MEM_RELEASEs the
      // whole arena and the L1 sub-slot goes with it.
      LIBC_ASSERT(l1_bytes_for(l1_size) <=
                      alloc::VaSubstrate::slot_size(
                          alloc::SubSlotClass::Small) &&
                  "L1 directory exceeds substrate Small slot size");
      alloc::SubSlotHandle l1_handle =
          alloc::substrate_acquire(alloc::SubSlotClass::Small,
                                    alloc::ConsumerTag::MappingTable);
      if (LIBC_UNLIKELY(!l1_handle))
        __builtin_trap();
      void *l1_storage = l1_handle.ptr();
      internal::PcbInitAccess::set_mapping_table_l1(l1_storage);
      internal::PcbInitAccess::set_mapping_table_l1_size(l1_size);
      internal::PcbInitAccess::set_mapping_table_max_view_base(max_view_base);

      if (LIBC_UNLIKELY(!state().guard_region_.init(GUARD_RESERVE_BYTES)))
        __builtin_trap();
      if (LIBC_UNLIKELY(!state().guard_region_.ensure_committed(GUARD_RESERVE_BYTES)))
        __builtin_trap();
      internal::PcbInitAccess::set_mapping_table_remap_guards(
          state().guard_region_.as<cpp::Atomic<uintptr_t>>());

      pin_critical_pages();

      // Make sure the RegionPool is live before the first register_*.
      memory::g_region_pool.init();

      state().init_state_.store_and_notify_all(INIT_READY);
      return true;
    }
    while ((cur = state().init_state_.load(cpp::MemoryOrder::ACQUIRE)) ==
           INIT_IN_PROGRESS) {
      long ret = state().init_state_.wait(INIT_IN_PROGRESS);
      // Yield on transient wait failure (-ENOMEM from wait-slot pool
      // exhaustion) so we don't spin-fail hot waiting for init to
      // complete. The init publisher will still wake us via the next
      // store_and_notify_all — we just need to not burn CPU.
      if (ret < 0 && ret != -EINTR)
        ::NtYieldExecution();
    }
    return cur == INIT_READY;
  }

private:
  LIBC_INLINE void pin_critical_pages() {
    cpp::Atomic<L2Page *> *l1 = pcb_l1();
    size_t l1_size = pcb_l1_size();
    if (!l1 || l1_size == 0)
      return;
    PVOID base = l1;
    SIZE_T size = l1_bytes_for(l1_size);
    ::NtLockVirtualMemory(NtCurrentProcess(), &base, &size, MAP_PROCESS);
  }

  // ---- Thread liveness --------------------------------------------------

  LIBC_INLINE static bool is_thread_dead(HANDLE h) {
    if (!h)
      return true;
    LARGE_INTEGER zero_timeout = {};
    NTSTATUS st = ::NtWaitForSingleObject(h, 0, &zero_timeout);
    return st == /*STATUS_WAIT_0*/ 0;
  }

  LIBC_INLINE static HANDLE open_current_thread_handle() {
    HANDLE h = nullptr;
    ::NtDuplicateObject(NtCurrentProcess(), NtCurrentThread(),
                        NtCurrentProcess(), &h,
                        SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, 0, 0);
    return h;
  }

  // ---- Slot publication --------------------------------------------------
  //
  // Every WRITING→LIVE/FREE/PLACEHOLDER/FOREIGN transition goes through
  // publish_slot(), which bumps the version (RELEASE store) before storing
  // the new key. Readers pair with ACQUIRE-order version load + post-fence
  // version re-check to detect overlapping mutations.

  LIBC_INLINE static void publish_slot(Slot *s, uintptr_t new_key) {
    s->version.fetch_add(1, cpp::MemoryOrder::RELEASE);
    s->key.store(new_key, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&s->key, UINT32_MAX);
  }

  // ---- Occupancy bitmap helpers -----------------------------------------

  LIBC_INLINE static void set_occupancy(L3Page *l3, unsigned i3) {
    l3->occupancy[i3 / 64].fetch_or(1ULL << (i3 % 64),
                                    cpp::MemoryOrder::RELEASE);
  }
  LIBC_INLINE static void clear_occupancy(L3Page *l3, unsigned i3) {
    l3->occupancy[i3 / 64].fetch_and(~(1ULL << (i3 % 64)),
                                     cpp::MemoryOrder::RELEASE);
  }

  LIBC_INLINE void set_occupancy_bit(uintptr_t view_base) {
    uintptr_t k = radix_key(view_base);
    L2Page *l2 = pcb_l1()[l1_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l2))
      return;
    L3Page *l3 = l2->children[l2_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l3))
      return;
    set_occupancy(l3, static_cast<unsigned>(l3_index(k)));
  }

  LIBC_INLINE void clear_occupancy_bit(uintptr_t view_base) {
    uintptr_t k = radix_key(view_base);
    L2Page *l2 = pcb_l1()[l1_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l2))
      return;
    L3Page *l3 = l2->children[l2_index(k)].load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(!l3))
      return;
    clear_occupancy(l3, static_cast<unsigned>(l3_index(k)));
  }

  // ---- Slot field helpers -----------------------------------------------

  LIBC_INLINE static void clear_slot_fields(Slot *s) {
    s->region_id = 0;
    s->alloc_id = 0;
    s->state_aux = 0;
    s->view_prot = 0;
    s->flags.store(0, cpp::MemoryOrder::RELAXED);
    s->extent = 0;
  }

  // FOREIGN-specific clear: transitions FOREIGN[+stale] → WRITING → FREE,
  // dropping the cordon entirely. No region reference to release (FOREIGN
  // slots either point at the FOREIGN_SENTINEL region or carry region_id
  // == NONE; the sentinel's refcount is not adjusted by per-slot teardown
  // because cordon stamps are stateless from the pool's perspective).
  LIBC_INLINE void clear_foreign_slot(Slot *slot, uintptr_t target) {
    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (!is_foreign(k) || (k & ~STATE_MASK) != target)
        return; // Someone else cleared or repurposed it.
      uintptr_t expected = k;
      if (!slot->key.compare_exchange_strong(expected, target | WRITING_BIT,
                                             cpp::MemoryOrder::ACQUIRE))
        continue;
      stamp_writing_owner(slot);
      clear_slot_fields(slot);
      publish_slot(slot, KEY_FREE);
      clear_occupancy_bit(target);
      return;
    }
  }

  // Zero only the REMAPPING-specific cold-union fields. The union is
  // dual-purposed by state; we zero the fields relevant to the state we're
  // leaving.
  LIBC_INLINE static void clear_remap_fields(Slot *s) {
    s->remap_owner_thread = nullptr;
    s->remap_guard_index = -1;
  }

  // ---- WRITING owner tracking -------------------------------------------

  LIBC_INLINE static uint32_t system_time_lo() {
    return static_cast<uint32_t>(
        *reinterpret_cast<const volatile ULONGLONG *>(0x7FFE0014ULL));
  }

  static constexpr uint32_t WRITING_LIVENESS_THRESHOLD = 1000000; // 100ms

  LIBC_INLINE static void stamp_writing_owner(Slot *s) {
    s->writing_owner_tid = NtCurrentThreadId();
    s->writing_start_time = system_time_lo();
  }

  LIBC_INLINE static bool is_writing_owner_dead(Slot *s) {
    DWORD tid = s->writing_owner_tid;
    if (tid == 0)
      return true;
    CLIENT_ID cid = {};
    cid.UniqueThread = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(tid));
    auto oa = internal_oa();
    HANDLE h = nullptr;
    NTSTATUS st = ::NtOpenThread(
        &h, SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, &oa, &cid);
    if (!NT_SUCCESS(st))
      return true;

    bool dead = is_thread_dead(h);
    if (dead) {
      ::NtClose(h);
      return true;
    }

    KERNEL_USER_TIMES times;
    st = ::NtQueryInformationThread(h, ThreadTimes, &times, sizeof(times),
                                    nullptr);
    ::NtClose(h);

    if (NT_SUCCESS(st)) {
      uint32_t create_lo = static_cast<uint32_t>(times.CreateTime.QuadPart);
      uint32_t delta = create_lo - s->writing_start_time;
      if (delta > 0 && delta < 0x80000000u)
        return true;
    }
    return false;
  }

  // Recover a WRITING slot whose owner is confirmed dead. The slot's
  // region ref (if any had been published) is released on the way out.
  LIBC_INLINE bool recover_dead_write(Slot *s, uintptr_t current_key) {
    uintptr_t expected = current_key;
    if (!s->key.compare_exchange_strong(expected, KEY_FREE,
                                        cpp::MemoryOrder::ACQ_REL))
      return false;
    uint32_t rid = s->region_id;
    clear_slot_fields(s);
    state().forced_write_recoveries_.fetch_add(1, cpp::MemoryOrder::RELAXED);
    s->version.fetch_add(1, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&s->key, UINT32_MAX);
    clear_occupancy_bit(current_key & ~STATE_MASK);
    if (rid != memory::RegionPool::NONE)
      memory::g_region_pool.release(rid);
    return true;
  }

  LIBC_INLINE void wait_for_writing(Slot *s, uintptr_t writing_key) {
    uint32_t start_time = system_time_lo();
    bool past_threshold = false;

    for (;;) {
      uintptr_t k = s->key.load(cpp::MemoryOrder::ACQUIRE);
      if (k != writing_key)
        return;

      if (!past_threshold) {
        uint32_t elapsed = system_time_lo() - start_time;
        if (elapsed >= WRITING_LIVENESS_THRESHOLD)
          past_threshold = true;
      }

      if (past_threshold && is_writing_owner_dead(s)) {
        recover_dead_write(s, writing_key);
        return;
      }

      wait_for_slot(s, writing_key);
    }
  }

  // ---- Slot wait / recovery ---------------------------------------------

  LIBC_INLINE static void wait_for_slot(Slot *s, uintptr_t current_key) {
    LARGE_INTEGER timeout;
    timeout.QuadPart = SLOT_WAIT_100NS;
    futex_addr::wait_nt(
        reinterpret_cast<const volatile uintptr_t *>(&s->key),
        current_key, &timeout);
  }

  LIBC_INLINE bool recover_dead_remap(Slot *s, uintptr_t current_key) {
    uintptr_t expected = current_key;
    if (!s->key.compare_exchange_strong(expected, KEY_FREE,
                                        cpp::MemoryOrder::ACQ_REL))
      return false;
    uint32_t rid = s->region_id;
    HANDLE owner = s->remap_owner_thread;
    int gi = s->remap_guard_index;
    clear_slot_fields(s);
    clear_remap_fields(s);
    if (owner)
      ::NtClose(owner);
    release_guard(gi);
    release_remap_count();
    s->version.fetch_add(1, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&s->key, UINT32_MAX);
    clear_occupancy_bit(current_key & ~STATE_MASK);
    if (rid != memory::RegionPool::NONE)
      memory::g_region_pool.release(rid);
    return true;
  }

  LIBC_INLINE void try_recover_slot(Slot *s, uintptr_t k) {
    if ((k & REMAPPING_BIT) && is_thread_dead(s->remap_owner_thread))
      recover_dead_remap(s, k);
  }

  // ---- Remap guard array ------------------------------------------------

  LIBC_INLINE uint32_t expand_guards() {
    uint32_t idx = state().guard_high_water_.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
    if (LIBC_UNLIKELY(idx >= MAX_GUARDS)) {
      state().guard_high_water_.fetch_sub(1, cpp::MemoryOrder::RELAXED);
      return MAX_GUARDS;
    }
    return idx;
  }

  LIBC_INLINE void ensure_hwm_covers(uint32_t min_hwm) {
    uint32_t hwm = state().guard_high_water_.load(cpp::MemoryOrder::ACQUIRE);
    while (hwm < min_hwm) {
      if (state().guard_high_water_.compare_exchange_weak(hwm, min_hwm,
                                                  cpp::MemoryOrder::RELEASE,
                                                  cpp::MemoryOrder::ACQUIRE))
        return;
    }
  }

  LIBC_INLINE int claim_guard(Slot *slot) {
    cpp::Atomic<uintptr_t> *guards = pcb_remap_guards();
    if (LIBC_UNLIKELY(!guards))
      return -1;
    uintptr_t encoded = reinterpret_cast<uintptr_t>(slot);

    for (;;) {
      uint32_t hwm = state().guard_high_water_.load(cpp::MemoryOrder::ACQUIRE);

      if (hwm > 0) {
        uint32_t start =
            state().alloc_cursor_.fetch_add(1, cpp::MemoryOrder::RELAXED) % hwm;
        for (uint32_t i = 0; i < hwm; ++i) {
          uint32_t gi = (start + i) % hwm;
          uintptr_t expected = 0;
          if (guards[gi].compare_exchange_strong(
                  expected, encoded, cpp::MemoryOrder::RELEASE,
                  cpp::MemoryOrder::RELAXED)) {
            ensure_hwm_covers(gi + 1);
            return static_cast<int>(gi);
          }
        }
      }

      uint32_t gi = expand_guards();
      if (LIBC_LIKELY(gi < MAX_GUARDS)) {
        guards[gi].store(encoded, cpp::MemoryOrder::RELEASE);
        ensure_hwm_covers(gi + 1);
        return static_cast<int>(gi);
      }

      uint32_t recycled = sweep_dead_guards();
      if (recycled > 0)
        continue;

      return -1;
    }
  }

  LIBC_INLINE uint32_t sweep_dead_guards() {
    cpp::Atomic<uintptr_t> *guards = pcb_remap_guards();
    uint32_t hwm = state().guard_high_water_.load(cpp::MemoryOrder::ACQUIRE);
    uint32_t recycled = 0;

    for (uint32_t i = 0; i < hwm; ++i) {
      uintptr_t ptr = guards[i].load(cpp::MemoryOrder::ACQUIRE);
      if (!ptr)
        continue;

      Slot *slot = reinterpret_cast<Slot *>(ptr);
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);

      if (!(k & REMAPPING_BIT)) {
        uintptr_t expected = ptr;
        if (guards[i].compare_exchange_strong(expected, 0,
                                              cpp::MemoryOrder::RELEASE,
                                              cpp::MemoryOrder::RELAXED))
          ++recycled;
        continue;
      }

      if (is_thread_dead(slot->remap_owner_thread)) {
        if (recover_dead_remap(slot, k))
          ++recycled;
      }
    }
    return recycled;
  }

  LIBC_INLINE void release_guard(int guard_index) {
    if (LIBC_UNLIKELY(guard_index < 0))
      return;
    pcb_remap_guards()[guard_index].store(0, cpp::MemoryOrder::RELEASE);
  }

  LIBC_INLINE void try_shrink_guards() {
    uint32_t hwm = state().guard_high_water_.load(cpp::MemoryOrder::ACQUIRE);
    if (hwm == 0)
      return;
    if (state().active_remap_count_.get_value(cpp::MemoryOrder::ACQUIRE) != 0)
      return;

    cpp::Atomic<uintptr_t> *guards = pcb_remap_guards();
    uint32_t new_hwm = 0;
    for (uint32_t i = 0; i < hwm; ++i) {
      if (guards[i].load(cpp::MemoryOrder::ACQUIRE) != 0)
        new_hwm = i + 1;
    }

    if (new_hwm < hwm) {
      state().guard_high_water_.compare_exchange_strong(hwm, new_hwm,
                                                cpp::MemoryOrder::RELEASE,
                                                cpp::MemoryOrder::RELAXED);
    }
  }

public:
  // =======================================================================
  // Public API
  // =======================================================================

  /// Number of in-progress remap operations. The memory-fault VEH handler
  /// checks this first — zero means no remap guard scan is needed.
  LIBC_INLINE FutexValueType active_remap_count() {
    return state().active_remap_count_.get_value(cpp::MemoryOrder::ACQUIRE);
  }

  /// Park the calling thread until every in-flight remap has drained
  /// (active_remap_count_ reaches zero). Used by reconciliation paths
  /// (post-fork, post-exec, post-dlopen) that need a quiet table to
  /// scan against NT's view of the VA space.
  ///
  /// Caller MUST hold MmapLock writer so that no NEW remap can begin
  /// after the count first reaches zero. Without that guarantee the
  /// drain has no terminating condition.
  ///
  /// Uses wait_on_predicate: ret == 0 guarantees v == 0 held at some
  /// point during the wait. The notify contract is satisfied by
  /// release_remap_count's fetch_sub(..., SEQ_CST) == 1 → notify_all
  /// on the 1→0 edge.
  ///
  /// Error return contract: the only non-zero return path is -ENOMEM
  /// from wait-slot pool exhaustion (not Interruptible, no timeout,
  /// no -EINVAL possible — the caller holds the writer lock so the
  /// futex cannot be destroyed). Pool exhaustion at a MmapLock-writer
  /// boundary is catastrophic — the reconciliation protocol has no
  /// retry semantics — so trap rather than spin.
  LIBC_INLINE void wait_for_remap_drain() {
    long ret = state().active_remap_count_.wait_on_predicate(&remap_count_is_zero, 0);
    if (LIBC_UNLIKELY(ret != 0))
      __builtin_trap();
  }

  /// Diagnostic: forced-recovery count for WRITING slots.
  LIBC_INLINE uint32_t get_forced_write_recoveries() {
    return state().forced_write_recoveries_.load(cpp::MemoryOrder::RELAXED);
  }

  // -----------------------------------------------------------------------
  // Slot publishing — caller owns the region reference being installed.
  // -----------------------------------------------------------------------
  //
  // Each register_* consumes ONE reference from the caller's region
  // refcount. On success, the slot now holds that reference; on failure
  // the caller retains it and must release as usual.

  /// Publish a LIVE slot covering [view_base, view_base + view_size) that
  /// belongs to `region_id` (with `alloc_id` captured at acquire time).
  /// Returns false only on radix-tree OOM or on state machine inconsistency
  /// (a concurrent registration won the address). Caller still owns the
  /// region ref on failure.
  [[nodiscard]] LIBC_INLINE bool register_mapping(void *view_base, SIZE_T view_size,
                                      uint32_t region_id, uint8_t alloc_id,
                                      DWORD prot, DWORD flags) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = ensure_slot(target);

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);

      if (k == KEY_FREE || is_placeholder(k) || is_foreign(k)) {
        if (is_placeholder(k) && (k & ~STATE_MASK) != target)
          return false;
        uintptr_t expected = k;
        if (!slot->key.compare_exchange_strong(expected, target | WRITING_BIT,
                                               cpp::MemoryOrder::ACQUIRE))
          continue;

        // We own the slot. If we took over a PLACEHOLDER / FOREIGN, the
        // old region_id (if any) is released now.
        uint32_t old_rid = slot->region_id;
        stamp_writing_owner(slot);
        slot->region_id = region_id;
        slot->alloc_id = alloc_id;
        slot->state_aux = 0;
        slot->extent = view_size;
        slot->view_prot = prot;
        slot->flags.store(flags, cpp::MemoryOrder::RELAXED);
        set_occupancy_bit(target);
        publish_slot(slot, live_key(target));
        if (old_rid != memory::RegionPool::NONE && old_rid != region_id)
          memory::g_region_pool.release(old_rid);
        return true;
      }

      if ((k & ~STATE_MASK) == target && is_live(k)) {
        // Re-register over an existing LIVE slot: replace the region ref.
        uintptr_t expected = k;
        if (!slot->key.compare_exchange_strong(expected, target | WRITING_BIT,
                                               cpp::MemoryOrder::ACQUIRE))
          continue;
        uint32_t old_rid = slot->region_id;
        stamp_writing_owner(slot);
        slot->region_id = region_id;
        slot->alloc_id = alloc_id;
        slot->state_aux = 0;
        slot->extent = view_size;
        slot->view_prot = prot;
        slot->flags.store(flags, cpp::MemoryOrder::RELAXED);
        publish_slot(slot, live_key(target));
        if (old_rid != memory::RegionPool::NONE && old_rid != region_id)
          memory::g_region_pool.release(old_rid);
        return true;
      }

      if (k & WRITING_BIT) {
        wait_for_writing(slot, k);
        continue;
      }

      try_recover_slot(slot, k);
      wait_for_slot(slot, k);
    }
  }

  /// Publish a PLACEHOLDER slot for a VA range that's reserved but not
  /// mapped. region_id identifies the owning region; refcount must already
  /// include this placeholder's reference.
  [[nodiscard]] LIBC_INLINE bool register_placeholder(void *view_base, SIZE_T extent,
                                          uint32_t region_id,
                                          uint8_t alloc_id) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = ensure_slot(target);

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);

      if (k == KEY_FREE) {
        uintptr_t expected = KEY_FREE;
        if (!slot->key.compare_exchange_strong(expected, target | WRITING_BIT,
                                               cpp::MemoryOrder::ACQUIRE))
          continue;
        stamp_writing_owner(slot);
        slot->region_id = region_id;
        slot->alloc_id = alloc_id;
        slot->state_aux = 0;
        slot->extent = extent;
        slot->view_prot = 0;
        slot->flags.store(0, cpp::MemoryOrder::RELAXED);
        set_occupancy_bit(target);
        publish_slot(slot, placeholder_key(target));
        return true;
      }

      if (k & WRITING_BIT) {
        wait_for_writing(slot, k);
        continue;
      }

      // Slot already occupied in some other state — caller expected free VA.
      return false;
    }
  }

  /// Stamp a FOREIGN slot over a range the table did not own. Used after
  /// an MRI_Ex probe when we discover NT-owned VA that's not ours.
  /// region_id may be 0 (the FOREIGN_SENTINEL region in the pool has a
  /// single shared descriptor) or a pool-allocated cordon region if the
  /// caller wants to track refcount.
  ///
  /// Returns true when the FOREIGN stamp lands (slot was FREE or already
  /// FOREIGN), false when the slot is currently LIVE/PLACEHOLDER/
  /// REMAPPING — i.e. some prior call asserted ownership of this VA.
  /// Callers (the alloc_fixed_anon retry loop, post-fork reconcile, the
  /// MAP_FIXED guard path) use the return value to distinguish "cordon
  /// applied" from "address belongs to one of our regions, do not treat
  /// as foreign".
  [[nodiscard]] LIBC_INLINE bool register_foreign(void *view_base,
                                                  SIZE_T extent,
                                                  uint32_t region_id,
                                                  uint8_t alloc_id) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = ensure_slot(target);

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);

      if (k == KEY_FREE || is_foreign(k)) {
        uintptr_t expected = k;
        if (!slot->key.compare_exchange_strong(expected, target | WRITING_BIT,
                                               cpp::MemoryOrder::ACQUIRE))
          continue;
        stamp_writing_owner(slot);
        slot->region_id = region_id;
        slot->alloc_id = alloc_id;
        slot->state_aux = 0;
        slot->extent = extent;
        slot->view_prot = 0;
        slot->flags.store(0, cpp::MemoryOrder::RELAXED);
        if (k == KEY_FREE)
          set_occupancy_bit(target);
        publish_slot(slot, foreign_key(target));
        return true;
      }

      if (k & WRITING_BIT) {
        wait_for_writing(slot, k);
        continue;
      }

      // Occupied by a LIVE/PLACEHOLDER/REMAPPING slot — not foreign.
      return false;
    }
  }

  // -----------------------------------------------------------------------
  // Sentinel fast-path registrations.
  // -----------------------------------------------------------------------
  //
  // LIBC_INTERNAL / IMAGE_REGION / KERNEL_REGION / FOREIGN_SENTINEL are
  // shapes whose descriptors live in four pre-reserved `RegionPool` slots
  // (ids 1..4). No per-registration `RegionPool::acquire` call is made;
  // the slot is published straight against the appropriate sentinel id.
  // This keeps each registration down to a single CAS + version bump
  // (~30–50 ns), which matters because `register_mapping_internal` is on
  // the `page_alloc` hot path.
  //
  // All four wrappers look up the sentinel's current alloc_id once per
  // call (ACQUIRE load on the pool's `alloc_id` field). If a prior
  // init/reset bumped it, the sentinel appears fresh to any snapshot
  // reader that captured an older generation.

private:
  LIBC_INLINE bool register_sentinel_live(void *view_base, SIZE_T view_size,
                                          uint32_t sentinel_region_id) {
    const memory::RegionDesc *sdesc =
        memory::g_region_pool.sentinel_desc(sentinel_region_id);
    if (LIBC_UNLIKELY(sdesc == nullptr))
      return false;
    const uint8_t aid = sdesc->get_alloc_id();
    return register_mapping(view_base, view_size, sentinel_region_id, aid,
                            /*prot=*/0, /*flags=*/0);
  }

  LIBC_INLINE bool register_sentinel_foreign(void *view_base, SIZE_T extent,
                                             uint32_t sentinel_region_id) {
    const memory::RegionDesc *sdesc =
        memory::g_region_pool.sentinel_desc(sentinel_region_id);
    if (LIBC_UNLIKELY(sdesc == nullptr))
      return false;
    const uint8_t aid = sdesc->get_alloc_id();
    return register_foreign(view_base, extent, sentinel_region_id, aid);
  }

public:
  /// Stamp [base, base+size) as `LIBC_INTERNAL`. Used by the registering
  /// `page_alloc` / `page_reserve` / `page_commit` wrappers.
  [[nodiscard]] LIBC_INLINE bool register_mapping_internal(void *base,
                                                           SIZE_T size) {
    return register_sentinel_live(base, size,
                                  memory::RegionPool::INTERNAL_REGION_ID);
  }

  /// Stamp [base, base+size) as `IMAGE_REGION`. Used by the DLL-load
  /// notification callback and the initial `discover_loaded_modules`
  /// sweep.
  [[nodiscard]] LIBC_INLINE bool register_mapping_image(void *base,
                                                        SIZE_T size) {
    return register_sentinel_live(base, size,
                                  memory::RegionPool::IMAGE_SENTINEL_REGION_ID);
  }

  /// Stamp [base, base+size) as `KERNEL_REGION`. Used by the
  /// TEB/PEB/stack discovery at Phase 0c.5.
  [[nodiscard]] LIBC_INLINE bool register_mapping_kernel(void *base,
                                                         SIZE_T size) {
    return register_sentinel_live(
        base, size, memory::RegionPool::KERNEL_SENTINEL_REGION_ID);
  }

  /// Stamp [base, base+size) as `FOREIGN_SENTINEL` (proper FOREIGN slot
  /// state, not LIVE — preserves the existing `revalidate_foreign`
  /// machinery). Used by the bulk-VAD scan in `discover_foreign_regions`
  /// and by on-demand reconciliation.
  [[nodiscard]] LIBC_INLINE bool register_mapping_foreign(void *base,
                                                          SIZE_T size) {
    return register_sentinel_foreign(
        base, size, memory::RegionPool::FOREIGN_SENTINEL_REGION_ID);
  }

  // -----------------------------------------------------------------------
  // Flag operations.
  // -----------------------------------------------------------------------

  LIBC_INLINE void add_flags(void *view_base, DWORD new_flags) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (k == KEY_FREE)
        return;

      if (k & WRITING_BIT) {
        wait_for_writing(slot, k);
        continue;
      }

      if ((k & ~STATE_MASK) != target)
        return;

      slot->flags.fetch_or(new_flags, cpp::MemoryOrder::RELEASE);

      uintptr_t k_after = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (LIBC_UNLIKELY(k_after == KEY_FREE ||
                        (k_after & ~STATE_MASK) != target))
        slot->flags.fetch_and(~new_flags, cpp::MemoryOrder::RELAXED);
      return;
    }
  }

  LIBC_INLINE void remove_flags(void *view_base, DWORD clear_flags_mask) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;
    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (k == KEY_FREE || (k & WRITING_BIT))
      return;
    if ((k & ~STATE_MASK) != target)
      return;
    slot->flags.fetch_and(~clear_flags_mask, cpp::MemoryOrder::RELEASE);
  }

  LIBC_INLINE void set_numa_interleave(void *view_base, DWORD64 mask) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;
    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (!is_live(k) || (k & ~STATE_MASK) != target)
      return;
    slot->numa_interleave_mask = mask;
    slot->numa_node_count = static_cast<ULONG>(__builtin_popcountll(mask));
    slot->flags.fetch_or(VM_FLAG_NUMA_INTERLEAVE, cpp::MemoryOrder::RELEASE);
  }

  LIBC_INLINE DWORD64 get_numa_interleave_mask(void *view_base) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return 0;
    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if ((k & ~STATE_MASK) != target)
      return 0;
    if (!(slot->flags.load(cpp::MemoryOrder::RELAXED) & VM_FLAG_NUMA_INTERLEAVE))
      return 0;
    return slot->numa_interleave_mask;
  }

  // -----------------------------------------------------------------------
  // Snapshot — consistent point-in-time read with region resolution.
  // -----------------------------------------------------------------------
  //
  // Returns true iff the snapshot caught a well-defined state (LIVE,
  // PLACEHOLDER, or FOREIGN) at `view_base`. The seqlock protocol catches
  // any concurrent mutation; alloc_id validation catches region pool
  // reuse. On success, out->region points to a live RegionDesc (or null
  // for FOREIGN/placeholder-without-region slots).

  LIBC_INLINE bool snapshot(void *view_base, SlotSnapshot *out) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return false;
    return snapshot_at_slot(slot, view_base, out);
  }

  /// Park until the table's state at `view_base` undergoes any transition
  /// (publish, extract, foreign stamp, foreign clear, shape promotion,
  /// remap commit). Uses futex_addr's hardware-monitor Phase 1
  /// (UMWAIT/MWAITX) → kernel sleep on NtWaitForAlertByThreadId. Returns
  /// immediately when:
  ///   * the radix slot has not been demand-allocated yet — no signal to
  ///     wait on; the caller's outer retry will re-snapshot and converge,
  ///     or
  ///   * the slot's version already differs from the value captured at
  ///     entry — a concurrent transition happened in the gap between the
  ///     caller's last observation and this call.
  ///
  /// Convergence-wait companion to `snapshot()`. Use after an NT-level
  /// failure (e.g. STATUS_CONFLICTING_ADDRESSES on MAP_FIXED) where the
  /// caller knows another thread held the address: park on the precise
  /// signal — the slot's key changed by `publish_slot()` — instead of
  /// blind yields or pause hints. Any state transition unparks us.
  ///
  /// Parks on `&slot->key`, the same address `publish_slot()` wakes on,
  /// so this pairs cleanly with every state mutation in the table.
  LIBC_INLINE void wait_for_state_change(void *view_base) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (slot == nullptr)
      return; // No slot exists — nothing to park on.
    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    (void)futex_addr::wait(&slot->key, k, nullptr);
  }

  /// Seqlock body shared with `walk_range` and any other caller that has
  /// already located the Slot pointer (e.g., bitmap-driven iteration).
  /// `view_base` is the address the caller expects the slot to be bound
  /// to; if the slot is FREE, WRITING, or bound to a different VA, the
  /// call returns false. Single source of truth for state classification.
  LIBC_INLINE bool snapshot_at_slot(Slot *slot, void *view_base,
                                    SlotSnapshot *out) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);

    for (;;) {
      uint32_t v1 = slot->version.load(cpp::MemoryOrder::ACQUIRE);
      uintptr_t k1 = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (LIBC_UNLIKELY((k1 & ~STATE_MASK) != target))
        return false;
      if (LIBC_UNLIKELY((k1 & WRITING_BIT) != 0)) {
        // Mutation in progress — retry briefly via the slot's futex.
        wait_for_slot(slot, k1);
        continue;
      }

      out->view_base = view_base;
      out->view_size = __atomic_load_n(&slot->extent, __ATOMIC_RELAXED);
      out->region_id = __atomic_load_n(&slot->region_id, __ATOMIC_RELAXED);
      out->alloc_id = __atomic_load_n(&slot->alloc_id, __ATOMIC_RELAXED);
      out->view_prot = __atomic_load_n(&slot->view_prot, __ATOMIC_RELAXED);
      out->flags = slot->flags.load(cpp::MemoryOrder::RELAXED);

      cpp::atomic_thread_fence(cpp::MemoryOrder::ACQUIRE);
      uint32_t v2 = slot->version.load(cpp::MemoryOrder::RELAXED);
      uintptr_t k2 = slot->key.load(cpp::MemoryOrder::RELAXED);

      if (LIBC_UNLIKELY(v1 != v2 || k1 != k2)) {
        if ((k2 & ~STATE_MASK) != target)
          return false;
        continue;
      }

      // Resolve region. The contract this enforces:
      //
      //   * LIBC_INTERNAL / IMAGE_REGION / KERNEL_REGION / FOREIGN_
      //     SENTINEL slot — region_id is one of the four reserved
      //     sentinels (1..4). Sentinel descriptors live at fixed pool
      //     positions with pinned refcount and alloc_id written once
      //     at init / fork_reinit. Skip `resolve_unlocked`'s
      //     alloc_id+refcount ABA dance — the sentinel is definitionally
      //     alive. This is the hot path for `page_alloc`-registered
      //     slots, image-region checks at dlopen, TEB/PEB probes, and
      //     FOREIGN cordon revalidation.
      //
      //   * LIVE / PLACEHOLDER slot (dynamic region_id) — the slot
      //     itself holds a live reference, so `resolve_unlocked()` MUST
      //     succeed under a stable seqlock window. A null result here
      //     can only mean a teardown raced our snapshot (the slot is
      //     being extracted and its region released). Restart from the
      //     top; the next iteration will observe FREE / a different VA
      //     / a fresh region and converge.
      //
      //   * FOREIGN slot with region_id == NONE — legacy shape from
      //     pre-sentinel cordons; null `region` pointer is the documented
      //     value. New FOREIGN stamps go through `register_mapping_
      //     foreign` and carry the FOREIGN_SENTINEL_REGION_ID.
      //
      // `resolve_unlocked()` is intentional: `snapshot_at_slot` is
      // reachable from contexts that cannot acquire MmapLock (the VEH
      // demand-commit handler). The atomic-field reads on the resolved
      // descriptor are safe; the caller is responsible for holding
      // MmapLock if it intends to dereference plain handle fields. See
      // `RegionPool::resolve()` for the full lifetime contract.
      if (memory::RegionPool::is_sentinel_id(out->region_id)) {
        out->region = memory::g_region_pool.sentinel_desc(out->region_id);
      } else if (out->region_id != memory::RegionPool::NONE) {
        out->region = memory::g_region_pool.resolve_unlocked(out->region_id,
                                                             out->alloc_id);
        if (LIBC_UNLIKELY(out->region == nullptr && !is_foreign(k1))) {
          // Teardown race on a LIVE/PLACEHOLDER slot. Re-snapshot.
          continue;
        }
      } else {
        out->region = nullptr;
      }
      return true;
    }
  }

  // -----------------------------------------------------------------------
  // Extract — transfer the slot's region reference to the caller.
  // -----------------------------------------------------------------------
  //
  // On success, the slot is cleared to FREE and `out->region_id` is the
  // reference the caller now owns. Caller calls g_region_pool.release()
  // when done. This is the canonical "remove" operation: no silent handle
  // leaks, no duplicate-close risks.

  LIBC_INLINE bool extract(void *view_base, MappingEntry *out) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return false;

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (k == KEY_FREE)
        return false;

      // LIVE and PLACEHOLDER both transition to FREE via the same write
      // window — the caller takes ownership of the slot's region ref
      // regardless of which state it was in. Distinguishing the two is
      // the caller's job (e.g., snapshot before extract); the slot's
      // region_id+alloc_id remain in `out` for caller-side dispatch.
      const bool extractable =
          (is_live(k) || is_placeholder(k)) && (k & ~STATE_MASK) == target;
      if (extractable) {
        uintptr_t expected = k;
        if (!slot->key.compare_exchange_strong(expected, target | WRITING_BIT,
                                               cpp::MemoryOrder::ACQUIRE))
          continue;
        stamp_writing_owner(slot);
        if (out) {
          out->view_base = view_base;
          out->view_size = slot->extent;
          out->region_id = slot->region_id;
          out->alloc_id = slot->alloc_id;
          out->view_prot = slot->view_prot;
          out->flags = slot->flags.load(cpp::MemoryOrder::RELAXED);
        }
        clear_slot_fields(slot);
        publish_slot(slot, KEY_FREE);
        clear_occupancy_bit(target);
        return true;
      }

      if ((k & ~STATE_MASK) == target) {
        if (k & WRITING_BIT) {
          wait_for_writing(slot, k);
        } else {
          try_recover_slot(slot, k);
          wait_for_slot(slot, k);
        }
        continue;
      }
      return false;
    }
  }

  /// Convenience: extract and release the region in one call.
  LIBC_INLINE void remove(void *view_base) {
    MappingEntry entry{};
    if (extract(view_base, &entry) &&
        entry.region_id != memory::RegionPool::NONE)
      memory::g_region_pool.release(entry.region_id);
  }

  // -----------------------------------------------------------------------
  // FOREIGN slot revalidation.
  // -----------------------------------------------------------------------
  //
  // FOREIGN slots cordon VA owned by another subsystem (NT loader, CRT
  // heap, thread pool, user VirtualAlloc2). When the foreign owner
  // releases its placeholder, the cordon goes stale — `mark_foreign_stale`
  // is called to set VM_FLAG_FOREIGN_STALE, and the next code path that
  // touches the slot (mmap hint scan, MAP_FIXED probe, reconciliation)
  // calls `revalidate_foreign` to confirm the new state.
  //
  // Concurrency: many threads may hit the same stale slot simultaneously,
  // so we serialize the MRI_Ex syscall via a probe-lock bit in state_aux
  // and park lost-CAS waiters on &state_aux through futex_addr (lock-free
  // multi-waiter parking lot, hardware-monitor wake).

  /// Mark a FOREIGN slot stale. No-op when no FOREIGN slot exists at
  /// `view_base`. Idempotent — repeated calls coalesce into a single
  /// flag bit.
  LIBC_INLINE void mark_foreign_stale(void *view_base) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;
    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (!is_foreign(k) || (k & ~STATE_MASK) != target)
      return;
    slot->flags.fetch_or(VM_FLAG_FOREIGN_STALE, cpp::MemoryOrder::RELEASE);
  }

  /// Lazy revalidation of a FOREIGN slot. Issues at most one MRI_Ex
  /// syscall regardless of contention: the first arrival CAS-acquires
  /// the probe lock; concurrent callers park on &state_aux and wake when
  /// the prober clears the lock.
  ///
  /// Returns:
  ///   true  — slot was cleared (NT shows MEM_FREE; caller may now use
  ///           the VA, or the table no longer holds a FOREIGN entry at
  ///           this address for any other reason).
  ///   false — cordon still valid (caller must continue to skip).
  ///
  /// Calling on a non-FOREIGN slot is harmless and returns true (the
  /// caller's foreign-skip predicate is satisfied trivially).
  LIBC_INLINE bool revalidate_foreign(void *view_base) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (!slot)
      return true;

    // First, classify the slot. Non-FOREIGN states resolve immediately.
    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (!is_foreign(k) || (k & ~STATE_MASK) != target)
      return true;

    // Acquire the probe lock. Race losers park on state_aux until the
    // winning prober clears the lock and broadcasts.
    for (;;) {
      uint8_t cur = __atomic_load_n(&slot->state_aux, __ATOMIC_ACQUIRE);
      if ((cur & FOREIGN_PROBE_LOCK) == 0) {
        uint8_t desired = static_cast<uint8_t>(cur | FOREIGN_PROBE_LOCK);
        if (__atomic_compare_exchange_n(&slot->state_aux, &cur, desired,
                                        /*weak=*/false,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
          break; // We are the prober.
        continue; // CAS lost to a sibling lock acquire — retry.
      }
      // Another thread holds the lock — park until they clear it.
      // wait() returns immediately if the value already changed.
      futex_addr::wait<uint8_t>(&slot->state_aux, cur, nullptr);

      // After a wake, fall through to re-check. If the slot was cleared
      // (extracted to FREE) the next iteration of the outer key load
      // will detect it; we need to re-load the key.
      uintptr_t k_after = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (!is_foreign(k_after) || (k_after & ~STATE_MASK) != target)
        return true;
    }

    // We hold the probe lock. A sibling may have completed a probe
    // moments ago and already cleared FOREIGN_STALE; fall through to
    // release without re-issuing the syscall in that case.
    DWORD f_before = slot->flags.load(cpp::MemoryOrder::ACQUIRE);
    if ((f_before & VM_FLAG_FOREIGN_STALE) == 0) {
      // Slot is already fresh. Release the probe lock with broadcast.
      uint8_t age_only = static_cast<uint8_t>(
          __atomic_load_n(&slot->state_aux, __ATOMIC_RELAXED) &
          FOREIGN_AGE_MASK);
      __atomic_store_n(&slot->state_aux, age_only, __ATOMIC_RELEASE);
      futex_addr::wake(&slot->state_aux, UINT32_MAX);
      return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    SIZE_T ret = 0;
    NTSTATUS st = ::NtQueryVirtualMemory(NtCurrentProcess(),
                                         reinterpret_cast<void *>(target),
                                         MemoryBasicInformation, &mbi,
                                         sizeof(mbi), &ret);

    const bool nt_freed = !NT_SUCCESS(st) || mbi.State == MEM_FREE;

    if (nt_freed) {
      // Drop the cordon entirely. extract() handles the WRITING transition
      // and clears the slot; FOREIGN slots have no region ref to release.
      // We must release the probe lock first so that any thread parked on
      // state_aux wakes and observes the eventual KEY_FREE.
      __atomic_store_n(&slot->state_aux, 0, __ATOMIC_RELEASE);
      futex_addr::wake(&slot->state_aux, UINT32_MAX);

      // Take the slot out. extract() walks past PLACEHOLDER/FOREIGN by
      // returning false; we use a direct FOREIGN→FREE CAS instead.
      clear_foreign_slot(slot, target);
      return true;
    }

    // Cordon still valid. Bump the stamp generation, clear the
    // FOREIGN_STALE flag, then release the probe lock with broadcast.
    uint8_t old = __atomic_load_n(&slot->state_aux, __ATOMIC_RELAXED);
    uint8_t age = static_cast<uint8_t>((old & FOREIGN_AGE_MASK) +
                                       (1u << FOREIGN_AGE_SHIFT));
    slot->flags.fetch_and(~VM_FLAG_FOREIGN_STALE, cpp::MemoryOrder::RELEASE);
    __atomic_store_n(&slot->state_aux,
                     static_cast<uint8_t>(age & FOREIGN_AGE_MASK),
                     __ATOMIC_RELEASE);
    futex_addr::wake(&slot->state_aux, UINT32_MAX);
    return false;
  }

  // -----------------------------------------------------------------------
  // Neighbor and range queries.
  // -----------------------------------------------------------------------
  //
  // These answer "what lives adjacent to / inside this range" without any
  // NT syscall. Replaces the former MRI-probe-based coalesce logic.

  /// O(1) probe: is there any occupied slot at `view_base - 64KB`?
  /// When true and `out_region_id` is non-null, stores the neighbor's
  /// region id. When false, state is FREE / radix absent.
  [[nodiscard]] LIBC_INLINE bool has_neighbor_before(void *view_base,
                                         uint32_t *out_region_id) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    if (target < get_alloc_granularity())
      return false;
    uintptr_t prev = target - get_alloc_granularity();
    Slot *slot = find_slot(prev);
    if (!slot)
      return false;
    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (k == KEY_FREE || (k & ~STATE_MASK) != prev)
      return false;
    if (out_region_id)
      *out_region_id =
          __atomic_load_n(&slot->region_id, __ATOMIC_RELAXED);
    return true;
  }

  /// O(1) probe: is the slot at `view_end` occupied?
  [[nodiscard]] LIBC_INLINE bool has_neighbor_after(void *view_end,
                                        uint32_t *out_region_id) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_end);
    Slot *slot = find_slot(target);
    if (!slot)
      return false;
    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (k == KEY_FREE || (k & ~STATE_MASK) != target)
      return false;
    if (out_region_id)
      *out_region_id =
          __atomic_load_n(&slot->region_id, __ATOMIC_RELAXED);
    return true;
  }

  /// Walk every occupied slot in [start, end) and invoke `cb(&snap, ctx)`
  /// for each LIVE / PLACEHOLDER / FOREIGN slot. Iteration uses the L3
  /// occupancy bitmap (tzcnt per word) to skip empty regions cheaply.
  ///
  /// `cb` receives a fully resolved SlotSnapshot (region pointer included).
  /// Stopping early is the callback's responsibility via ctx.
  using MappingCallback = void (*)(const SlotSnapshot *snap, void *ctx);

  LIBC_INLINE void walk_range(void *start, void *end, MappingCallback cb,
                              void *ctx) {
    if (!ensure_init() || start >= end)
      return;

    const SIZE_T gran = get_alloc_granularity();
    uintptr_t lo = reinterpret_cast<uintptr_t>(start);
    uintptr_t hi = reinterpret_cast<uintptr_t>(end);

    // Snap down to a granularity boundary so the radix-key arithmetic below
    // matches what register_*/extract use. The final L3 page is iterated
    // up to (but not past) `hi`.
    if (lo % gran != 0)
      lo -= (lo % gran);
    if (hi > pcb_max_view_base() + gran)
      hi = pcb_max_view_base() + gran;
    if (lo >= hi)
      return;

    const uintptr_t lo_key = radix_key(lo);
    const uintptr_t hi_key_excl = radix_key(hi - 1) + 1;

    const size_t i1_lo = l1_index(lo_key);
    const size_t i1_hi = l1_index(hi_key_excl - 1);

    cpp::Atomic<L2Page *> *l1 = pcb_l1();
    for (size_t i1 = i1_lo; i1 <= i1_hi; ++i1) {
      L2Page *l2 = l1[i1].load(cpp::MemoryOrder::ACQUIRE);
      if (!l2)
        continue;

      const uintptr_t l1_first_key =
          static_cast<uintptr_t>(i1) << (L2_BITS + L3_BITS);
      const uintptr_t l1_last_key_excl =
          l1_first_key + (uintptr_t{1} << (L2_BITS + L3_BITS));
      const uintptr_t lo_in_l1 =
          (lo_key > l1_first_key) ? (lo_key - l1_first_key) : 0;
      const uintptr_t hi_in_l1 =
          (hi_key_excl < l1_last_key_excl)
              ? (hi_key_excl - l1_first_key)
              : (uintptr_t{1} << (L2_BITS + L3_BITS));

      const unsigned i2_first = static_cast<unsigned>(lo_in_l1 >> L3_BITS);
      const unsigned i2_last_excl =
          static_cast<unsigned>((hi_in_l1 + (uintptr_t{1} << L3_BITS) - 1) >>
                                L3_BITS);

      for (unsigned i2 = i2_first; i2 < i2_last_excl; ++i2) {
        L3Page *l3 = l2->children[i2].load(cpp::MemoryOrder::ACQUIRE);
        if (!l3)
          continue;

        // Per-L3-page key window clipped to [lo_key, hi_key_excl).
        const uintptr_t l3_first_key = l1_first_key +
                                       (static_cast<uintptr_t>(i2) << L3_BITS);
        const uintptr_t l3_last_key_excl =
            l3_first_key + (uintptr_t{1} << L3_BITS);

        const unsigned i3_first =
            (lo_key > l3_first_key) ? static_cast<unsigned>(lo_key - l3_first_key)
                                    : 0u;
        const unsigned i3_last_excl =
            (hi_key_excl < l3_last_key_excl)
                ? static_cast<unsigned>(hi_key_excl - l3_first_key)
                : static_cast<unsigned>(L3_SIZE);
        if (i3_first >= i3_last_excl)
          continue;

        // Walk only occupancy bits inside [i3_first, i3_last_excl) using
        // tzcnt over the range-clipped bitmap words.
        const unsigned w_first = i3_first >> 6;        // / 64
        const unsigned w_last = (i3_last_excl - 1) >> 6;
        for (unsigned w = w_first; w <= w_last; ++w) {
          uint64_t bits = l3->occupancy[w].load(cpp::MemoryOrder::ACQUIRE);
          if (bits == 0)
            continue;

          // Clip to [i3_first, i3_last_excl) inside this 64-slot word.
          const unsigned word_lo = w * 64u;
          if (i3_first > word_lo) {
            const unsigned shift = i3_first - word_lo;
            bits &= ~uint64_t{0} << shift;
          }
          if (i3_last_excl < word_lo + 64u) {
            const unsigned keep = i3_last_excl - word_lo;
            bits &= (uint64_t{1} << keep) - 1u;
          }

          while (bits) {
            const unsigned b = static_cast<unsigned>(__builtin_ctzll(bits));
            const unsigned i3 = word_lo + b;
            const uint64_t next_bits = bits & (bits - 1);
            if (LIBC_LIKELY(next_bits != 0)) {
              const unsigned nb =
                  static_cast<unsigned>(__builtin_ctzll(next_bits));
              // L1 temporal: snapshot_at_slot dereferences immediately.
              __builtin_prefetch(&l3->slots[word_lo + nb], 0, 3);
            }
            // Compute the slot's expected base directly from its radix
            // position — no extra key load. snapshot_at_slot validates
            // the slot's actual key matches and applies the canonical
            // state classification (single source of truth).
            Slot *slot = &l3->slots[i3];
            const uintptr_t base_key = l3_first_key + i3;
            void *base = reinterpret_cast<void *>(base_key << 16);
            SlotSnapshot snap;
            if (snapshot_at_slot(slot, base, &snap))
              cb(&snap, ctx);
            bits = next_bits;
          }
        }
      }
    }
  }

  // -----------------------------------------------------------------------
  // Transactional remap API.
  // -----------------------------------------------------------------------

  /// LIVE -> WRITING -> REMAPPING. Snapshots the captured entry into *out
  /// (caller owns that region ref until commit/abort). O(1).
  ///
  /// `guarded_size` is both the new VEH guard window size and the caller's
  /// expected current extent of the slot — they always coincide because
  /// callers compute guarded_size from a snapshot of `view_base`/`view_end`.
  /// If a concurrent partial-unmap shrunk the slot between the caller's
  /// snapshot and our CAS, the slot's stored `extent` will not match and
  /// the caller's view bounds are stale; we undo the CAS and set
  /// `*out_stale = true` (when provided) so the caller can re-snapshot.
  [[nodiscard]] LIBC_INLINE bool begin_remap(void *view_base, SIZE_T guarded_size,
                                 MappingEntry *out,
                                 bool *out_stale = nullptr) {
    if (out_stale)
      *out_stale = false;
    if (!ensure_init())
      return false;
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return false;

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (k == KEY_FREE)
        return false;

      if (is_live(k) && (k & ~STATE_MASK) == target) {
        uintptr_t expected = k;
        if (!slot->key.compare_exchange_strong(expected, target | WRITING_BIT,
                                               cpp::MemoryOrder::ACQUIRE))
          continue;
        // Stale-snapshot defence under WRITING exclusivity. A concurrent
        // partial_unmap_view that promoted MONO->CHUNKED and rebound this
        // slot to a smaller extent would slip past the LIVE/key check
        // because the slot is still LIVE at the same key — only its
        // extent shrunk. Compare to the caller's expected size and undo
        // the CAS on mismatch so the outer dispatch can re-snapshot.
        if (LIBC_UNLIKELY(slot->extent != guarded_size)) {
          publish_slot(slot, live_key(target));
          if (out_stale)
            *out_stale = true;
          return false;
        }
        out->view_base = view_base;
        out->view_size = slot->extent;
        out->region_id = slot->region_id;
        out->alloc_id = slot->alloc_id;
        out->view_prot = slot->view_prot;
        out->flags = slot->flags.load(cpp::MemoryOrder::RELAXED);
        slot->extent = guarded_size;
        slot->remap_owner_thread = open_current_thread_handle();
        slot->remap_guard_index = claim_guard(slot);
        state().active_remap_count_.fetch_add(1, cpp::MemoryOrder::RELEASE);
        slot->key.store(target | REMAPPING_BIT, cpp::MemoryOrder::RELEASE);
        return true;
      }

      if ((k & ~STATE_MASK) == target) {
        if (k & WRITING_BIT) {
          wait_for_writing(slot, k);
        } else {
          try_recover_slot(slot, k);
          wait_for_slot(slot, k);
        }
        continue;
      }
      return false;
    }
  }

  /// Create a REMAPPING sentinel for MAP_FIXED: either installs at a FREE
  /// slot or takes over an existing LIVE/PLACEHOLDER/FOREIGN one.
  [[nodiscard]] LIBC_INLINE bool begin_remap_guard(void *view_base, SIZE_T guarded_size) {
    if (!ensure_init())
      return false;
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = ensure_slot(target);

    for (;;) {
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);

      if (k == KEY_FREE) {
        uintptr_t expected = KEY_FREE;
        if (!slot->key.compare_exchange_strong(expected, target | WRITING_BIT,
                                               cpp::MemoryOrder::ACQUIRE))
          continue;
        clear_slot_fields(slot);
        slot->extent = guarded_size;
        slot->remap_owner_thread = open_current_thread_handle();
        slot->remap_guard_index = claim_guard(slot);
        state().active_remap_count_.fetch_add(1, cpp::MemoryOrder::RELEASE);
        slot->key.store(target | REMAPPING_BIT, cpp::MemoryOrder::RELEASE);
        return true;
      }

      if ((k & ~STATE_MASK) == target &&
          (is_live(k) || is_placeholder(k) || is_foreign(k))) {
        // Reject MAP_FIXED takeover of libc-internal VA. Without
        // WRITING_BIT set on `k`, `region_id` is stable — a concurrent
        // writer would have to CAS the key first, and our own CAS below
        // would then lose and reloop. Substrate arenas (and every other
        // LIBC_INTERNAL stamp) must not be silently overwritten; user
        // MAP_FIXED failing here bubbles up to mmap as EPERM.
        if (slot->region_id == memory::RegionPool::INTERNAL_REGION_ID)
          return false;
        uintptr_t expected = k;
        if (!slot->key.compare_exchange_strong(expected, target | WRITING_BIT,
                                               cpp::MemoryOrder::ACQUIRE))
          continue;
        slot->extent = guarded_size;
        slot->remap_owner_thread = open_current_thread_handle();
        slot->remap_guard_index = claim_guard(slot);
        state().active_remap_count_.fetch_add(1, cpp::MemoryOrder::RELEASE);
        slot->key.store(target | REMAPPING_BIT, cpp::MemoryOrder::RELEASE);
        return true;
      }

      if ((k & ~STATE_MASK) == target) {
        if (k & WRITING_BIT) {
          wait_for_writing(slot, k);
        } else {
          try_recover_slot(slot, k);
          wait_for_slot(slot, k);
        }
        continue;
      }
      return false;
    }
  }

  /// REMAPPING -> LIVE with new region binding. If new_region_id differs
  /// from the slot's current region_id, the previous region's reference
  /// is released here (consolidating ownership through the table). Pass
  /// the same region_id + alloc_id to preserve the existing reference.
  LIBC_INLINE bool commit_remap(void *old_view_base, void *new_view_base, SIZE_T view_size,
                    uint32_t region_id, uint8_t alloc_id, DWORD prot,
                    DWORD flags) {
    uintptr_t old_target = reinterpret_cast<uintptr_t>(old_view_base);
    uintptr_t new_target = reinterpret_cast<uintptr_t>(new_view_base);

    Slot *old_slot = find_slot(old_target);
    if (LIBC_UNLIKELY(!old_slot))
      return false;

    uintptr_t k = old_slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (k != (old_target | REMAPPING_BIT))
      return false;

    if (old_target == new_target) {
      // Same-base commit: swap region binding in place.
      old_slot->key.store(new_target | WRITING_BIT | REMAPPING_BIT,
                          cpp::MemoryOrder::RELEASE);
      uint32_t prev_rid = old_slot->region_id;
      old_slot->region_id = region_id;
      old_slot->alloc_id = alloc_id;
      old_slot->extent = view_size;
      old_slot->view_prot = prot;
      old_slot->flags.fetch_or(flags, cpp::MemoryOrder::RELAXED);
      int gi = old_slot->remap_guard_index;
      HANDLE owner = old_slot->remap_owner_thread;
      clear_remap_fields(old_slot);
      set_occupancy_bit(new_target);
      publish_slot(old_slot, live_key(new_target));
      if (owner)
        ::NtClose(owner);
      release_guard(gi);
      release_remap_count();
      if (prev_rid != memory::RegionPool::NONE && prev_rid != region_id)
        memory::g_region_pool.release(prev_rid);
      return true;
    }

    // Different-base commit: install new slot + free old.
    Slot *new_slot = ensure_slot(new_target);
    bool inserted = false;

    uintptr_t expected = KEY_FREE;
    if (new_slot->key.compare_exchange_strong(expected, new_target | WRITING_BIT,
                                              cpp::MemoryOrder::ACQUIRE)) {
      stamp_writing_owner(new_slot);
      new_slot->region_id = region_id;
      new_slot->alloc_id = alloc_id;
      new_slot->state_aux = 0;
      new_slot->extent = view_size;
      new_slot->view_prot = prot;
      new_slot->flags.store(flags, cpp::MemoryOrder::RELAXED);
      set_occupancy_bit(new_target);
      publish_slot(new_slot, live_key(new_target));
      inserted = true;
    }

    uint32_t prev_rid = old_slot->region_id;
    int gi = old_slot->remap_guard_index;
    HANDLE owner = old_slot->remap_owner_thread;
    clear_slot_fields(old_slot);
    clear_remap_fields(old_slot);
    publish_slot(old_slot, KEY_FREE);
    clear_occupancy_bit(old_target);
    if (owner)
      ::NtClose(owner);
    release_guard(gi);
    release_remap_count();

    if (!inserted && prev_rid != memory::RegionPool::NONE)
      memory::g_region_pool.release(prev_rid);
    else if (inserted && prev_rid != memory::RegionPool::NONE &&
             prev_rid != region_id)
      memory::g_region_pool.release(prev_rid);

    return inserted;
  }

  /// REMAPPING -> LIVE (restore original). The previously captured
  /// MappingEntry's region reference is put back into the slot; caller
  /// relinquishes their temporary ownership of it.
  LIBC_INLINE void abort_remap(void *view_base, const MappingEntry &original) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;

    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (k != (target | REMAPPING_BIT))
      return;

    slot->region_id = original.region_id;
    slot->alloc_id = original.alloc_id;
    slot->extent = original.view_size;
    slot->view_prot = original.view_prot;
    slot->flags.store(original.flags, cpp::MemoryOrder::RELAXED);
    HANDLE owner = slot->remap_owner_thread;
    int gi = slot->remap_guard_index;
    clear_remap_fields(slot);
    publish_slot(slot, live_key(target));
    if (owner)
      ::NtClose(owner);
    release_guard(gi);
    release_remap_count();
  }

  /// REMAPPING -> FREE. Releases the region reference the slot held. O(1).
  LIBC_INLINE void discard_remap(void *view_base) {
    uintptr_t target = reinterpret_cast<uintptr_t>(view_base);
    Slot *slot = find_slot(target);
    if (LIBC_UNLIKELY(!slot))
      return;

    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (k != (target | REMAPPING_BIT))
      return;

    uint32_t rid = slot->region_id;
    int gi = slot->remap_guard_index;
    HANDLE owner = slot->remap_owner_thread;
    clear_slot_fields(slot);
    clear_remap_fields(slot);
    publish_slot(slot, KEY_FREE);
    clear_occupancy_bit(target);
    if (owner)
      ::NtClose(owner);
    release_guard(gi);
    release_remap_count();
    if (rid != memory::RegionPool::NONE)
      memory::g_region_pool.release(rid);
  }

  /// REMAPPING sentinel (no region ref) -> FREE. Used by MAP_FIXED paths
  /// that reserve a guard without yet binding a region.
  LIBC_INLINE void abort_remap_guard(void *view_base) { discard_remap(view_base); }

  // -----------------------------------------------------------------------
  // VEH remap-guard interface.
  // -----------------------------------------------------------------------

  LIBC_INLINE int check_remap_guard(uintptr_t fault_addr) {
    cpp::Atomic<uintptr_t> *guards = pcb_remap_guards();
    if (LIBC_UNLIKELY(!guards))
      return -1;
    if (LIBC_LIKELY(
            state().active_remap_count_.get_value(cpp::MemoryOrder::ACQUIRE) == 0))
      return -1;

    uint32_t hwm = state().guard_high_water_.load(cpp::MemoryOrder::ACQUIRE);
    for (uint32_t i = 0; i < hwm; ++i) {
      uintptr_t ptr = guards[i].load(cpp::MemoryOrder::ACQUIRE);
      if (!ptr)
        continue;

      Slot *slot = reinterpret_cast<Slot *>(ptr);
      uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);

      if (!(k & REMAPPING_BIT))
        continue;

      uintptr_t base = k & ~STATE_MASK;
      SIZE_T size = slot->extent;

      if (fault_addr >= base && fault_addr < base + size) {
        uintptr_t k_recheck = slot->key.load(cpp::MemoryOrder::ACQUIRE);
        if (!(k_recheck & REMAPPING_BIT))
          return -1;
        if (is_thread_dead(slot->remap_owner_thread)) {
          recover_dead_remap(slot, k_recheck);
          return -1;
        }
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  LIBC_INLINE void wait_for_remap(int guard_index) {
    uintptr_t ptr =
        pcb_remap_guards()[guard_index].load(cpp::MemoryOrder::ACQUIRE);
    if (!ptr)
      return;

    Slot *slot = reinterpret_cast<Slot *>(ptr);
    uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
    if (!(k & REMAPPING_BIT))
      return;

    LARGE_INTEGER timeout;
    timeout.QuadPart = REMAP_WAIT_TIMEOUT_100NS;

    while ((k = slot->key.load(cpp::MemoryOrder::ACQUIRE)) & REMAPPING_BIT) {
      futex_addr::wait_nt(
          reinterpret_cast<const volatile uintptr_t *>(&slot->key), k,
          &timeout);

      k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
      if (!(k & REMAPPING_BIT))
        return;

      if (is_thread_dead(slot->remap_owner_thread)) {
        recover_dead_remap(slot, k);
        return;
      }
    }
  }

  // -----------------------------------------------------------------------
  // Fork reinit.
  // -----------------------------------------------------------------------
  //
  // Parent's region descriptors are inherited by the child unchanged
  // (RegionPool::fork_reinit handles pool internal state). Slot entries
  // survive verbatim — the child's view of its inherited mappings is the
  // parent's last-committed state. In-flight REMAPPING slots from parent
  // threads that did not survive the fork are scrubbed.

  LIBC_INLINE void fork_reinit() {
    FutexValueType init_state = state().init_state_.load(cpp::MemoryOrder::ACQUIRE);
    if (init_state == INIT_UNINITIALIZED || init_state == INIT_DESTROYED)
      return;

    if (init_state == INIT_IN_PROGRESS) {
      reset_root_state();
      state().init_state_.store(INIT_UNINITIALIZED, cpp::MemoryOrder::RELAXED);
      return;
    }

    // Scrub stale REMAPPING entries from the guard array.
    cpp::Atomic<uintptr_t> *guards = pcb_remap_guards();
    uint32_t hwm = state().guard_high_water_.load(cpp::MemoryOrder::RELAXED);
    for (uint32_t i = 0; i < hwm; ++i) {
      uintptr_t ptr = guards[i].load(cpp::MemoryOrder::RELAXED);
      if (!ptr)
        continue;

      Slot *slot = reinterpret_cast<Slot *>(ptr);
      uintptr_t k = slot->key.load(cpp::MemoryOrder::RELAXED);
      if (k & REMAPPING_BIT) {
        HANDLE owner = slot->remap_owner_thread;
        clear_slot_fields(slot);
        clear_remap_fields(slot);
        if (owner)
          ::NtClose(owner);
        slot->version.fetch_add(1, cpp::MemoryOrder::RELAXED);
        slot->key.store(KEY_FREE, cpp::MemoryOrder::RELAXED);
        clear_occupancy_bit(k & ~STATE_MASK);
        // Region reference is owned by the parent's perspective and has
        // already propagated to the child via RegionPool; we leave the
        // region alone. Any accounting drift is reconciled by
        // region_reconcile::post_fork_scan.
      }
      guards[i].store(0, cpp::MemoryOrder::RELAXED);
    }
    state().active_remap_count_.store(0, cpp::MemoryOrder::RELAXED);
    state().guard_high_water_.store(0, cpp::MemoryOrder::RELAXED);
    state().alloc_cursor_.store(0, cpp::MemoryOrder::RELAXED);

    // LIVE slots' region_id/alloc_id stay valid — the RegionPool carries
    // descriptors across fork as-is. No per-slot handle duping is needed.
  }

  // -----------------------------------------------------------------------
  // Bitmap-accelerated iteration.
  // -----------------------------------------------------------------------

  LIBC_INLINE unsigned count_live() {
    if (!ensure_init())
      return 0;
    cpp::Atomic<L2Page *> *l1 = pcb_l1();
    size_t l1_size = pcb_l1_size();
    unsigned count = 0;
    for (size_t i1 = 0; i1 < l1_size; ++i1) {
      L2Page *l2 = l1[i1].load(cpp::MemoryOrder::ACQUIRE);
      if (!l2)
        continue;
      for (unsigned i2 = 0; i2 < static_cast<unsigned>(L2_SIZE); ++i2) {
        L3Page *l3 = l2->children[i2].load(cpp::MemoryOrder::ACQUIRE);
        if (!l3)
          continue;
        for (unsigned w = 0; w < 16; ++w)
          count += static_cast<unsigned>(__builtin_popcountll(
              l3->occupancy[w].load(cpp::MemoryOrder::RELAXED)));
      }
    }
    return count;
  }

  LIBC_INLINE void for_each_live(MappingCallback cb, void *ctx) {
    if (!ensure_init())
      return;
    cpp::Atomic<L2Page *> *l1 = pcb_l1();
    size_t l1_size = pcb_l1_size();
    for (size_t i1 = 0; i1 < l1_size; ++i1) {
      L2Page *l2 = l1[i1].load(cpp::MemoryOrder::ACQUIRE);
      if (!l2)
        continue;
      for (unsigned i2 = 0; i2 < static_cast<unsigned>(L2_SIZE); ++i2) {
        L3Page *l3 = l2->children[i2].load(cpp::MemoryOrder::ACQUIRE);
        if (!l3)
          continue;
        for (unsigned w = 0; w < 16; ++w) {
          uint64_t bits = l3->occupancy[w].load(cpp::MemoryOrder::ACQUIRE);
          while (bits) {
            unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
            unsigned i3 = w * 64 + bit;
            uint64_t next_bits = bits & (bits - 1);
            if (LIBC_LIKELY(next_bits != 0)) {
              unsigned nb =
                  static_cast<unsigned>(__builtin_ctzll(next_bits));
              // Write hint (rw=1 → PREFETCHW with +prfchw): for_each_live's
              // sole caller is region_reconcile::clear_stale_cb, whose
              // callback re-enters the table via revalidate_foreign() and
              // CASes the same slot. Prefetching in M-state avoids the
              // S→M upgrade on the subsequent CAS.
              __builtin_prefetch(&l3->slots[w * 64 + nb], 1, 1);
            }
            Slot *slot = &l3->slots[i3];
            uintptr_t k = slot->key.load(cpp::MemoryOrder::ACQUIRE);
            if (k != KEY_FREE && !(k & WRITING_BIT)) {
              SlotSnapshot snap;
              if (snapshot(reinterpret_cast<void *>(k & ~STATE_MASK), &snap))
                cb(&snap, ctx);
            }
            bits = next_bits;
          }
        }
      }
    }
  }

  // -----------------------------------------------------------------------
  // Destroy (process shutdown).
  // -----------------------------------------------------------------------

  // destroy() runs at process fini via the .libcfin sweep. The L1 / L2 /
  // L3 pages and the guard region are all in heap-style storage (page_alloc
  // VA), which we explicitly release. We CANNOT zero the PCB-resident
  // pointers (mapping_table_l1_ / remap_guards_) — Zone 0 is sealed
  // PAGE_READONLY for the process lifetime — but the seal is moot here:
  // the process is exiting and the kernel will tear down the section
  // anyway. The stale pointer is unobservable.
  LIBC_INLINE void destroy() {
    if (state().init_state_.load(cpp::MemoryOrder::ACQUIRE) != INIT_READY)
      return;

    // Phase-4 teardown. Walks every live slot to release its region
    // reference + close the remap owner handle; does NOT free the radix
    // page backing itself. L1 / L2 / L3 are substrate sub-slots; their
    // memory belongs to the substrate's Small / Large arenas. Freeing a
    // sub-slot via raw page_free would either fail at NtFreeVirtualMemory
    // (sub-slot VA is not the base of an NT reservation) or, worse,
    // desynchronise substrate bookkeeping. Phase-1 (va_substrate) runs
    // after every user of mapping_table has torn down; its destroy()
    // MEM_RELEASEs each whole arena, reclaiming every radix page in one
    // shot.
    cpp::Atomic<L2Page *> *l1 = pcb_l1();
    size_t l1_size = pcb_l1_size();
    for (size_t i1 = 0; i1 < l1_size; ++i1) {
      L2Page *l2 = l1[i1].load(cpp::MemoryOrder::RELAXED);
      if (!l2)
        continue;

      for (unsigned i2 = 0; i2 < static_cast<unsigned>(L2_SIZE); ++i2) {
        L3Page *l3 = l2->children[i2].load(cpp::MemoryOrder::RELAXED);
        if (!l3)
          continue;

        for (unsigned i3 = 0; i3 < static_cast<unsigned>(L3_SIZE); ++i3) {
          Slot *slot = &l3->slots[i3];
          uintptr_t k = slot->key.load(cpp::MemoryOrder::RELAXED);
          if (k == KEY_FREE)
            continue;
          uint32_t rid = slot->region_id;
          HANDLE owner =
              (k & REMAPPING_BIT) ? slot->remap_owner_thread : nullptr;
          clear_slot_fields(slot);
          if (owner)
            ::NtClose(owner);
          slot->flags.store(0, cpp::MemoryOrder::RELAXED);
          slot->version.fetch_add(1, cpp::MemoryOrder::RELAXED);
          slot->key.store(KEY_FREE, cpp::MemoryOrder::RELAXED);
          if (rid != memory::RegionPool::NONE)
            memory::g_region_pool.release(rid);
        }

        // L3 sub-slot stays backed by substrate memory. The slot entries
        // above are logically empty; the radix page itself is released
        // by va_substrate.destroy() when its containing Large arena is
        // MEM_RELEASE'd at fini $P1.
        l2->children[i2].store(nullptr, cpp::MemoryOrder::RELAXED);
      }

      // L2 sub-slot — same deal, released as part of the Small arena
      // MEM_RELEASE at fini $P1.
      l1[i1].store(nullptr, cpp::MemoryOrder::RELAXED);
    }

    // L1 directory: also a substrate Small sub-slot. The PCB pointer to
    // it lives in sealed Zone 0 and is never rewritten; callers gated on
    // INIT_READY can no longer reach it because the store below flips
    // state to INIT_DESTROYED. The backing memory is reclaimed by
    // va_substrate.destroy() at fini $P1.
    reset_root_state();
    state().init_state_.store_and_notify_all(INIT_DESTROYED);
  }
};

/// Global mapping table controller. Geometry initialized lazily from the
/// runtime user-VA ceiling.
// MappingTable carries no state — all runtime bookkeeping lives in
// PCB Zone 1 (g_pcb.mapping_table) and all sealed handles in Zone 0.
// The instance exists solely as a callsite-stable dispatch handle so
// legacy `g_mapping_table.method()` call syntax keeps working.
//
// Defined exactly once in mapping_table.cpp. Declaring it extern (not
// inline) is load-bearing for static-archive consumers: it forces the
// linker to pull mapping_table.cpp.obj to resolve the symbol, which in
// turn pulls its `.libcmem$P2` registry entry into the final image.
// With an inline definition every TU emitted its own COMDAT copy and
// mapping_table.cpp.obj was silently discarded from hermetic test
// archives, dropping the P2 record and crashing Tier A at the walker.
extern MappingTable g_mapping_table;

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MAPPING_TABLE_H
