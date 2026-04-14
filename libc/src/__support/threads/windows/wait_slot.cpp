//===--- Lock-free wait slot pool implementation --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/wait_slot.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/commit_region.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace wait_slot {

// Demand-committed pool: VA reserved at init, pages committed as slots are
// needed. Each page (4KB) holds 64 slots (64 bytes each). Only the sentinel
// page is committed at startup — the rest is committed on first exhaustion.
// Capacity matches the 16-bit head index — 65K slots covers 32K+ concurrent
// threads (each needs at most 2: primary + VEH nesting). 4MB VA reservation;
// physical memory is demand-committed.
static constexpr uint32_t POOL_CAPACITY = 1u << 16;
static constexpr size_t POOL_BYTES = POOL_CAPACITY * sizeof(WaitSlot);
// Computed once at first use; commit granularity must match OS page size.
static uint32_t slots_per_page() {
  static const uint32_t val =
      static_cast<uint32_t>(windows::get_cached_page_size() / sizeof(WaitSlot));
  return val;
}

// Backing region: reserve-then-commit VA via CommitRegion.
static internal::CommitRegion pool_region;
static WaitSlot *pool;

// High-water mark: slots below this index are committed and linked.
static cpp::Atomic<uint32_t> committed_slots{0};

// Lock-free freelist: Treiber stack of available slot indices.
// Packed [gen:16 | head:16] to prevent ABA — same scheme as the futex itself.
static cpp::Atomic<uint32_t> freelist{0};

static constexpr uint32_t GEN_SHIFT = 16;
static constexpr uint32_t INDEX_MASK = (1u << GEN_SHIFT) - 1;

static uint32_t fl_head(uint32_t packed) { return packed & INDEX_MASK; }
static uint32_t fl_gen(uint32_t packed) { return packed >> GEN_SHIFT; }
static uint32_t fl_pack(uint32_t gen, uint32_t head) {
  return ((gen & 0xFFFF) << GEN_SHIFT) | (head & INDEX_MASK);
}

static DWORD tls_index = internal::TLS_OUT_OF_INDEXES;

// TLS value packs {index, generation} so we can detect stale references
// after waker reclamation. Layout: [generation:16 | index:32] in a void*.
static void *pack_tls(uint32_t index, uint16_t gen) {
  return reinterpret_cast<void *>(static_cast<uintptr_t>(index) |
                                  (static_cast<uintptr_t>(gen) << 32));
}
static uint32_t tls_index_of(void *val) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(val));
}
static uint16_t tls_gen_of(void *val) {
  return static_cast<uint16_t>(reinterpret_cast<uintptr_t>(val) >> 32);
}

// TLS cleanup callback — runs on thread exit via .CRT$XLC.
static void NTAPI slot_cleanup(void *data) {
  if (!data)
    return;
  uint32_t index = tls_index_of(data);
  uint8_t state = pool[index].state.load(cpp::MemoryOrder::ACQUIRE);
  if (state == IDLE || state == SIGNALED) {
    // IDLE: between waits — safe to free.
    // SIGNALED: waker already popped from Treiber stack — safe to free.
    free_slot(index);
  } else {
    // WAITING/IN_KERNEL/TIMED_OUT: still linked in a Treiber stack.
    // Mark dead for waker reclamation.
    pool[index].state.store(TIMED_OUT, cpp::MemoryOrder::RELEASE);
  }
}

// Commit the next page of slots and link them into the freelist.
// Returns true if new slots were made available.
static bool commit_more_slots() {
  uint32_t cur = committed_slots.load(cpp::MemoryOrder::ACQUIRE);
  if (cur >= POOL_CAPACITY)
    return false;

  uint32_t end = cur + slots_per_page();
  if (end > POOL_CAPACITY)
    end = POOL_CAPACITY;

  // CAS to claim this batch — only one thread commits a given page.
  if (!committed_slots.compare_exchange_strong(cur, end,
                                               cpp::MemoryOrder::ACQ_REL,
                                               cpp::MemoryOrder::ACQUIRE))
    return true; // another thread committed — retry alloc

  // Demand-commit via CommitRegion. Idempotent — safe if another thread
  // races (though the CAS above serializes the normal path).
  if (!pool_region.ensure_committed(end * sizeof(WaitSlot))) {
    // Roll back the watermark so the next caller can retry.
    committed_slots.compare_exchange_strong(end, cur,
                                            cpp::MemoryOrder::RELEASE,
                                            cpp::MemoryOrder::RELAXED);
    return false;
  }

  // Chain the new slots and push onto freelist.
  for (uint32_t i = cur; i < end - 1; ++i)
    pool[i].next = i + 1;
  pool[end - 1].next = NULL_INDEX;

  // Splice: make the new chain's tail point to the current freelist head,
  // then CAS the freelist head to the new chain's first slot.
  for (;;) {
    uint32_t old = freelist.load(cpp::MemoryOrder::ACQUIRE);
    pool[end - 1].next = fl_head(old);
    uint32_t desired = fl_pack(fl_gen(old) + 1, cur);
    if (freelist.compare_exchange_weak(old, desired,
                                       cpp::MemoryOrder::RELEASE,
                                       cpp::MemoryOrder::RELAXED))
      return true;
  }
}

// Pop from freelist (Treiber stack pop).
static uint32_t alloc_slot() {
  for (;;) {
    uint32_t old = freelist.load(cpp::MemoryOrder::ACQUIRE);
    uint32_t head = fl_head(old);
    if (head == NULL_INDEX) {
      // Freelist empty — try to commit more slots.
      if (!commit_more_slots())
        return NULL_INDEX; // pool fully committed and exhausted
      continue;
    }
    uint32_t next = pool[head].next;
    uint32_t desired = fl_pack(fl_gen(old) + 1, next);
    if (freelist.compare_exchange_weak(old, desired,
                                       cpp::MemoryOrder::ACQ_REL,
                                       cpp::MemoryOrder::ACQUIRE))
      return head;
  }
}

void free_slot(uint32_t index) {
  pool[index].state.store(IDLE, cpp::MemoryOrder::RELAXED);
  pool[index].thread_id = 0;
  pool[index].generation.fetch_add(1, cpp::MemoryOrder::RELEASE);
  for (;;) {
    uint32_t old = freelist.load(cpp::MemoryOrder::ACQUIRE);
    pool[index].next = fl_head(old);
    uint32_t desired = fl_pack(fl_gen(old) + 1, index);
    if (freelist.compare_exchange_weak(old, desired,
                                       cpp::MemoryOrder::RELEASE,
                                       cpp::MemoryOrder::RELAXED))
      return;
  }
}

void reclaim_slot(uint32_t index) {
  // Waker-driven reclamation: the waker popped a dead (TIMED_OUT) slot
  // from a futex chain. The owning thread has moved on (allocated a fresh
  // slot or exited). Safe to return to the freelist.
  free_slot(index);
}

uint32_t get_stale_slot(uintptr_t &wait_address_out) {
  if (tls_index == internal::TLS_OUT_OF_INDEXES)
    return NULL_INDEX;
  void *val = internal::teb_tls_get(tls_index);
  if (!val)
    return NULL_INDEX;
  uint32_t index = tls_index_of(val);
  uint16_t saved_gen = tls_gen_of(val);
  // Only reclaim if the slot is still ours (gen match) and TIMED_OUT.
  if (pool[index].generation.load(cpp::MemoryOrder::ACQUIRE) == saved_gen &&
      pool[index].state.load(cpp::MemoryOrder::ACQUIRE) == TIMED_OUT) {
    wait_address_out = pool[index].wait_address;
    return index;
  }
  return NULL_INDEX;
}

uint32_t alloc_secondary() {
  uint32_t index = alloc_slot();
  if (index == NULL_INDEX)
    return NULL_INDEX;
  pool[index].thread_id = NtCurrentThreadId();
  pool[index].generation.fetch_add(1, cpp::MemoryOrder::RELAXED);
  pool[index].state.store(IDLE, cpp::MemoryOrder::RELAXED);
  return index;
}

void release_secondary(uint32_t index) { free_slot(index); }

WaitSlot &get_slot(uint32_t index) { return pool[index]; }

uint32_t get_slot_index() {
  if (tls_index == internal::TLS_OUT_OF_INDEXES)
    return NULL_INDEX;

  // Fast path: reuse the thread's existing slot if it's not still linked
  // and hasn't been reclaimed by a waker (generation match). Direct TEB
  // lookup — single instruction, no function call.
  void *val = internal::teb_tls_get(tls_index);
  if (val) {
    uint32_t index = tls_index_of(val);
    uint16_t saved_gen = tls_gen_of(val);
    if (pool[index].state.load(cpp::MemoryOrder::ACQUIRE) == IDLE &&
        pool[index].generation.load(cpp::MemoryOrder::ACQUIRE) == saved_gen) {
      uint16_t new_gen = saved_gen + 1;
      pool[index].generation.store(new_gen, cpp::MemoryOrder::RELAXED);
      internal::teb_tls_set(tls_index, pack_tls(index, new_gen));
      return index;
    }
    // Stale (reclaimed by waker) or still linked — allocate fresh.
  }

  uint32_t index = alloc_slot();
  if (index == NULL_INDEX)
    return NULL_INDEX;

  pool[index].thread_id = NtCurrentThreadId();
  uint16_t gen =
      pool[index].generation.fetch_add(1, cpp::MemoryOrder::RELAXED) + 1;
  pool[index].state.store(IDLE, cpp::MemoryOrder::RELAXED);
  internal::teb_tls_set(tls_index, pack_tls(index, gen));
  return index;
}

void init() {
  // Reserve VA for the entire pool, commit the first page.
  (void)pool_region.init(POOL_BYTES, slots_per_page() * sizeof(WaitSlot));
  pool = pool_region.as<WaitSlot>();

  // Link slots 1..slots_per_page()-1 into the freelist (slot 0 is sentinel).
  for (uint32_t i = 1; i < slots_per_page() - 1; ++i)
    pool[i].next = i + 1;
  pool[slots_per_page() - 1].next = NULL_INDEX;
  freelist.store(fl_pack(0, 1), cpp::MemoryOrder::RELAXED);
  committed_slots.store(slots_per_page(), cpp::MemoryOrder::RELAXED);

  tls_index = internal::tls_alloc();
  if (tls_index != internal::TLS_OUT_OF_INDEXES)
    internal::tls_cleanup_register(tls_index, slot_cleanup);
}

void fini() {
  if (tls_index != internal::TLS_OUT_OF_INDEXES) {
    internal::tls_free(tls_index);
    tls_index = internal::TLS_OUT_OF_INDEXES;
  }
  pool_region.destroy();
  pool = nullptr;
}

void fork_reinit() {
  // Re-link all committed slots (except sentinel 0) into the freelist.
  uint32_t committed = committed_slots.load(cpp::MemoryOrder::RELAXED);
  for (uint32_t i = 1; i < committed; ++i) {
    pool[i].state.store(IDLE, cpp::MemoryOrder::RELAXED);
    pool[i].thread_id = 0;
    pool[i].next = (i + 1 < committed) ? i + 1 : NULL_INDEX;
  }
  freelist.store(fl_pack(0, committed > 1 ? 1 : 0),
                 cpp::MemoryOrder::RELAXED);

  // Re-allocate TLS (parent's TLS state is invalid in child).
  if (tls_index != internal::TLS_OUT_OF_INDEXES)
    internal::tls_free(tls_index);
  tls_index = internal::tls_alloc();
  if (tls_index != internal::TLS_OUT_OF_INDEXES)
    internal::tls_cleanup_register(tls_index, slot_cleanup);
}

} // namespace wait_slot
} // namespace LIBC_NAMESPACE_DECL

int LIBC_NAMESPACE::internal::wait_slot_startup_init() {
  LIBC_NAMESPACE::wait_slot::init();
  return 0;
}

void LIBC_NAMESPACE::internal::wait_slot_fork_reinit() {
  LIBC_NAMESPACE::wait_slot::fork_reinit();
}
