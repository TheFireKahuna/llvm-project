//===-- Software memory protection keys for Windows x86_64 ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Software implementation of POSIX memory protection keys on Windows.
//
// x86_64 CPUs with PKU support have a PKRU register (32-bit, 2 bits per key,
// 16 keys) that controls per-thread access rights. RDPKRU/WRPKRU are
// unprivileged instructions and work on any OS. Windows saves/restores PKRU
// via XSAVE on context switches.
//
// However, Windows does not expose an API to set protection key bits in page
// table entries. Without PTE PK bits, PKRU has no hardware effect -- all pages
// use key 0 by default.
//
// This module provides software enforcement:
//   pkey_alloc/free: userspace bitmap of keys 1-15 (key 0 is default)
//   pkey_get/set: RDPKRU/WRPKRU (real hardware register) + software tracking
//   pkey_mprotect: mprotect + record key association
//
// Enforcement: when pkey_set changes rights for a key, page protections are
// updated via mprotect() for all pages associated with that key:
//   PKEY_DISABLE_ACCESS -> PROT_NONE
//   PKEY_DISABLE_WRITE  -> remove PROT_WRITE from original prot
//   rights == 0         -> restore original prot
//
// Range tracking is lock-free using atomic keys with seqlock-style reads,
// the same pattern as MappingTable. No SRWLOCK, no kernel sync objects.
//
// Process-wide allocation bitmap and per-key rights live in the PCB
// (spec-bounded by hardware: 16 keys). The range table is workload-bounded
// and stays external behind the PCB's g_pcb.pkey.range_table pointer.
//
// aarch64: Returns ENOSYS (no equivalent register; matches Linux generic stub).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PKEY_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PKEY_STATE_H

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/new.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/legacy/section_region.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/futex_utils.h"

#ifdef LIBC_TARGET_ARCH_IS_X86_64
#include <immintrin.h>
#endif

namespace LIBC_NAMESPACE_DECL {

// Forward declaration -- enforcement calls mprotect() through the POSIX layer.
int mprotect(void *addr, size_t len, int prot);

namespace windows {

inline constexpr int PKEY_BITS_PER_KEY = 2;
inline constexpr uint32_t PKEY_MASK = 0x3;

/// Lock-free range table for pkey-associated memory regions.
///
/// Same seqlock pattern as MappingTable: each slot's key encodes state:
///   0               = free
///   addr            = live (fields stable, readers safe)
///   addr | 1        = writing (fields being modified, readers retry)
///
/// Page-aligned addresses always have bit 0 clear, so the low bit is free.
/// Section-backed demand-committed storage -- only touched pages cost memory.
struct PkeyRangeTable {
  static constexpr uintptr_t KEY_FREE = 0;
  static constexpr uintptr_t WRITING_BIT = 1;
  static constexpr int MAX_RANGES = 4096;
  static constexpr FutexValueType INIT_UNINITIALIZED = 0;
  static constexpr FutexValueType INIT_IN_PROGRESS = 1;
  static constexpr FutexValueType INIT_READY = 2;

  struct Slot {
    cpp::Atomic<uintptr_t> key{0};
    SIZE_T size;
    int pkey;
    int original_prot; // POSIX prot at pkey_mprotect time
  };

  Slot *slots_ = nullptr;
  SectionRegion storage_{};
  cpp::Atomic<int> high_water_{0};
  Futex init_state_{INIT_UNINITIALIZED};

  bool ensure_init() {
    FutexValueType state = init_state_.load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_LIKELY(state == INIT_READY))
      return true;
    if (state == INIT_UNINITIALIZED &&
        init_state_.compare_exchange_strong(state, INIT_IN_PROGRESS,
                                            cpp::MemoryOrder::ACQUIRE)) {
      bool ok = do_init();
      init_state_.store_and_notify_all(ok ? INIT_READY : INIT_UNINITIALIZED);
      return ok;
    }
    while ((state = init_state_.load(cpp::MemoryOrder::ACQUIRE)) ==
           INIT_IN_PROGRESS) {
      // Yield on transient wait failure (-ENOMEM on pool exhaustion) so
      // we don't spin-fail hot waiting for the concurrent initializer.
      long ret = init_state_.wait(INIT_IN_PROGRESS);
      if (ret < 0 && ret != -EINTR)
        ::NtYieldExecution();
    }
    return state == INIT_READY;
  }

  bool do_init() {
    storage_ = SectionRegion::create_anon(
        static_cast<size_t>(MAX_RANGES) * sizeof(Slot));
    if (!storage_)
      return false;
    slots_ = storage_.as<Slot>();
    return true;
  }

  void fork_reinit() {
    if (init_state_.load(cpp::MemoryOrder::ACQUIRE) != INIT_IN_PROGRESS)
      return;
    storage_.destroy();
    slots_ = nullptr;
    high_water_.store(0, cpp::MemoryOrder::RELAXED);
    init_state_.store(INIT_UNINITIALIZED, cpp::MemoryOrder::RELAXED);
  }

  /// Register a pkey range. Lock-free: CAS a free slot to claim it.
  bool register_range(void *addr, SIZE_T size, int pkey, int posix_prot) {
    if (!ensure_init())
      return false;

    uintptr_t target = reinterpret_cast<uintptr_t>(addr);

    for (;;) {
      int hw = high_water_.load(cpp::MemoryOrder::ACQUIRE);
      int free_idx = -1;

      // Scan for existing entry (update) or first free slot.
      for (int i = 0; i < hw; ++i) {
        uintptr_t k = slots_[i].key.load(cpp::MemoryOrder::ACQUIRE);

        // Existing entry for this address -- claim for update.
        if (k == target) {
          uintptr_t expected = target;
          if (slots_[i].key.compare_exchange_strong(
                  expected, target | WRITING_BIT,
                  cpp::MemoryOrder::ACQUIRE)) {
            slots_[i].size = size;
            slots_[i].pkey = pkey;
            slots_[i].original_prot = posix_prot;
            slots_[i].key.store(target, cpp::MemoryOrder::RELEASE);
            return true;
          }
          break; // CAS failed -- retry from top.
        }

        if (k == KEY_FREE && free_idx < 0)
          free_idx = i;
      }

      // Insert at free slot below high_water.
      if (free_idx >= 0) {
        uintptr_t expected = KEY_FREE;
        if (slots_[free_idx].key.compare_exchange_strong(
                expected, target | WRITING_BIT,
                cpp::MemoryOrder::ACQUIRE)) {
          slots_[free_idx].size = size;
          slots_[free_idx].pkey = pkey;
          slots_[free_idx].original_prot = posix_prot;
          slots_[free_idx].key.store(target, cpp::MemoryOrder::RELEASE);
          return true;
        }
        continue; // Slot claimed by another thread -- retry.
      }

      // Extend: claim next slot at high_water.
      int idx = high_water_.fetch_add(1, cpp::MemoryOrder::RELAXED);
      if (idx >= MAX_RANGES) {
        high_water_.store(MAX_RANGES, cpp::MemoryOrder::RELAXED);
        return false;
      }
      slots_[idx].size = size;
      slots_[idx].pkey = pkey;
      slots_[idx].original_prot = posix_prot;
      slots_[idx].key.store(target, cpp::MemoryOrder::RELEASE);
      return true;
    }
  }

  /// Remove all ranges for a key. Lock-free: CAS each matching slot to free.
  void remove_key(int pkey) {
    if (!slots_)
      return;

    int hw = high_water_.load(cpp::MemoryOrder::ACQUIRE);
    for (int i = 0; i < hw; ++i) {
      uintptr_t k = slots_[i].key.load(cpp::MemoryOrder::ACQUIRE);
      if (k == KEY_FREE || (k & WRITING_BIT))
        continue;

      // Seqlock read: check pkey field.
      int slot_pkey = slots_[i].pkey;
      cpp::atomic_thread_fence(cpp::MemoryOrder::ACQUIRE);
      uintptr_t k2 = slots_[i].key.load(cpp::MemoryOrder::RELAXED);
      if (k2 != k)
        continue; // Writer intervened -- skip (will catch on next pass).

      if (slot_pkey != pkey)
        continue;

      // Claim and free.
      uintptr_t expected = k;
      slots_[i].key.compare_exchange_strong(expected, KEY_FREE,
                                            cpp::MemoryOrder::RELEASE,
                                            cpp::MemoryOrder::RELAXED);
      // CAS failure is fine -- another thread freed or updated it.
    }
  }

  /// Lock-free convergent enforcement. Scans all slots matching the key
  /// and calls mprotect() to apply the effective protection.
  /// If rights change mid-scan (concurrent set_rights), bails early --
  /// the newer caller will converge to the final value.
  void enforce_key(int pkey, uint32_t target_rights, int base_prot_override,
                   cpp::Atomic<uint32_t> &authoritative_rights) {
    if (!slots_)
      return;

    int hw = high_water_.load(cpp::MemoryOrder::ACQUIRE);
    for (int i = 0; i < hw; ++i) {
      uintptr_t k1 = slots_[i].key.load(cpp::MemoryOrder::ACQUIRE);
      if (k1 == KEY_FREE || (k1 & WRITING_BIT))
        continue;

      // Seqlock read: snapshot fields.
      int slot_pkey = slots_[i].pkey;
      SIZE_T slot_size = slots_[i].size;
      int slot_prot = slots_[i].original_prot;

      cpp::atomic_thread_fence(cpp::MemoryOrder::ACQUIRE);
      uintptr_t k2 = slots_[i].key.load(cpp::MemoryOrder::RELAXED);
      if (k2 != k1)
        continue; // Writer intervened -- skip.

      if (slot_pkey != pkey)
        continue;

      // Compute effective POSIX prot from rights + original prot.
      int effective_prot;
      if (base_prot_override >= 0) {
        effective_prot = base_prot_override;
      } else {
        effective_prot = rights_to_prot(slot_prot, target_rights);
      }

      LIBC_NAMESPACE::mprotect(reinterpret_cast<void *>(k1), slot_size,
                               effective_prot);

      // Convergence check: if rights changed, a newer set_rights will
      // enforce the final value. Stop wasting syscalls.
      uint32_t current =
          authoritative_rights.load(cpp::MemoryOrder::ACQUIRE);
      if (current != target_rights)
        return;
    }
  }

  /// Translate pkey rights + original POSIX prot -> effective POSIX prot.
  static int rights_to_prot(int original_prot, uint32_t key_rights) {
    if (key_rights & 0x1 /*PKEY_DISABLE_ACCESS*/)
      return PROT_NONE;
    if (key_rights & 0x2 /*PKEY_DISABLE_WRITE*/)
      return original_prot & ~PROT_WRITE;
    return original_prot;
  }
};

// ---------------------------------------------------------------------------
// Range table instance -- file-scope inline storage
//
// PkeyRangeTable is the external workload-bounded table (up to 4096 slots
// in a demand-committed SectionRegion). The control struct is small (~40 B)
// and lives here. The PCB's g_pcb.pkey.range_table pointer references it.
//
// PkeyRangeTable's own ensure_init() lazily creates the SectionRegion
// backing store on first register_range call.
// ---------------------------------------------------------------------------

alignas(PkeyRangeTable) inline unsigned char
    g_pkey_range_storage[sizeof(PkeyRangeTable)] = {};

inline constexpr uint32_t PKEY_STATE_UNINITIALIZED = 0;
inline constexpr uint32_t PKEY_STATE_IN_PROGRESS = 1;
inline constexpr uint32_t PKEY_STATE_READY = 2;

LIBC_INLINE PkeyRangeTable *published_range_table() {
  return static_cast<PkeyRangeTable *>(g_pcb.pkey.range_table);
}

LIBC_INLINE PkeyRangeTable *construct_and_publish_range_table() {
  auto *table = ::new (g_pkey_range_storage) PkeyRangeTable();
  g_pcb.pkey.range_table = table;
  return table;
}

/// Ensure the range table is initialized and accessible.
///
/// Uses g_pcb.pkey.initialized as a three-state gate:
///   0 = uninitialized, 1 = constructing, 2 = ready.
/// The PkeyRangeTable struct is placement-new'd into inline storage on
/// first call; subsequent calls return the cached pointer.
LIBC_INLINE PkeyRangeTable *ensure_range_table() {
  auto &state = g_pcb.pkey;
  for (;;) {
    uint32_t init_state = state.initialized.load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_LIKELY(init_state == PKEY_STATE_READY))
      return static_cast<PkeyRangeTable *>(state.range_table);

    if (init_state == PKEY_STATE_UNINITIALIZED) {
      uint32_t expected = PKEY_STATE_UNINITIALIZED;
      if (!state.initialized.compare_exchange_strong(
              expected, PKEY_STATE_IN_PROGRESS, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::RELAXED))
        continue;

      auto *table = construct_and_publish_range_table();
      state.initialized.store(PKEY_STATE_READY, cpp::MemoryOrder::RELEASE);
      futex_addr::wake(&state.initialized, static_cast<uint32_t>(-1));
      return table;
    }

    futex_addr::wait(&state.initialized, PKEY_STATE_IN_PROGRESS, nullptr);
  }
}

LIBC_INLINE void pkey_fork_reinit() {
  auto &state = g_pcb.pkey;
  uint32_t init_state = state.initialized.load(cpp::MemoryOrder::ACQUIRE);
  if (init_state == PKEY_STATE_UNINITIALIZED)
    return;

  auto *table = published_range_table();
  if (!table) {
    state.range_table = nullptr;
    state.initialized.store(PKEY_STATE_UNINITIALIZED,
                            cpp::MemoryOrder::RELAXED);
    return;
  }

  // Once the control object pointer is published it is fully constructed.
  // The child keeps that object and only repairs any inherited inner
  // backing-store initialization that was interrupted by fork.
  table->fork_reinit();
  state.initialized.store(PKEY_STATE_READY, cpp::MemoryOrder::RELAXED);
}

// ---------------------------------------------------------------------------
// PKRU register helpers (x86_64 only)
// ---------------------------------------------------------------------------

#ifdef LIBC_TARGET_ARCH_IS_X86_64
[[gnu::target("pku")]]
#endif
LIBC_INLINE void write_pkru_for_key(int key, uint32_t key_rights) {
#ifdef LIBC_TARGET_ARCH_IS_X86_64
  unsigned pkru = _rdpkru_u32();
  pkru &= ~(PKEY_MASK << (key * PKEY_BITS_PER_KEY));
  pkru |= ((key_rights & PKEY_MASK) << (key * PKEY_BITS_PER_KEY));
  _wrpkru(pkru);
#else
  (void)key;
  (void)key_rights;
#endif
}

// ---------------------------------------------------------------------------
// Free functions — pkey operations on PCB-resident state
//
// These replace the dissolved PkeyState struct methods and operate on the
// PCB fields directly.
// ---------------------------------------------------------------------------

/// Allocate a protection key. Returns key number (1-15) or -1 on failure.
LIBC_INLINE int pkey_alloc_key(uint32_t initial_rights) {
  auto &state = g_pcb.pkey;
  uint32_t old_bits = state.allocated.load(cpp::MemoryOrder::RELAXED);
  for (;;) {
    // Key 0 is the default key and cannot be allocated.
    // The 0xFFFE mask excludes bit 0 regardless of bitmap state.
    uint32_t free = ~old_bits & 0xFFFE;
    if (free == 0)
      return -1;

    int key = __builtin_ctz(free);

    uint32_t new_bits = old_bits | (1u << key);
    if (state.allocated.compare_exchange_strong(old_bits, new_bits,
                                                cpp::MemoryOrder::ACQ_REL,
                                                cpp::MemoryOrder::RELAXED)) {
      state.rights[key].store(initial_rights & PKEY_MASK,
                              cpp::MemoryOrder::RELAXED);
      write_pkru_for_key(key, initial_rights & PKEY_MASK);
      return key;
    }
  }
}

LIBC_INLINE bool pkey_set_rights(int key, uint32_t new_rights);

/// Free a protection key. Restores original protection on all associated
/// pages and removes range tracking entries. Lock-free.
LIBC_INLINE bool pkey_free_key(int key) {
  if (key < 1 || key >= PKEY_COUNT)
    return false;

  auto &state = g_pcb.pkey;
  uint32_t old_bits = state.allocated.load(cpp::MemoryOrder::RELAXED);
  if (!(old_bits & (1u << key)))
    return false;

  // Restore all pages to their original protection.
  pkey_set_rights(key, 0);

  // Remove all range entries for this key.
  PkeyRangeTable *table = ensure_range_table();
  if (table)
    table->remove_key(key);

  // Clear PKRU bits and mark key as free.
  write_pkru_for_key(key, 0);
  state.rights[key].store(0, cpp::MemoryOrder::RELAXED);
  state.allocated.fetch_and(~(1u << key), cpp::MemoryOrder::RELEASE);
  return true;
}

/// Get current access rights for a key from PKRU (x86_64) or software state.
#ifdef LIBC_TARGET_ARCH_IS_X86_64
[[gnu::target("pku")]]
#endif
LIBC_INLINE int pkey_get_rights(int key) {
  if (key < 0 || key >= PKEY_COUNT)
    return -1;
#ifdef LIBC_TARGET_ARCH_IS_X86_64
  unsigned pkru = _rdpkru_u32();
  return static_cast<int>((pkru >> (key * PKEY_BITS_PER_KEY)) & PKEY_MASK);
#else
  return static_cast<int>(
      g_pcb.pkey.rights[key].load(cpp::MemoryOrder::RELAXED));
#endif
}

/// Set access rights for a key. Updates PKRU and enforces via mprotect()
/// on all tracked ranges. Lock-free and convergent: if two threads race,
/// the last writer's value wins and stale enforcers bail early.
LIBC_INLINE bool pkey_set_rights(int key, uint32_t new_rights) {
  if (key < 0 || key >= PKEY_COUNT)
    return false;
  if (new_rights > PKEY_MASK)
    return false;

  auto &state = g_pcb.pkey;
  uint32_t old_rights =
      state.rights[key].exchange(new_rights, cpp::MemoryOrder::RELEASE);
  write_pkru_for_key(key, new_rights);

  if (old_rights != new_rights) {
    PkeyRangeTable *table = ensure_range_table();
    if (table)
      table->enforce_key(key, new_rights, /*base_prot_override=*/-1,
                         state.rights[key]);
  }

  return true;
}

/// Register a range and apply current rights if restricted. Called from
/// pkey_mprotect after the base mprotect() succeeds.
LIBC_INLINE bool pkey_register_range(void *addr, SIZE_T size, int pkey,
                                     int posix_prot) {
  PkeyRangeTable *table = ensure_range_table();
  if (!table || !table->register_range(addr, size, pkey, posix_prot))
    return false;

  // If the key currently has restrictions, apply them immediately.
  uint32_t current_rights =
      g_pcb.pkey.rights[pkey].load(cpp::MemoryOrder::ACQUIRE);
  if (current_rights != 0) {
    int effective =
        PkeyRangeTable::rights_to_prot(posix_prot, current_rights);
    LIBC_NAMESPACE::mprotect(addr, size, effective);
  }

  return true;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PKEY_STATE_H
