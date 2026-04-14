//===-- Flat-slab thread registry for Windows --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/thread_registry.h"

#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/threads/windows/futex_addr.h"

namespace LIBC_NAMESPACE_DECL {

namespace robust_mutex {
void lifecycle_destroy(ThreadLifecycle *lc);
} // namespace robust_mutex

namespace {

// =========================================================================
// Access to the PCB-resident state
// =========================================================================

LIBC_INLINE ThreadRegistryState &state() {
  return g_pcb.thread_registry;
}

// =========================================================================
// Constants
// =========================================================================

constexpr ACCESS_MASK kThreadHandleAccess =
    THREAD_QUERY_LIMITED_INFORMATION | THREAD_TERMINATE | THREAD_SET_CONTEXT |
    THREAD_GET_CONTEXT | THREAD_SET_INFORMATION | SYNCHRONIZE;

constexpr size_t kPageIndexBytes = kMaxRegistryPages * sizeof(uintptr_t);
constexpr uint64_t kValidSlotMask = (1ULL << kSlotsPerPage) - 1;
constexpr uint32_t kHashLoadNumerator = 3;
constexpr uint32_t kHashLoadDenominator = 4; // resize at 75% load

// =========================================================================
// Atomic load helper (const-cast workaround for cpp::Atomic)
// =========================================================================

template <typename T>
LIBC_INLINE T load_atomic(const cpp::Atomic<T> &a, cpp::MemoryOrder order) {
  return const_cast<cpp::Atomic<T> &>(a).load(order);
}

// =========================================================================
// Page array access
// =========================================================================

LIBC_INLINE cpp::Atomic<uintptr_t> *page_array() {
  auto base = state().page_index_base.load(cpp::MemoryOrder::ACQUIRE);
  return reinterpret_cast<cpp::Atomic<uintptr_t> *>(base);
}

LIBC_INLINE RegistryPage *load_page(uint32_t index) {
  if (index >= kMaxRegistryPages)
    return nullptr;
  auto *arr = page_array();
  if (!arr)
    return nullptr;
  return reinterpret_cast<RegistryPage *>(
      arr[index].load(cpp::MemoryOrder::ACQUIRE));
}

// =========================================================================
// Lazy initialization
// =========================================================================

LIBC_INLINE TidHashTable *alloc_hash_table(uint32_t capacity) {
  size_t entry_bytes = capacity * sizeof(cpp::Atomic<uint64_t>);
  size_t total = sizeof(TidHashTable) + entry_bytes;
  void *mem = internal::page_alloc(total);
  if (!mem)
    return nullptr;
  __builtin_memset(mem, 0, total);

  auto *table = static_cast<TidHashTable *>(mem);
  table->entries = reinterpret_cast<cpp::Atomic<uint64_t> *>(
      static_cast<char *>(mem) + sizeof(TidHashTable));
  table->capacity = capacity;
  table->mask = capacity - 1;
  table->retired_next = nullptr;
  return table;
}

void free_hash_table(TidHashTable *table) {
  if (table)
    internal::page_free(table);
}

bool ensure_initialized() {
  while (true) {
    if (LIBC_LIKELY(state().init_state.load(cpp::MemoryOrder::ACQUIRE) == 2))
      return true;

    uint32_t expected = 0;
    if (!state().init_state.compare_exchange_strong(
            expected, 1, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE)) {
      if (expected == 2)
        return true; // already initialized

      // Another thread is mid-init (state == 1). Spin briefly, then wait.
      // We do NOT force-reset init_state — that would cause double-init and
      // leak the first initializer's allocations. Instead, trust the
      // initializer to finish and wait with a bounded kernel sleep.
      for (int round = 0; round < 3; ++round) {
        for (uint32_t spins = 0; spins < 4096; ++spins) {
          uint32_t s = state().init_state.load(cpp::MemoryOrder::ACQUIRE);
          if (s == 2)
            return true;
          if (s == 0)
            goto retry; // reset happened (init failed), retry from outer loop
#if defined(__x86_64__)
          __builtin_ia32_pause();
#elif defined(__aarch64__)
          __builtin_arm_yield();
#endif
        }
        // Yield to the OS scheduler to let the initializer run.
        ::NtYieldExecution();
      }
      // After ~12K spins + 3 yields, wait on the init_state address.
      // The initializer stores state=2 with RELEASE; we'll see it on retry.
      futex_addr::wait<uint32_t>(
          reinterpret_cast<const volatile uint32_t *>(&state().init_state), 1,
          nullptr);
    retry:
      continue;
    }

    // We won the CAS (0 → 1). Perform initialization.

    // Reserve VA for the page pointer array.
    void *index_va = internal::page_reserve(kPageIndexBytes);
    if (!index_va) {
      state().init_state.store(0, cpp::MemoryOrder::RELEASE);
      futex_addr::wake(
          reinterpret_cast<const volatile uint32_t *>(&state().init_state),
          INT32_MAX);
      return false;
    }

    // Commit the first page of the index (covers ~512 page pointers).
    if (!internal::page_commit(index_va, 4096)) {
      internal::page_free(index_va);
      state().init_state.store(0, cpp::MemoryOrder::RELEASE);
      futex_addr::wake(
          reinterpret_cast<const volatile uint32_t *>(&state().init_state),
          INT32_MAX);
      return false;
    }

    // Allocate the initial TID hash table.
    TidHashTable *table = alloc_hash_table(kInitialHashCapacity);
    if (!table) {
      internal::page_free(index_va);
      state().init_state.store(0, cpp::MemoryOrder::RELEASE);
      futex_addr::wake(
          reinterpret_cast<const volatile uint32_t *>(&state().init_state),
          INT32_MAX);
      return false;
    }

    state().page_index_base.store(reinterpret_cast<uintptr_t>(index_va),
                                  cpp::MemoryOrder::RELAXED);
    state().tid_table.store(reinterpret_cast<uintptr_t>(table),
                            cpp::MemoryOrder::RELAXED);
    state().global_epoch.store(1, cpp::MemoryOrder::RELAXED);
    state().page_count.store(0, cpp::MemoryOrder::RELAXED);
    state().free_hint.store(0, cpp::MemoryOrder::RELAXED);
    state().live_count.store(0, cpp::MemoryOrder::RELAXED);
    state().index_committed_bytes.store(4096, cpp::MemoryOrder::RELAXED);

    state().init_state.store(2, cpp::MemoryOrder::RELEASE);
    // Wake any threads waiting on init_state in the spin loop above.
    futex_addr::wake(
        reinterpret_cast<const volatile uint32_t *>(&state().init_state),
        INT32_MAX);
    return true;
  }
}

// =========================================================================
// Page management
// =========================================================================

RegistryPage *alloc_registry_page(uint32_t page_index) {
  void *mem = internal::page_alloc(sizeof(RegistryPage));
  if (!mem)
    return nullptr;
  __builtin_memset(mem, 0, sizeof(RegistryPage));
  auto *page = static_cast<RegistryPage *>(mem);
  page->page_index = page_index;
  return page;
}

RegistryPage *ensure_page(uint32_t page_index) {
  if (page_index >= kMaxRegistryPages)
    return nullptr;
  auto *arr = page_array();
  if (!arr)
    return nullptr;

  // Try to load existing page.
  auto *page = reinterpret_cast<RegistryPage *>(
      arr[page_index].load(cpp::MemoryOrder::ACQUIRE));
  if (page)
    return page;

  // Ensure the page index slot itself is committed. Each 4KB of the index
  // holds 512 pointers (x64). Use the high-water mark to avoid redundant
  // page_commit calls on already-committed memory.
  size_t needed_offset =
      (static_cast<size_t>(page_index) + 1) * sizeof(uintptr_t);
  size_t needed_committed = (needed_offset + 4095) & ~size_t{4095};
  uint32_t current_committed =
      state().index_committed_bytes.load(cpp::MemoryOrder::ACQUIRE);
  if (needed_committed > current_committed) {
    void *base = reinterpret_cast<void *>(
        state().page_index_base.load(cpp::MemoryOrder::RELAXED));
    void *target = static_cast<char *>(base) + current_committed;
    size_t extra = needed_committed - current_committed;
    if (!internal::page_commit(target, extra))
      return nullptr;
    // CAS-advance the high-water mark. Losers see a value >= theirs = fine.
    while (current_committed < static_cast<uint32_t>(needed_committed)) {
      if (state().index_committed_bytes.compare_exchange_weak(
              current_committed, static_cast<uint32_t>(needed_committed),
              cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE))
        break;
    }
  }

  // Allocate a new SlotPage.
  RegistryPage *fresh = alloc_registry_page(page_index);
  if (!fresh)
    return nullptr;

  // CAS the page pointer into the index. Losers free their allocation.
  uintptr_t expected = 0;
  if (arr[page_index].compare_exchange_strong(
          expected, reinterpret_cast<uintptr_t>(fresh),
          cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE)) {
    // Update page_count if we extended it.
    uint32_t prev_count = state().page_count.load(cpp::MemoryOrder::RELAXED);
    while (prev_count <= page_index) {
      state().page_count.compare_exchange_weak(prev_count, page_index + 1,
                                               cpp::MemoryOrder::ACQ_REL,
                                               cpp::MemoryOrder::RELAXED);
    }
    return fresh;
  }

  // Another thread published first. Use theirs, free ours.
  internal::page_free(fresh);
  return reinterpret_cast<RegistryPage *>(expected);
}

// =========================================================================
// TID hash table operations
// =========================================================================

LIBC_INLINE TidHashTable *load_tid_table() {
  return reinterpret_cast<TidHashTable *>(
      state().tid_table.load(cpp::MemoryOrder::ACQUIRE));
}

bool tid_hash_insert(TidHashTable *table, DWORD tid, uint32_t page,
                     uint32_t index) {
  uint64_t value = pack_tid_entry(tid, page, index);
  uint32_t bucket = hash_tid(tid) & table->mask;

  for (uint32_t probe = 0; probe < table->capacity; ++probe) {
    uint32_t idx = (bucket + probe) & table->mask;

    // Re-read on each attempt — CAS failure updates `current` in place,
    // so we re-examine the same bucket with the winner's value.
    while (true) {
      uint64_t current = table->entries[idx].load(cpp::MemoryOrder::ACQUIRE);

      if (current == kHashEmpty || current == kHashTombstone) {
        // Try to claim this slot.
        if (table->entries[idx].compare_exchange_strong(
                current, value, cpp::MemoryOrder::ACQ_REL,
                cpp::MemoryOrder::ACQUIRE))
          return true;
        // CAS failed — re-read and decide again for this same bucket.
        continue;
      }

      if (unpack_tid(current) == tid) {
        // TID already in table — update the slot reference. This happens
        // during hash rebuild (ground-truth rebuild from bitmap) or
        // re-registration with the same TID. Not a concurrent duplicate.
        table->entries[idx].store(value, cpp::MemoryOrder::RELEASE);
        return true;
      }

      // Occupied by a different TID — advance to next probe position.
      break;
    }
  }
  return false; // table full (shouldn't happen with resize at 75%)
}

void tid_hash_delete(TidHashTable *table, DWORD tid) {
  uint32_t bucket = hash_tid(tid) & table->mask;

  for (uint32_t probe = 0; probe < table->capacity; ++probe) {
    uint32_t idx = (bucket + probe) & table->mask;
    uint64_t current = table->entries[idx].load(cpp::MemoryOrder::ACQUIRE);

    if (current == kHashEmpty)
      return; // not found

    if (current != kHashTombstone && unpack_tid(current) == tid) {
      table->entries[idx].store(kHashTombstone, cpp::MemoryOrder::RELEASE);
      state().tombstone_count.fetch_add(1, cpp::MemoryOrder::RELAXED);
      return;
    }
  }
}

uint64_t tid_hash_find(TidHashTable *table, DWORD tid) {
  uint32_t bucket = hash_tid(tid) & table->mask;

  for (uint32_t probe = 0; probe < table->capacity; ++probe) {
    uint32_t idx = (bucket + probe) & table->mask;
    uint64_t current = table->entries[idx].load(cpp::MemoryOrder::ACQUIRE);

    if (current == kHashEmpty)
      return 0;

    if (current != kHashTombstone && unpack_tid(current) == tid)
      return current;
  }
  return 0;
}

/// Core resize: acquire lock, allocate new table of `new_capacity`, rebuild
/// from bitmap ground truth, publish, retire old table. Caller must verify
/// the resize is needed before calling — this always executes if the lock
/// is available.
void tid_hash_do_resize(TidHashTable *old_table, uint32_t new_capacity) {
  // Try to acquire the resize lock.
  uint32_t expected = 0;
  if (!state().resize_lock.compare_exchange_strong(
          expected, 1, cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::RELAXED))
    return; // another thread is resizing

  TidHashTable *new_table = alloc_hash_table(new_capacity);
  if (!new_table) {
    state().resize_lock.store(0, cpp::MemoryOrder::RELEASE);
    return;
  }

  // Rebuild from ground truth (bitmap + slot.tid) instead of copying from
  // the old table. This eliminates the race where a concurrent insert lands
  // in the old table after we copy but before we publish the new one.
  uint32_t page_count = state().page_count.load(cpp::MemoryOrder::ACQUIRE);
  for (uint32_t p = 0; p < page_count; ++p) {
    auto *rpage = reinterpret_cast<RegistryPage *>(
        page_array()[p].load(cpp::MemoryOrder::ACQUIRE));
    if (!rpage)
      continue;
    uint64_t bitmap = rpage->bitmap.load(cpp::MemoryOrder::ACQUIRE);
    bitmap &= kValidSlotMask; // mask off retiring sentinel / high bits
    while (bitmap) {
      uint32_t idx = static_cast<uint32_t>(__builtin_ctzll(bitmap));
      bitmap &= bitmap - 1;
      // Check lifecycle first: a concurrent deregister clears lifecycle
      // before clearing the bitmap bit, so a null lifecycle means the slot
      // is mid-deregister — skip it to avoid phantom hash entries.
      // Lifecycle and tid share the same cache line — zero extra cost.
      if (!rpage->slots[idx].lifecycle.load(cpp::MemoryOrder::ACQUIRE))
        continue;
      DWORD slot_tid = rpage->slots[idx].tid.load(cpp::MemoryOrder::RELAXED);
      if (slot_tid)
        tid_hash_insert(new_table, slot_tid, p, idx);
    }
  }

  // Publish the new table, then reset tombstone count. The RELEASE on
  // tid_table ensures the new table is visible before the count reset.
  // Resetting after publish prevents a concurrent tid_hash_delete (into
  // the old table) from incrementing the count between reset and publish.
  state().tid_table.store(reinterpret_cast<uintptr_t>(new_table),
                          cpp::MemoryOrder::RELEASE);
  state().tombstone_count.store(0, cpp::MemoryOrder::RELEASE);

  // Retire the old table. Stamp with current epoch and push onto the
  // retired list. The reclaimer frees it once min_pinned_epoch advances past.
  old_table->retired_epoch = state().global_epoch.load(cpp::MemoryOrder::ACQUIRE);
  uintptr_t old_head;
  do {
    old_head = state().retired_tables.load(cpp::MemoryOrder::RELAXED);
    old_table->retired_next = reinterpret_cast<TidHashTable *>(old_head);
  } while (!state().retired_tables.compare_exchange_weak(
      old_head, reinterpret_cast<uintptr_t>(old_table),
      cpp::MemoryOrder::RELEASE, cpp::MemoryOrder::RELAXED));

  state().resize_lock.store(0, cpp::MemoryOrder::RELEASE);
}

/// Grow the hash table if occupied slots (live + tombstones) exceed 75%.
void tid_hash_grow_if_needed(TidHashTable *table) {
  uint32_t live = state().live_count.load(cpp::MemoryOrder::RELAXED);
  uint32_t tombstones = state().tombstone_count.load(cpp::MemoryOrder::RELAXED);
  uint32_t occupied = live + tombstones;
  if (occupied * kHashLoadDenominator < table->capacity * kHashLoadNumerator)
    return; // under 75% threshold
  tid_hash_do_resize(table, table->capacity * 2);
}

/// Shrink the hash table if live count drops below 25% and we're above the
/// initial capacity. Avoids keeping peak-sized tables after thread count drops.
void tid_hash_shrink_if_needed(TidHashTable *table) {
  uint32_t live = state().live_count.load(cpp::MemoryOrder::RELAXED);
  if (table->capacity <= kInitialHashCapacity)
    return; // already at minimum
  // Shrink when live entries use less than 25% of capacity. The new table
  // at half capacity will be ~50% full — well within the 75% grow threshold.
  if (live * 4 >= table->capacity)
    return; // above 25% — no shrink
  tid_hash_do_resize(table, table->capacity / 2);
}

// =========================================================================
// Slot allocation
// =========================================================================

/// Find a free slot and claim it. Returns the page and index, or false.
///
/// Uses fetch_or instead of CAS on the bitmap. fetch_or (x86 `lock or`,
/// ARM `ldset`) never spuriously fails — non-conflicting bit claims on
/// the same bitmap word don't interfere. Under N concurrent registrations:
///
///   1. All N threads read bitmap, pick lowest free bit via ctzll.
///   2. All N issue fetch_or(1 << idx). Exactly one sees old bit == 0
///      (winner). The other N-1 see old bit == 1 (already taken) —
///      their fetch_or was idempotent (OR 1 with 1 = 1, no side effect).
///   3. Losers re-read bitmap, pick the next free bit (different now
///      since the bitmap evolved). Total: O(N) atomics, not O(N²).
///
/// In the uncontended case: one fetch_or, one bit test — same cost and
/// same bottom-up packing as a bare ctzll + CAS approach.
bool allocate_slot(uint32_t &out_page, uint32_t &out_index) {
  // Outer retry: if the new-page fast path loses the race and the page
  // fills before we claim a slot, reload page_count and scan again. The
  // winning thread advanced page_count, so the next iteration sees the
  // new page (or a further one). Bounded to prevent infinite loops under
  // pathological contention; 3 retries is generous — each one allocates
  // a fresh 63-slot page, so we'd need 189+ concurrent registrations to
  // exhaust all retries.
  for (uint32_t retry = 0; retry < 3; ++retry) {
    uint32_t hint = state().free_hint.load(cpp::MemoryOrder::RELAXED);
    uint32_t count = state().page_count.load(cpp::MemoryOrder::ACQUIRE);

    // Scan existing pages starting from the free hint.
    for (uint32_t attempt = 0; attempt < count; ++attempt) {
      uint32_t p = (hint + attempt < count) ? hint + attempt : attempt;
      if (p >= count)
        break;

      RegistryPage *page = load_page(p);
      if (!page) {
        // Hole left by a retired page. Backfill: allocate a fresh page here
        // to reuse the index instead of growing page_count monotonically.
        page = ensure_page(p);
        if (!page)
          continue;
      }

      // Try to claim a free bit via fetch_or.
      while (true) {
        uint64_t bitmap = page->bitmap.load(cpp::MemoryOrder::ACQUIRE);
        uint64_t free_bits = ~bitmap & kValidSlotMask;
        if (free_bits == 0)
          break; // page full

        uint32_t idx = static_cast<uint32_t>(__builtin_ctzll(free_bits));
        uint64_t bit = 1ULL << idx;

        // Atomically set the bit. fetch_or never spuriously fails —
        // unlike CAS, concurrent claims on OTHER bits don't cause retries.
        uint64_t old = page->bitmap.fetch_or(bit, cpp::MemoryOrder::ACQ_REL);
        if ((old & bit) == 0) {
          // Bit was free, now set — we own this slot.
          out_page = p;
          out_index = idx;
          return true;
        }
        // Bit was already set by another thread between our load and
        // fetch_or. Our OR was idempotent (no side effect). Re-read
        // bitmap and pick the next free bit.
      }
    }

    // All existing pages are full. Allocate a new one.
    if (count >= kMaxRegistryPages)
      return false; // hard limit reached

    RegistryPage *page = ensure_page(count);
    if (!page)
      return false;

    // Claim slot 0 in the new page via fetch_or.
    uint64_t old = page->bitmap.fetch_or(1ULL, cpp::MemoryOrder::ACQ_REL);
    if ((old & 1ULL) == 0) {
      out_page = count;
      out_index = 0;
      return true;
    }

    // Another thread claimed slot 0. Scan the new page for the next free bit.
    while (true) {
      uint64_t bitmap = page->bitmap.load(cpp::MemoryOrder::ACQUIRE);
      uint64_t free_bits = ~bitmap & kValidSlotMask;
      if (free_bits == 0)
        break; // Page filled by racers. Retry from top with fresh page_count.

      uint32_t idx = static_cast<uint32_t>(__builtin_ctzll(free_bits));
      uint64_t bit = 1ULL << idx;
      old = page->bitmap.fetch_or(bit, cpp::MemoryOrder::ACQ_REL);
      if ((old & bit) == 0) {
        out_page = count;
        out_index = idx;
        return true;
      }
    }
  }

  return false;
}

// =========================================================================
// Handle duplication
// =========================================================================

LIBC_INLINE bool duplicate_thread_handle(HANDLE source, HANDLE *out) {
  NTSTATUS st = ::NtDuplicateObject(NtCurrentProcess(), source,
                                    NtCurrentProcess(), out,
                                    kThreadHandleAccess, 0, 0);
  return NT_SUCCESS(st);
}

// =========================================================================
// Epoch protocol
// =========================================================================

LIBC_INLINE RegistrySlot *current_thread_slot() {
  ThreadLifecycle *lc = get_current_lifecycle();
  if (!lc || lc->slot_page.load(cpp::MemoryOrder::RELAXED) == UINT32_MAX)
    return nullptr;
  uint32_t pg = lc->slot_page.load(cpp::MemoryOrder::RELAXED);
  RegistryPage *page = load_page(pg);
  if (!page)
    return nullptr;
  return &page->slots[lc->slot_index];
}

// =========================================================================
// Lifecycle cleanup for fork_reinit
// =========================================================================

void cleanup_stale_lifecycle(ThreadLifecycle *lc) {
  if (!lc)
    return;

  // Thread handles were duplicated via NtDuplicateObject(..., 0, 0)
  // (no OBJ_INHERIT), so they don't exist in the child's handle table.
  // Just null the stale pointer — no NtClose.
  lc->thread_handle.store(nullptr, cpp::MemoryOrder::RELAXED);
  lc->owner_tid.store(0, cpp::MemoryOrder::RELAXED);
  lc->slot_page.store(UINT32_MAX, cpp::MemoryOrder::RELAXED);

  // Clean up robust mutex records and free pool-allocated lifecycles.
  // Post-fork, these threads are dead — their robust records need marking
  // as OWNER_DIED, and pool memory needs returning.
  robust_mutex::lifecycle_destroy(lc);
  if (lc->dynamically_allocated)
    free_lifecycle(lc);
}

// =========================================================================
// Page retirement
// =========================================================================

/// Try to retire an empty page. Called from deregister when bitmap drops to 0.
/// Uses a two-phase CAS protocol:
///   1. CAS bitmap 0 → kBitmapRetiring (blocks allocators from claiming slots)
///   2. CAS page array entry → nullptr (blocks new readers)
///   3. Push page onto retired_pages list (freed after epoch quiescence)
void try_retire_page(uint32_t page_idx, RegistryPage *page) {
  // Phase 1: claim the page by marking bitmap as retiring.
  uint64_t zero = 0;
  if (!page->bitmap.compare_exchange_strong(zero, kBitmapRetiring,
                                            cpp::MemoryOrder::ACQ_REL,
                                            cpp::MemoryOrder::RELAXED)) {
    // Someone set a bit between our fetch_and and now — page is live again.
    return;
  }

  // Phase 2: remove from the page index. New readers won't find it.
  auto *arr = page_array();
  if (!arr) {
    // Shouldn't happen, but recover gracefully.
    page->bitmap.store(0, cpp::MemoryOrder::RELEASE);
    return;
  }

  uintptr_t expected_ptr = reinterpret_cast<uintptr_t>(page);
  if (!arr[page_idx].compare_exchange_strong(expected_ptr, 0,
                                             cpp::MemoryOrder::ACQ_REL,
                                             cpp::MemoryOrder::RELAXED)) {
    // Someone replaced the page pointer (ensure_page raced). Undo the bitmap.
    page->bitmap.store(0, cpp::MemoryOrder::RELEASE);
    return;
  }

  // Phase 3: stamp with current epoch and push onto the retired pages list.
  page->retired_epoch = state().global_epoch.load(cpp::MemoryOrder::ACQUIRE);
  uintptr_t old_head;
  do {
    old_head = state().retired_pages.load(cpp::MemoryOrder::RELAXED);
    page->retired_next = reinterpret_cast<RegistryPage *>(old_head);
  } while (!state().retired_pages.compare_exchange_weak(
      old_head, reinterpret_cast<uintptr_t>(page),
      cpp::MemoryOrder::RELEASE, cpp::MemoryOrder::RELAXED));

  // If free_hint was pointing at or past this page, reset it to this index
  // so allocate_slot will backfill the hole via ensure_page.
  uint32_t hint = state().free_hint.load(cpp::MemoryOrder::RELAXED);
  if (hint >= page_idx)
    state().free_hint.store(page_idx, cpp::MemoryOrder::RELAXED);

  // Tail-compact page_count: if this was the last page (or near it),
  // decrement page_count while trailing entries are null. This avoids
  // iterators scanning dead indices after high-water pages are retired.
  uint32_t count = state().page_count.load(cpp::MemoryOrder::ACQUIRE);
  while (count > 0) {
    uintptr_t tail_ptr = arr[count - 1].load(cpp::MemoryOrder::ACQUIRE);
    if (tail_ptr != 0)
      break; // non-null page at the tail — stop compacting
    // Try to decrement. If another thread already changed it, reload and retry.
    if (state().page_count.compare_exchange_weak(
            count, count - 1, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE))
      --count;
    // CAS failure reloads count via the weak CAS's expected update.
  }
}

} // anonymous namespace

// =========================================================================
// Non-template iteration loop (single copy in the binary)
// =========================================================================

bool registry_for_each_impl(RegistryVisitorFn visitor, void *ctx,
                                DWORD skip_tid) {
  auto *arr = page_array();
  if (!arr)
    return false;

  // Auto-pin: the visitor may safely borrow handles and read lifecycle
  // fields without an external EpochGuard. Nestable via pin_depth.
  registry_pin();

  bool early_exit = false;
  uint32_t count = state().page_count.load(cpp::MemoryOrder::ACQUIRE);
  for (uint32_t p = 0; p < count; ++p) {
    auto *page = reinterpret_cast<RegistryPage *>(
        arr[p].load(cpp::MemoryOrder::ACQUIRE));
    if (!page)
      continue;

    uint64_t bitmap = page->bitmap.load(cpp::MemoryOrder::ACQUIRE);
    bitmap &= kValidSlotMask; // mask off retiring sentinel / high bits
    while (bitmap) {
      uint32_t idx = static_cast<uint32_t>(__builtin_ctzll(bitmap));
      bitmap &= bitmap - 1;

      RegistrySlot &slot = page->slots[idx];
      if (skip_tid && slot.tid.load(cpp::MemoryOrder::RELAXED) == skip_tid)
        continue;

      ThreadLifecycle *lc = slot.lifecycle.load(cpp::MemoryOrder::ACQUIRE);
      if (lc && visitor(ctx, lc)) {
        early_exit = true;
        goto done;
      }
    }
  }
done:
  registry_unpin();
  return early_exit;
}

// =========================================================================
// Public API implementation
// =========================================================================

SlotRef registry_register(ThreadLifecycle *lc, HANDLE thread_handle,
                             DWORD tid, bool duplicate_handle) {
  if (!ensure_initialized())
    return SlotRef::invalid();

  // Idempotency: if this lifecycle is already registered with the same TID,
  // return the existing slot. This happens when a foreign thread registers
  // via robust mutex and later triggers signal state initialization — both
  // paths call registry_register_self with the same thread identity.
  //
  // Re-registering would churn a slot and close the old handle while
  // epoch-pinned readers may still be borrowing it (use-after-close race).
  // Returning the existing slot avoids all of that.
  if (lc->slot_page.load(cpp::MemoryOrder::ACQUIRE) != UINT32_MAX) {
    DWORD existing_tid = lc->owner_tid.load(cpp::MemoryOrder::ACQUIRE);
    if (existing_tid == tid)
      return {lc->slot_page.load(cpp::MemoryOrder::RELAXED), lc->slot_index,
              lc->slot_generation};
    // Different TID — genuinely re-registering (e.g., fork child inheriting
    // a parent lifecycle). Deregister the old slot and close its handle.
    // Safe: fork reinit is single-threaded, so no concurrent borrowers.
    HANDLE old_handle = lc->thread_handle.load(cpp::MemoryOrder::RELAXED);
    registry_deregister(lc);
    if (old_handle) {
      ::NtClose(old_handle);
      lc->thread_handle.store(nullptr, cpp::MemoryOrder::RELAXED);
    }
  }

  // Prepare the handle.
  HANDLE owned_handle = thread_handle;
  if (duplicate_handle) {
    if (!duplicate_thread_handle(thread_handle, &owned_handle))
      return SlotRef::invalid();
  }

  // Find a free slot.
  uint32_t page_idx = 0, slot_idx = 0;
  if (!allocate_slot(page_idx, slot_idx)) {
    if (duplicate_handle && owned_handle)
      ::NtClose(owned_handle);
    return SlotRef::invalid();
  }

  // Initialize the slot.
  RegistryPage *page = load_page(page_idx);
  RegistrySlot &slot = page->slots[slot_idx];
  uint32_t gen = slot.generation + 1;
  slot.generation = gen;
  slot.tid.store(tid, cpp::MemoryOrder::RELAXED);
  slot.pin_depth = 0;
  slot.pinned_epoch.store(0, cpp::MemoryOrder::RELAXED);

  // Publish handle and lifecycle.
  lc->thread_handle.store(owned_handle, cpp::MemoryOrder::RELAXED);
  lc->owner_tid.store(tid, cpp::MemoryOrder::RELEASE);

  // Store slot ref in lifecycle for pin/unpin and deregister.
  // slot_page is stored last with RELEASE — it doubles as the registration
  // sentinel (UINT32_MAX = not registered), so cross-thread readers that
  // ACQUIRE-load slot_page see consistent slot_index/slot_generation.
  lc->slot_index = slot_idx;
  lc->slot_generation = gen;
  lc->slot_page.store(page_idx, cpp::MemoryOrder::RELEASE);
  // Publish lifecycle pointer (the slot is already visible via bitmap).
  slot.lifecycle.store(lc, cpp::MemoryOrder::RELEASE);

  // Insert into TID hash table.
  //
  // Visibility window: between slot.lifecycle.store above and the hash
  // insert below, registry_find_by_tid(tid) will NOT find this thread.
  // registry_for_each WILL see it (bitmap bit is already set). This
  // window is benign: signal delivery and cancellation retry on ESRCH,
  // and the thread hasn't started user code yet (it's still in bootstrap).
  TidHashTable *table = load_tid_table();
  tid_hash_insert(table, tid, page_idx, slot_idx);
  state().live_count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);

  // Check if hash table needs to grow.
  tid_hash_grow_if_needed(table);

  // Update free hint if we used a page before the current hint.
  uint32_t current_hint = state().free_hint.load(cpp::MemoryOrder::RELAXED);
  if (page_idx < current_hint)
    state().free_hint.store(page_idx, cpp::MemoryOrder::RELAXED);

  return {page_idx, slot_idx, gen};
}

SlotRef registry_register_self(ThreadLifecycle *lc) {
  return registry_register(lc, NtCurrentThread(), NtCurrentThreadId(),
                              /*duplicate_handle=*/true);
}

void registry_deregister(SlotRef ref) {
  RegistryPage *page = load_page(ref.page);
  if (!page)
    return;

  RegistrySlot &slot = page->slots[ref.index];

  // ABA check.
  if (slot.generation != ref.generation)
    return;

  // Load lifecycle to get TID for hash deletion.
  ThreadLifecycle *lc = slot.lifecycle.load(cpp::MemoryOrder::ACQUIRE);
  DWORD tid = slot.tid.load(cpp::MemoryOrder::RELAXED);

  // Delete from TID hash table first — find_by_tid won't resolve this TID.
  TidHashTable *table = load_tid_table();
  if (table)
    tid_hash_delete(table, tid);

  // Clear slot contents BEFORE bitmap. This ordering ensures that a
  // concurrent hash rebuild (tid_hash_do_resize) that sees the bitmap bit
  // still set will load lifecycle=null and skip the slot — eliminating
  // phantom hash entries entirely. Iterators (registry_for_each) that
  // loaded the old bitmap will see lifecycle=null and skip safely.
  slot.lifecycle.store(nullptr, cpp::MemoryOrder::RELEASE);
  slot.tid.store(0, cpp::MemoryOrder::RELAXED);

  // Now clear the bitmap bit. After this, new iterators and rebuilds
  // won't visit this slot at all.
  uint64_t bit = 1ULL << ref.index;
  uint64_t old_bitmap = page->bitmap.fetch_and(~bit, cpp::MemoryOrder::ACQ_REL);
  uint64_t new_bitmap = old_bitmap & ~bit;

  // Decrement live count.
  state().live_count.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);

  // Update free hint.
  uint32_t current_hint = state().free_hint.load(cpp::MemoryOrder::RELAXED);
  if (ref.page < current_hint)
    state().free_hint.store(ref.page, cpp::MemoryOrder::RELAXED);

  // Clear lifecycle's registry link. Reset slot coordinates to UINT32_MAX
  // so current_thread_slot() returns nullptr — prevents a stale pin_depth
  // write to a freed or reused slot if anything calls registry_pin() after
  // deregistration.
  if (lc) {
    lc->slot_page.store(UINT32_MAX, cpp::MemoryOrder::RELEASE);
    lc->slot_index = UINT32_MAX;
    lc->owner_tid.store(0, cpp::MemoryOrder::RELEASE);
  }

  // If this was the last slot on the page, try to retire and free it.
  if (new_bitmap == 0)
    try_retire_page(ref.page, page);

  // Check if the hash table should shrink after the live count dropped.
  TidHashTable *cur_table = load_tid_table();
  if (cur_table)
    tid_hash_shrink_if_needed(cur_table);
}

void registry_deregister(ThreadLifecycle *lc) {
  if (!lc)
    return;
  uint32_t pg = lc->slot_page.load(cpp::MemoryOrder::ACQUIRE);
  if (pg == UINT32_MAX)
    return;
  SlotRef ref{pg, lc->slot_index, lc->slot_generation};
  registry_deregister(ref);
}

// --- Epoch Protocol ---

void registry_pin() {
  RegistrySlot *slot = current_thread_slot();
  if (!slot)
    return;
  // Nesting: only store the epoch on the first pin. Subsequent pins just
  // increment the depth counter. pin_depth is thread-local (no atomic needed).
  if (slot->pin_depth++ == 0) {
    uint64_t epoch = state().global_epoch.load(cpp::MemoryOrder::ACQUIRE);
    slot->pinned_epoch.store(epoch, cpp::MemoryOrder::RELEASE);
  }
}

void registry_unpin() {
  RegistrySlot *slot = current_thread_slot();
  if (!slot)
    return;
  // Only clear the epoch pin when the outermost guard releases.
  if (slot->pin_depth > 0 && --slot->pin_depth == 0)
    slot->pinned_epoch.store(0, cpp::MemoryOrder::RELEASE);
}

// --- Point Lookups ---

ThreadLifecycle *registry_find_by_tid(DWORD tid) {
  TidHashTable *table = load_tid_table();
  if (!table)
    return nullptr;

  uint64_t packed = tid_hash_find(table, tid);
  if (packed == 0)
    return nullptr;

  uint32_t page_idx = unpack_page(packed);
  uint32_t slot_idx = unpack_index(packed);

  RegistryPage *page = load_page(page_idx);
  if (!page)
    return nullptr;

  ThreadLifecycle *lc =
      page->slots[slot_idx].lifecycle.load(cpp::MemoryOrder::ACQUIRE);
  if (!lc)
    return nullptr;

  // Verify TID still matches (guards against stale hash entries during
  // concurrent deregister + re-register with different TID).
  if (lc->owner_tid.load(cpp::MemoryOrder::ACQUIRE) != tid)
    return nullptr;

  return lc;
}

ThreadLifecycle *registry_find_by_task_id(uint32_t task_id) {
  return registry_find_if(
      [task_id](ThreadLifecycle *lc) { return lc->task_id == task_id; });
}

// --- Batch TID Extraction ---

uint32_t registry_collect_tids(HANDLE *buf, uint32_t capacity,
                                  DWORD skip_tid, uint32_t cursor,
                                  uint32_t *next_cursor) {
  auto *arr = page_array();
  if (!arr || !buf || capacity == 0) {
    if (next_cursor)
      *next_cursor = UINT32_MAX;
    return 0;
  }

  // Unpack cursor: bits [15:6] = page index, bits [5:0] = first slot to scan.
  uint32_t start_page = (cursor >> 6) & 0x3FF;
  uint32_t start_slot = cursor & 0x3F;

  uint32_t count = 0;
  uint32_t page_count = state().page_count.load(cpp::MemoryOrder::ACQUIRE);

  for (uint32_t p = start_page; p < page_count && count < capacity; ++p) {
    auto *page = reinterpret_cast<RegistryPage *>(
        arr[p].load(cpp::MemoryOrder::ACQUIRE));
    if (!page) {
      start_slot = 0; // reset for subsequent pages
      continue;
    }

    uint64_t bitmap = page->bitmap.load(cpp::MemoryOrder::ACQUIRE);
    bitmap &= kValidSlotMask; // mask off retiring sentinel / high bits

    // On the first page, skip slots below start_slot to avoid re-emitting.
    if (start_slot > 0) {
      bitmap &= ~((1ULL << start_slot) - 1);
      start_slot = 0; // only applies to the first page
    }

    while (bitmap && count < capacity) {
      uint32_t idx = static_cast<uint32_t>(__builtin_ctzll(bitmap));
      bitmap &= bitmap - 1;

      DWORD tid = page->slots[idx].tid.load(cpp::MemoryOrder::RELAXED);
      if (tid && tid != skip_tid)
        buf[count++] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(tid));

      // If buffer is now full, encode resume cursor for the next slot.
      if (count == capacity && bitmap) {
        uint32_t next_idx = static_cast<uint32_t>(__builtin_ctzll(bitmap));
        if (next_cursor)
          *next_cursor = (p << 6) | next_idx;
        return count;
      }
    }
  }

  if (next_cursor)
    *next_cursor = UINT32_MAX;
  return count;
}

// --- Batch Alert ---

void registry_alert_all(DWORD skip_tid) {
  uint32_t live = state().live_count.load(cpp::MemoryOrder::ACQUIRE);
  if (live == 0)
    return;

  // live_count is sampled once — threads registering concurrently may not be
  // included. This is safe: newly registered threads haven't entered any wait
  // that needs alerting. Threads deregistering concurrently get a harmless
  // spurious alert (the TID is no longer valid, NtAlertThreadByThreadId is
  // a no-op for stale TIDs).

  // Fast path: use a stack buffer for small thread counts to avoid the 4KB
  // minimum page_alloc overhead.
  constexpr uint32_t kStackBufSize = 64;
  HANDLE stack_buf[kStackBufSize];

  HANDLE *tid_buf;
  bool heap_allocated = false;
  if (live <= kStackBufSize) {
    tid_buf = stack_buf;
  } else {
    size_t buf_bytes = static_cast<size_t>(live) * sizeof(HANDLE);
    tid_buf = static_cast<HANDLE *>(internal::page_alloc(buf_bytes));
    if (!tid_buf)
      return;
    heap_allocated = true;
  }

  uint32_t count = registry_collect_tids(tid_buf, live, skip_tid);
  if (count > 0) {
    NTSTATUS st = nt_optional().alert_multiple(tid_buf, count,
                                               nullptr, 0);
    if (!NT_SUCCESS(st)) {
      // Batch call failed — fall back to per-thread alerts so callers
      // waiting on NtWaitForAlertByThreadId are guaranteed to wake.
      for (uint32_t i = 0; i < count; ++i)
        ::NtAlertThreadByThreadId(tid_buf[i]);
    }
  }

  if (heap_allocated)
    internal::page_free(tid_buf);
}

// --- Suspend / Resume ---

bool registry_suspend(ThreadLifecycle *lc) {
  if (!lc)
    return false;
  HANDLE h = lc->thread_handle.load(cpp::MemoryOrder::ACQUIRE);
  if (!h)
    return false;

  HANDLE sc = nullptr;
  NTSTATUS st = ::NtCreateThreadStateChange(&sc, THREAD_STATE_ALL_ACCESS,
                                            nullptr, h, 0);
  if (!NT_SUCCESS(st))
    return false;

  st = ::NtChangeThreadState(sc, h, ThreadStateSuspend, nullptr, 0, 0);
  ::NtClose(sc);
  return NT_SUCCESS(st);
}

bool registry_resume(ThreadLifecycle *lc) {
  if (!lc)
    return false;
  HANDLE h = lc->thread_handle.load(cpp::MemoryOrder::ACQUIRE);
  if (!h)
    return false;

  HANDLE sc = nullptr;
  NTSTATUS st = ::NtCreateThreadStateChange(&sc, THREAD_STATE_ALL_ACCESS,
                                            nullptr, h, 0);
  if (!NT_SUCCESS(st))
    return false;

  st = ::NtChangeThreadState(sc, h, ThreadStateResume, nullptr, 0, 0);
  ::NtClose(sc);
  return NT_SUCCESS(st);
}

// --- Epoch Scan (non-blocking) ---

/// Scan all live slots and return the minimum pinned epoch. If no thread
/// is currently pinned, returns UINT64_MAX (meaning everything is safe to
/// free). Non-blocking: reads only, no waiting, no spinning.
///
/// Does not require the caller to be epoch-pinned. The scan reads only
/// pinned_epoch fields (always valid for live slots via bitmap); it never
/// dereferences lifecycle pointers, so epoch protection is unnecessary.
///
/// Cost: O(live_threads) — one cache-line read per live slot via bitmap.
uint64_t scan_min_pinned_epoch() {
  uint64_t min_epoch = UINT64_MAX;
  auto *arr = page_array();
  if (!arr)
    return min_epoch;

  uint32_t count = state().page_count.load(cpp::MemoryOrder::ACQUIRE);
  for (uint32_t p = 0; p < count; ++p) {
    auto *page = reinterpret_cast<RegistryPage *>(
        arr[p].load(cpp::MemoryOrder::ACQUIRE));
    if (!page)
      continue;

    uint64_t bitmap = page->bitmap.load(cpp::MemoryOrder::ACQUIRE);
    bitmap &= kValidSlotMask;
    while (bitmap) {
      uint32_t s = static_cast<uint32_t>(__builtin_ctzll(bitmap));
      bitmap &= bitmap - 1;

      uint64_t pinned =
          page->slots[s].pinned_epoch.load(cpp::MemoryOrder::ACQUIRE);
      // pinned == 0 means unpinned — not a constraint on reclamation.
      if (pinned != 0 && pinned < min_epoch)
        min_epoch = pinned;
    }
  }
  return min_epoch;
}

// --- Deferred Reclamation ---

/// Retire a lifecycle for deferred epoch-safe free. Stamps the current
/// global epoch, pushes onto the retired_lifecycles list, and returns
/// immediately. The lifecycle memory stays alive until the reclaimer
/// determines no reader can still reference it.
///
/// The epoch stamp may be slightly stale if global_epoch advances between
/// our load and the CAS push. This is safe (conservative direction): a
/// stale stamp means the item becomes reclaimable sooner, and the item
/// is already deregistered (bitmap/hash cleared) so no new reader can
/// discover it regardless of the stamp.
void retire_lifecycle(ThreadLifecycle *lc) {
  lc->retired_epoch = state().global_epoch.load(cpp::MemoryOrder::ACQUIRE);

  uintptr_t old_head;
  do {
    old_head = state().retired_lifecycles.load(cpp::MemoryOrder::RELAXED);
    lc->retired_next = reinterpret_cast<ThreadLifecycle *>(old_head);
  } while (!state().retired_lifecycles.compare_exchange_weak(
      old_head, reinterpret_cast<uintptr_t>(lc),
      cpp::MemoryOrder::RELEASE, cpp::MemoryOrder::RELAXED));
}

/// Push-back helper: re-push a singly-linked chain onto an atomic list
/// in a single CAS (batch push). The chain must be terminated with nullptr.
/// This avoids per-item CAS loops when pushing back multiple kept items.
template <typename T>
void push_chain(cpp::Atomic<uintptr_t> &list_head, T *chain_head,
                T *chain_tail, T *T::*next_field) {
  if (!chain_head)
    return;
  uintptr_t old_head;
  do {
    old_head = list_head.load(cpp::MemoryOrder::RELAXED);
    chain_tail->*next_field = reinterpret_cast<T *>(old_head);
  } while (!list_head.compare_exchange_weak(
      old_head, reinterpret_cast<uintptr_t>(chain_head),
      cpp::MemoryOrder::RELEASE, cpp::MemoryOrder::RELAXED));
}

// =========================================================================
// Retired-list helpers
// =========================================================================
//
// Two operations on retired singly-linked lists:
//   drain_retired  — unconditionally free every item (single-threaded).
//   reclaim_retired — epoch-conditional: free items with retired_epoch <
//                     safe_epoch, push the rest back. Returns true if
//                     anything was freed.
//
// Both are parameterized on {T, next-pointer member, epoch member, FreeFn}.

/// Unconditional drain: detach the list and free every item.
/// Used post-fork (single-threaded) and by registry_collect_retired.
template <typename T, typename FreeFn>
void drain_retired(cpp::Atomic<uintptr_t> &list_head, T *T::*next_field,
                   FreeFn free_fn) {
  auto *head = reinterpret_cast<T *>(
      list_head.exchange(0, cpp::MemoryOrder::ACQ_REL));
  while (head) {
    T *next = head->*next_field;
    free_fn(head);
    head = next;
  }
}

/// Epoch-conditional reclaim: free items retired before safe_epoch, push
/// the rest back onto the list. Returns true if any item was freed.
template <typename T, typename FreeFn>
bool reclaim_retired(cpp::Atomic<uintptr_t> &list_head, T *T::*next_field,
                     uint64_t T::*epoch_field, uint64_t safe_epoch,
                     FreeFn free_fn) {
  auto *head = reinterpret_cast<T *>(
      list_head.exchange(0, cpp::MemoryOrder::ACQ_REL));
  T *keep_head = nullptr;
  T *keep_tail = nullptr;
  bool freed = false;
  while (head) {
    T *next = head->*next_field;
    if (head->*epoch_field < safe_epoch) {
      free_fn(head);
      freed = true;
    } else {
      head->*next_field = nullptr;
      if (keep_tail) {
        keep_tail->*next_field = head;
        keep_tail = head;
      } else {
        keep_head = keep_tail = head;
      }
    }
    head = next;
  }
  push_chain(list_head, keep_head, keep_tail, next_field);
  return freed;
}

/// Amortization threshold: attempt reclamation every N retirements.
/// Must be a power of 2 (the trigger uses a bitmask, not modulo).
constexpr uint32_t kReclaimInterval = 16;
static_assert((kReclaimInterval & (kReclaimInterval - 1)) == 0,
              "kReclaimInterval must be a power of 2");

/// Non-blocking reclamation pass. Serialized via reclaim_lock CAS — if
/// another thread is already reclaiming, the caller skips silently (the
/// winner's pass covers all pending items from all threads).
///
/// Algorithm:
///   1. Bump global_epoch so newly-retired items sort after current readers.
///   2. Scan all live slots to find min_pinned_epoch (non-blocking).
///   3. Detach each retired list, partition into free/keep, batch-push keep.
///
/// Safety invariant: an item with retired_epoch E is freed only when
/// min_pinned_epoch > E. A reader pinned at epoch E loaded global_epoch
/// as E *before* the item was deregistered (bitmap/hash cleared happened
/// before the epoch bump in step 1). A reader pinned at E+1 started
/// *after* the bump and cannot discover the deregistered item.
void registry_try_reclaim() {
  // Serialize: only one reclaim pass at a time. Losers skip — the winner
  // processes all pending items including those retired by the loser.
  uint32_t expected = 0;
  if (!state().reclaim_lock.compare_exchange_strong(
          expected, 1, cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::RELAXED))
    return;

  // Step 1: bump epoch. Items retired at the old epoch become reclaimable
  // once all readers pinned at that epoch unpin.
  state().global_epoch.fetch_add(1, cpp::MemoryOrder::ACQ_REL);

  // Step 2: non-blocking scan for the oldest active reader.
  uint64_t safe_epoch = scan_min_pinned_epoch();

  // Step 3: process each retired list.
  bool freed_any = false;

  freed_any |= reclaim_retired<TidHashTable>(
      state().retired_tables, &TidHashTable::retired_next,
      &TidHashTable::retired_epoch, safe_epoch, free_hash_table);

  freed_any |= reclaim_retired<RegistryPage>(
      state().retired_pages, &RegistryPage::retired_next,
      &RegistryPage::retired_epoch, safe_epoch,
      [](RegistryPage *p) { internal::page_free(p); });

  freed_any |= reclaim_retired<ThreadLifecycle>(
      state().retired_lifecycles, &ThreadLifecycle::retired_next,
      &ThreadLifecycle::retired_epoch, safe_epoch,
      [](ThreadLifecycle *lc) {
        robust_mutex::lifecycle_destroy(lc);
        free_lifecycle(lc);
      });

  // Sweep empty pages only if we freed something — avoids O(pages) scan
  // when the retired lists were empty or nothing was reclaimable.
  if (freed_any) {
    uint32_t page_count = state().page_count.load(cpp::MemoryOrder::ACQUIRE);
    for (uint32_t p = 0; p < page_count; ++p) {
      RegistryPage *page = load_page(p);
      if (!page)
        continue;
      uint64_t bm = page->bitmap.load(cpp::MemoryOrder::ACQUIRE);
      if (bm == 0)
        try_retire_page(p, page);
    }
  }

  state().reclaim_lock.store(0, cpp::MemoryOrder::RELEASE);
}

// --- Synchronize (blocking, fork_reinit only) ---

/// Blocking synchronize — waits until all epoch-pinned readers quiesce.
/// Used only by fork_reinit where the child process is single-threaded
/// and must free all parent-thread resources before returning.
///
/// Safe for concurrent callers: fetch_add serializes epoch bumps, each
/// caller waits for its own target epoch independently.
void registry_synchronize() {
  uint64_t target =
      state().global_epoch.fetch_add(1, cpp::MemoryOrder::ACQ_REL);

  uint32_t page_count = state().page_count.load(cpp::MemoryOrder::ACQUIRE);
  for (uint32_t p = 0; p < page_count; ++p) {
    RegistryPage *page = load_page(p);
    if (!page)
      continue;

    uint64_t bitmap = page->bitmap.load(cpp::MemoryOrder::ACQUIRE);
    bitmap &= kValidSlotMask;
    while (bitmap) {
      uint32_t s = static_cast<uint32_t>(__builtin_ctzll(bitmap));
      bitmap &= bitmap - 1;

      RegistrySlot &slot = page->slots[s];
      while (true) {
        uint64_t pinned = slot.pinned_epoch.load(cpp::MemoryOrder::ACQUIRE);
        if (pinned == 0 || pinned > target)
          break;
        for (int spin = 0; spin < 32; ++spin) {
#if defined(__x86_64__)
          __builtin_ia32_pause();
#elif defined(__aarch64__)
          __builtin_arm_yield();
#endif
        }
        ::NtYieldExecution();
      }
    }
  }

  // Drain all retired resources — single-threaded post-fork, safe to
  // free unconditionally.
  registry_collect_retired();
}

// --- Collect Retired (unconditional drain, for fork_reinit) ---

void registry_collect_retired() {
  drain_retired<TidHashTable>(state().retired_tables,
                              &TidHashTable::retired_next, free_hash_table);

  drain_retired<RegistryPage>(
      state().retired_pages, &RegistryPage::retired_next,
      [](RegistryPage *p) { internal::page_free(p); });

  drain_retired<ThreadLifecycle>(
      state().retired_lifecycles, &ThreadLifecycle::retired_next,
      [](ThreadLifecycle *lc) {
        robust_mutex::lifecycle_destroy(lc);
        free_lifecycle(lc);
      });
}

// --- Deregister and Free (non-blocking) ---

void registry_deregister_and_free(ThreadLifecycle *lc) {
  if (!lc)
    return;
  registry_deregister(lc);

  // Retire for deferred epoch-safe free. Returns immediately —
  // the lifecycle is freed later by registry_try_reclaim.
  retire_lifecycle(lc);

  // Amortized reclamation: every kReclaimInterval retirements, run
  // a non-blocking reclaim pass to free items from old epochs.
  uint32_t n = state().retire_count.fetch_add(1, cpp::MemoryOrder::RELAXED);
  if ((n & (kReclaimInterval - 1)) == 0)
    registry_try_reclaim();
}

// --- Fork Reinit ---

void registry_fork_reinit(ThreadLifecycle *self) {
  if (!self)
    return;

  uint32_t self_page = self->slot_page.load(cpp::MemoryOrder::RELAXED);
  uint32_t self_index = self->slot_index;
  uint32_t page_count = state().page_count.load(cpp::MemoryOrder::RELAXED);

  // Walk all pages, clean up non-self lifecycles, free empty pages.
  auto *arr = page_array();
  for (uint32_t p = 0; p < page_count; ++p) {
    RegistryPage *page = load_page(p);
    if (!page)
      continue;

    uint64_t bitmap = page->bitmap.load(cpp::MemoryOrder::RELAXED);
    // Normalize: kBitmapRetiring and raw bitmap both get masked to valid bits.
    // kBitmapRetiring & kValidSlotMask = all valid bits set — we'll clean them.
    bitmap &= kValidSlotMask;
    while (bitmap) {
      uint32_t idx = static_cast<uint32_t>(__builtin_ctzll(bitmap));
      bitmap &= bitmap - 1;

      if (p == self_page && idx == self_index)
        continue; // keep self

      RegistrySlot &slot = page->slots[idx];
      ThreadLifecycle *lc = slot.lifecycle.load(cpp::MemoryOrder::RELAXED);
      cleanup_stale_lifecycle(lc);
      slot.lifecycle.store(nullptr, cpp::MemoryOrder::RELAXED);
      slot.tid.store(0, cpp::MemoryOrder::RELAXED);
      slot.pin_depth = 0;
      slot.pinned_epoch.store(0, cpp::MemoryOrder::RELAXED);
    }

    if (p == self_page) {
      // Keep self's page, just clear the bitmap and re-set self's bit below.
      page->bitmap.store(0, cpp::MemoryOrder::RELAXED);
    } else {
      // Free non-self pages outright (single-threaded, safe).
      if (arr)
        arr[p].store(0, cpp::MemoryOrder::RELAXED);
      internal::page_free(page);
    }
  }

  // Re-set self's bitmap bit.
  RegistryPage *self_page_ptr = load_page(self_page);
  if (self_page_ptr)
    self_page_ptr->bitmap.store(1ULL << self_index, cpp::MemoryOrder::RELAXED);

  // Compact page_count: only self's page survives. Pages 0..self_page-1 are
  // null pointers now; pages self_page+1..old_count-1 were freed. Set
  // page_count to self_page + 1 so future scans don't walk dead indices.
  state().page_count.store(self_page + 1, cpp::MemoryOrder::RELAXED);

  // Set free_hint to 0 so the first allocation scans from the bottom,
  // reusing null slots in pages 0..self_page-1 before extending.
  state().free_hint.store(0, cpp::MemoryOrder::RELAXED);

  // Replace hash table with a fresh small one. The parent's table may be
  // vastly oversized for a single-thread child.
  TidHashTable *old_table = load_tid_table();
  TidHashTable *new_table = alloc_hash_table(kInitialHashCapacity);
  if (new_table) {
    DWORD self_tid = static_cast<DWORD>(self->tid);
    tid_hash_insert(new_table, self_tid, self_page, self_index);
    state().tid_table.store(reinterpret_cast<uintptr_t>(new_table),
                            cpp::MemoryOrder::RELAXED);
    free_hash_table(old_table);
  } else if (old_table) {
    // Allocation failed — fall back to clearing the existing table in place.
    __builtin_memset(old_table->entries, 0,
                     old_table->capacity * sizeof(cpp::Atomic<uint64_t>));
    DWORD self_tid = static_cast<DWORD>(self->tid);
    tid_hash_insert(old_table, self_tid, self_page, self_index);
  }

  // Reset epoch, counters, and locks. resize_lock may have been held by a
  // parent thread that no longer exists in the child.
  state().global_epoch.store(1, cpp::MemoryOrder::RELAXED);
  state().live_count.store(1, cpp::MemoryOrder::RELAXED);
  state().tombstone_count.store(0, cpp::MemoryOrder::RELAXED);
  state().resize_lock.store(0, cpp::MemoryOrder::RELAXED);
  state().reclaim_lock.store(0, cpp::MemoryOrder::RELAXED);
  state().retire_count.store(0, cpp::MemoryOrder::RELAXED);
  // init_state should already be 2, but reset defensively in case of
  // a fork from an unusual code path.
  state().init_state.store(2, cpp::MemoryOrder::RELAXED);

  // Free retired tables, pages, and lifecycles (single-threaded, safe).
  registry_collect_retired();
}

// --- Stats ---

uint32_t registry_live_count_impl() {
  return state().live_count.load(cpp::MemoryOrder::ACQUIRE);
}

} // namespace LIBC_NAMESPACE_DECL
