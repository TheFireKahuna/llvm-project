//===-- POSIX slab allocator built on SlabPool -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Three-tier allocator:
//
// Tier 1 — Thread Cache (lock-free):
//   Per-thread free lists for each size class, accessed via TEB TLS / FLS.
//   malloc/free hit this path with zero locking.
//   Freelist pointers XOR-encoded (per-slab cookie + slot address).
//
// Tier 2 — SlabPool (per-class pool):
//   64KB slabs with bump + free list allocation, guard pages, placeholder
//   lifecycle, per-slab cookies, epoch-based release, uniform hardening.
//   Cross-thread returns via encoded atomic CAS list (cache-line-split).
//
// Tier 3 — Large (direct mmap):
//   Allocations > 32KB go through mmap/munmap directly.
//
// Free-path dispatch: slab_registry.contains(ptr & ~(64K-1)) distinguishes
// slab from large allocations — unforgeable (registry is in our memory).
//
//===----------------------------------------------------------------------===//

#include "posix_alloc.h"
#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/new.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/OSUtil/windows/resource/rlimit_data_guard.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/libc_assert.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/string/memory_utils/inline_memcpy.h"
#include "src/string/memory_utils/inline_memset.h"
#include "src/sys/mman/madvise.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {

using internal::kCanaryOffset;
using internal::kSlabBytes;
using internal::SlabFreeNode;
using internal::SlabHeader;
using internal::SlabPool;
using internal::slab_registry;

// ===----------------------------------------------------------------------===//
// Constants
// ===----------------------------------------------------------------------===//

static constexpr size_t THREAD_CACHE_MAX = 64;

// ===----------------------------------------------------------------------===//
// Size Classes
// ===----------------------------------------------------------------------===//

static constexpr int NUM_CLASSES = 40;
static constexpr uint16_t SIZE_CLASSES[NUM_CLASSES] = {
    // 16–64: step 16 (alignment-constrained)
    16,    32,    48,    64,
    // 64–128: step 16 (4 per doubling begins)
    80,    96,    112,   128,
    // 128–256: step 32
    160,   192,   224,   256,
    // 256–512: step 64
    320,   384,   448,   512,
    // 512–1024: step 128
    640,   768,   896,   1024,
    // 1024–2048: step 256
    1280,  1536,  1792,  2048,
    // 2048–4096: step 512
    2560,  3072,  3584,  4096,
    // 4096–8192: step 1024
    5120,  6144,  7168,  8192,
    // 8192–16384: step 2048
    10240, 12288, 14336, 16384,
    // 16384–32768: step ~4096
    20480, 24576, 28672, 32768,
};

static constexpr uint8_t SMALL_LOOKUP[32] = {
    0,  0,  1,  1,  2,  2,  3,  3,    // 1-64
    4,  4,  5,  5,  6,  6,  7,  7,    // 65-128
    8,  8,  8,  8,  9,  9,  9,  9,    // 129-192
    10, 10, 10, 10, 11, 11, 11, 11,   // 193-256
};

static unsigned size_to_class(size_t size) {
  if (size <= 256) {
    unsigned idx = static_cast<unsigned>((size - 1) >> 3);
    return SMALL_LOOKUP[idx];
  }
  // Sizes 257–32768: 4 classes per power-of-two doubling range.
  // Compute from the MSB position + 2 fractional bits below it.
  // Branchless: one clz + shifts + mask.
  unsigned k = 63 - __builtin_clzll(size - 1);
  unsigned sub = static_cast<unsigned>(((size - 1) >> (k - 2)) & 3);
  return (k - 8) * 4 + 12 + sub;
}

// ===----------------------------------------------------------------------===//
// Large Allocation Header
// ===----------------------------------------------------------------------===//

struct LargeHeader {
  uint64_t magic;
  size_t mmap_size;
  size_t user_size;
  size_t base_offset;
};

static_assert(sizeof(LargeHeader) == 32, "LargeHeader must be 32 bytes");
static_assert(sizeof(LargeHeader) % MALLOC_ALIGN == 0);

static constexpr uint64_t LARGE_MAGIC = 0x4C52474D41500000ULL;

// ===----------------------------------------------------------------------===//
// Thread Cache
// ===----------------------------------------------------------------------===//

struct CacheBin {
  void *head;
  uint16_t count;
};

struct ThreadSlabs {
  SlabPool::ThreadSlab slabs[NUM_CLASSES];
  CacheBin bins[NUM_CLASSES];
  ThreadSlabs *tc_next;
  ThreadSlabs *tc_prev;
  DWORD owner_tid;
};

// ===----------------------------------------------------------------------===//
// Global Allocator State
// ===----------------------------------------------------------------------===//

struct AllocatorState {
  SlabPool pools[NUM_CLASSES];

  // TLS index for direct TEB access + .CRT$XLC thread-exit cleanup.
  DWORD tls_index = internal::TLS_OUT_OF_INDEXES;

  // Encryption key for large allocation headers. XOR'd with magic,
  // mmap_size, and base_offset to prevent header forgery via write
  // primitives. Independent of slab cookies.
  uintptr_t large_key;

  // SlabPool for ThreadSlabs structs. Uses thread_local for the slab
  // pointer (not FLS) — avoids recursion since FLS cleanup triggers
  // ThreadSlabs free.
  SlabPool tc_pool;

  // All-ThreadSlabs list (for fork_reinit).
  ThreadSlabs *tc_list_head;
  cpp::Atomic<int> tc_list_lock;
};

alignas(AllocatorState) static unsigned char
    alloc_state_storage[sizeof(AllocatorState)] = {};
static Futex alloc_state_init{0};

static AllocatorState &alloc_state() {
  return *reinterpret_cast<AllocatorState *>(alloc_state_storage);
}

// ===----------------------------------------------------------------------===//
// Helpers
// ===----------------------------------------------------------------------===//

static size_t round_up_page(size_t size) {
  const size_t mask = windows::get_cached_page_mask();
  return (size + mask) & ~mask;
}

// ThreadSlabs list — spinlock.
static void tc_list_lock() {
  int expected = 0;
  while (!alloc_state().tc_list_lock.compare_exchange_weak(
      expected, 1, cpp::MemoryOrder::ACQUIRE, cpp::MemoryOrder::RELAXED))
    expected = 0;
}
static void tc_list_unlock() {
  alloc_state().tc_list_lock.store(0, cpp::MemoryOrder::RELEASE);
}

static void tc_list_insert(ThreadSlabs *ts) {
  tc_list_lock();
  ts->tc_prev = nullptr;
  ts->tc_next = alloc_state().tc_list_head;
  if (alloc_state().tc_list_head)
    alloc_state().tc_list_head->tc_prev = ts;
  alloc_state().tc_list_head = ts;
  tc_list_unlock();
}

static void tc_list_remove(ThreadSlabs *ts) {
  tc_list_lock();
  if (ts->tc_prev)
    ts->tc_prev->tc_next = ts->tc_next;
  else
    alloc_state().tc_list_head = ts->tc_next;
  if (ts->tc_next)
    ts->tc_next->tc_prev = ts->tc_prev;
  tc_list_unlock();
}

// ===----------------------------------------------------------------------===//
// ThreadSlabs Allocation (SlabPool-backed, recyclable)
// ===----------------------------------------------------------------------===//

static constexpr size_t TC_SLOT_SIZE =
    (sizeof(ThreadSlabs) + 15) & ~static_cast<size_t>(15);

// Per-thread slab for tc_pool. thread_local (not FLS) avoids recursion:
// FlsCallback is what frees the ThreadSlabs, so it can't go through FLS.
static thread_local SlabPool::ThreadSlab tc_pool_slab = nullptr;

static void rollback_partial_posix_alloc_init_after_fork() {
  auto &state = alloc_state();
  if (state.tls_index != internal::TLS_OUT_OF_INDEXES) {
    internal::teb_tls_set(state.tls_index, nullptr);
    internal::tls_cleanup_unregister(state.tls_index);
    internal::tls_free(state.tls_index);
    state.tls_index = internal::TLS_OUT_OF_INDEXES;
  }
  tc_pool_slab = nullptr;
}

static ThreadSlabs *alloc_thread_slabs() {
  alloc_state().tc_pool.init(TC_SLOT_SIZE, alignof(ThreadSlabs));

  void *slot = SlabPool::alloc(tc_pool_slab);
  if (!slot)
    slot = alloc_state().tc_pool.alloc_slow(&tc_pool_slab);
  if (!slot)
    return nullptr;

  auto *ts = static_cast<ThreadSlabs *>(slot);
  for (int i = 0; i < NUM_CLASSES; i++) {
    ts->slabs[i] = nullptr;
    ts->bins[i].head = nullptr;
    ts->bins[i].count = 0;
  }
  ts->owner_tid = SlabPool::get_current_tid();
  tc_list_insert(ts);
  return ts;
}

// ===----------------------------------------------------------------------===//
// Thread Cache — Cache Bin Drain
// ===----------------------------------------------------------------------===//

// Drain slots from a thread cache bin back to their source slabs.
// Two-phase: partition by slab into per-slab chains, then batch-push
// each chain with one operation (plain splice for owner, one CAS for
// xthread). Reduces O(n) CAS to O(k) where k = distinct slabs.
static void drain_cache_bin(ThreadSlabs *ts, unsigned ci) {
  unsigned to_drain = ts->bins[ci].count / 2;
  if (to_drain == 0)
    to_drain = 1;

  // Per-slab group — typically 1-3 slabs per drain.
  struct SlabGroup {
    SlabHeader *slab;
    SlabFreeNode *head;
    SlabFreeNode *tail;
    unsigned count;
  };
  static constexpr unsigned kMaxGroups = 8;
  SlabGroup groups[kMaxGroups];
  unsigned num_groups = 0;

  // Phase 1: Detach nodes from bin, partition into per-slab chains.
  for (unsigned i = 0; i < to_drain; i++) {
    void *slot = ts->bins[ci].head;
    if (!slot)
      break;
    auto *slab = SlabPool::ptr_to_slab(slot);
    auto *node = static_cast<SlabFreeNode *>(slot);
    uintptr_t ck = slab->freelist_cookie;

    // Advance bin head (decode current link, which may cross slabs).
    ts->bins[ci].head = SlabPool::decode_free_next(
        node->next, ck, &node->next);
    ts->bins[ci].count--;

    // Find group for this slab (linear scan — branch-predictor friendly).
    unsigned g = 0;
    for (; g < num_groups; g++) {
      if (groups[g].slab == slab)
        break;
    }

    if (g == num_groups && num_groups < kMaxGroups) {
      groups[num_groups] = {slab, nullptr, nullptr, 0};
      g = num_groups++;
    } else if (g == num_groups) {
      // Overflow: fall back to per-node push.
      SlabPool::raw_free(slot);
      continue;
    }

    // Prepend to group chain (re-encode next → group head).
    node->next = SlabPool::encode_free_next(
        groups[g].head, ck, &node->next);
    if (!groups[g].tail)
      groups[g].tail = node;
    groups[g].head = node;
    groups[g].count++;
  }

  // Phase 2: Batch-push each group's chain to its slab.
  uint32_t my_tid = SlabPool::get_current_tid();
  for (unsigned g = 0; g < num_groups; g++) {
    SlabHeader *slab = groups[g].slab;
    auto *head = groups[g].head;
    auto *tail = groups[g].tail;
    uintptr_t ck = slab->freelist_cookie;

    if (slab->tid() == my_tid) {
      // Set canaries on each slot so alloc-time verification passes.
      if (slab->slot_size >= kCanaryOffset + sizeof(uintptr_t)) {
        auto *n = head;
        for (;;) {
          *reinterpret_cast<uintptr_t *>(
              reinterpret_cast<char *>(n) + kCanaryOffset) =
              SlabPool::make_canary(slab, n);
          if (n == tail)
            break;
          n = SlabPool::decode_free_next(n->next, ck, &n->next);
        }
      }
      // Owner: splice into local_free (zero atomics).
      tail->next = SlabPool::encode_free_next(
          slab->local_free, ck, &tail->next);
      slab->local_free = head;
      slab->returned += groups[g].count;
      // Return physical pages if slab is now fully empty.
      if (slab->returned == slab->bump && ts->slabs[ci] != slab)
        slab->pool->release_if_empty(slab);
    } else {
      // Non-owner: one CAS for the entire chain.
      SlabPool::push_chain_xthread(slab, head, tail);
    }
  }
}

// Flush all cache bins back to their respective slabs.
static void flush_cache_bins(ThreadSlabs *ts) {
  for (int i = 0; i < NUM_CLASSES; i++) {
    void *p = ts->bins[i].head;
    while (p) {
      auto *slab = SlabPool::ptr_to_slab(p);
      auto *node = static_cast<SlabFreeNode *>(p);
      void *next = SlabPool::decode_free_next(
          node->next, slab->freelist_cookie, &node->next);
      SlabPool::raw_free(p);
      p = next;
    }
    ts->bins[i].head = nullptr;
    ts->bins[i].count = 0;
  }
}

// ===----------------------------------------------------------------------===//
// Thread Cache — TLS Management
// ===----------------------------------------------------------------------===//

static void NTAPI thread_slabs_cleanup(PVOID data) {
  if (!data)
    return;

  auto *ts = static_cast<ThreadSlabs *>(data);
  tc_list_remove(ts);

  // Flush all bins back to their respective slabs.
  flush_cache_bins(ts);

  // Abandon all thread-owned slabs.
  for (int i = 0; i < NUM_CLASSES; i++) {
    if (ts->slabs[i]) {
      alloc_state().pools[i].abandon(ts->slabs[i]);
      ts->slabs[i] = nullptr;
    }
  }

  // Return the ThreadSlabs struct to the pool.
  SlabPool::free(ts);

  // Abandon the tc_pool slab if this thread owned it.
  if (tc_pool_slab)
    alloc_state().tc_pool.abandon(tc_pool_slab);
  tc_pool_slab = nullptr;
}

static ThreadSlabs *get_thread_slabs() {
  if (alloc_state_init.load(cpp::MemoryOrder::ACQUIRE) < 2)
    posix_alloc_init();

  // Hot path: direct TEB read — single instruction.
  if (alloc_state().tls_index != internal::TLS_OUT_OF_INDEXES) {
    auto *ts = static_cast<ThreadSlabs *>(
        internal::teb_tls_get(alloc_state().tls_index));
    if (ts)
      return ts;
  }

  ThreadSlabs *ts = alloc_thread_slabs();
  if (ts && alloc_state().tls_index != internal::TLS_OUT_OF_INDEXES)
    internal::teb_tls_set(alloc_state().tls_index, ts);
  return ts;
}

// ===----------------------------------------------------------------------===//
// Slow Path — Get Slab and Batch Refill
// ===----------------------------------------------------------------------===//

static void *slow_alloc(unsigned ci, ThreadSlabs *ts) {
  // Get a slab for this class (may adopt abandoned, recycle, or alloc fresh).
  SlabPool::ThreadSlab *slab_ptr = ts ? &ts->slabs[ci] : nullptr;
  SlabPool::ThreadSlab temp_slab = nullptr;
  if (!slab_ptr)
    slab_ptr = &temp_slab;

  void *result = alloc_state().pools[ci].alloc_slow(slab_ptr);
  if (!result) {
    libc_errno = ENOMEM;
    return nullptr;
  }

  // Batch refill: pull additional slots into a stack array, shuffle,
  // then link into the bin. Shuffling randomizes reuse order so an
  // attacker can't predict which freed slot a subsequent malloc returns.
  if (ts) {
    constexpr unsigned kBatch = THREAD_CACHE_MAX / 2;
    void *buf[kBatch];
    unsigned n = 0;
    SlabPool::ThreadSlab slab = *slab_ptr;
    while (n < kBatch && slab) {
      void *ptr = SlabPool::alloc(slab);
      if (!ptr)
        break;
      buf[n++] = ptr;
    }

    // Fisher-Yates shuffle with OS CSPRNG entropy.
    // Uses Lemire's nearly divisionless method: (uint64_t)r * range >> 32
    // maps a 32-bit random value to [0, range) with negligible bias.
    if (n > 1) {
      uint32_t rng[kBatch];
      ::ProcessPrng(reinterpret_cast<unsigned char *>(rng),
                    (n - 1) * sizeof(uint32_t));
      for (unsigned i = n - 1; i > 0; i--) {
        unsigned j = static_cast<unsigned>(
            (static_cast<uint64_t>(rng[i - 1]) * (i + 1)) >> 32);
        void *tmp = buf[i];
        buf[i] = buf[j];
        buf[j] = tmp;
      }
    }

    // Link shuffled slots into the bin with canaries.
    // Bump-allocated slots are zero-filled; the canary must be written
    // here so that the thread-cache pop path can verify it.
    for (unsigned i = 0; i < n; i++) {
      auto *node = static_cast<SlabFreeNode *>(buf[i]);
      auto *hdr = SlabPool::ptr_to_slab(buf[i]);
      node->next = SlabPool::encode_free_next(
          static_cast<SlabFreeNode *>(ts->bins[ci].head),
          hdr->freelist_cookie, &node->next);
      if (hdr->slot_size >= kCanaryOffset + sizeof(uintptr_t)) {
        *reinterpret_cast<uintptr_t *>(
            static_cast<char *>(buf[i]) + kCanaryOffset) =
            SlabPool::make_canary(hdr, buf[i]);
      }
      ts->bins[ci].head = buf[i];
      ts->bins[ci].count++;
    }
  }

  return result;
}

// ===----------------------------------------------------------------------===//
// Large Allocation — Encrypted Header Helpers
// ===----------------------------------------------------------------------===//

static void write_large_header(LargeHeader *hdr, size_t mmap_size,
                                size_t user_size, size_t base_offset) {
  uintptr_t k = alloc_state().large_key;
  hdr->magic = LARGE_MAGIC ^ k;
  hdr->mmap_size = mmap_size ^ k;
  hdr->user_size = user_size;
  hdr->base_offset = base_offset ^ k;
}

// Decode and validate a large header. Traps on any inconsistency.
static void read_large_header(void *ptr, void *&base, size_t &total,
                               size_t &user_size) {
  auto *hdr = reinterpret_cast<LargeHeader *>(
      static_cast<char *>(ptr) - sizeof(LargeHeader));
  uintptr_t k = alloc_state().large_key;

  if ((hdr->magic ^ k) != LARGE_MAGIC)
    __builtin_trap(); // Corrupt or forged header.

  total = hdr->mmap_size ^ k;
  size_t offset = hdr->base_offset ^ k;
  user_size = hdr->user_size;

  // Plausibility: page-aligned total, sane minimum, offset within mapping.
  const size_t pmask = windows::get_cached_page_mask();
  if ((total & pmask) != 0 || total < sizeof(LargeHeader) + windows::get_cached_page_size())
    __builtin_trap();
  if (offset >= total)
    __builtin_trap();
  base = reinterpret_cast<char *>(hdr) - offset;
  if ((reinterpret_cast<uintptr_t>(base) & pmask) != 0)
    __builtin_trap(); // Base must be page-aligned.
}

// ===----------------------------------------------------------------------===//
// Large Allocation Path
// ===----------------------------------------------------------------------===//

static void *large_alloc(size_t size) {
  if (size > SIZE_MAX - sizeof(LargeHeader) - windows::get_cached_page_mask()) {
    libc_errno = ENOMEM;
    return nullptr;
  }
  size_t total = round_up_page(size + sizeof(LargeHeader));
  void *base = LIBC_NAMESPACE::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    libc_errno = ENOMEM;
    return nullptr;
  }

  auto *hdr = static_cast<LargeHeader *>(base);
  write_large_header(hdr, total, size, 0);
  return reinterpret_cast<char *>(base) + sizeof(LargeHeader);
}

static void *large_alloc_aligned(size_t size, size_t alignment) {
  if (size > SIZE_MAX - sizeof(LargeHeader) - alignment - windows::get_cached_page_mask()) {
    libc_errno = ENOMEM;
    return nullptr;
  }
  size_t total = round_up_page(size + sizeof(LargeHeader) + alignment);
  void *base = LIBC_NAMESPACE::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    libc_errno = ENOMEM;
    return nullptr;
  }

  uintptr_t raw = reinterpret_cast<uintptr_t>(base) + sizeof(LargeHeader);
  uintptr_t user = (raw + alignment - 1) & ~(alignment - 1);

  auto *hdr =
      reinterpret_cast<LargeHeader *>(user - sizeof(LargeHeader));
  size_t offset = static_cast<size_t>(reinterpret_cast<char *>(hdr) -
                                       static_cast<char *>(base));
  write_large_header(hdr, total, size, offset);
  return reinterpret_cast<void *>(user);
}

static void large_free(void *ptr) {
  void *base;
  size_t total, user_size;
  read_large_header(ptr, base, total, user_size);
  LIBC_NAMESPACE::munmap(base, total);
}

static size_t large_usable_size(void *ptr) {
  void *base;
  size_t total, user_size;
  read_large_header(ptr, base, total, user_size);
  return user_size;
}

static void *large_realloc(void *ptr, size_t new_size) {
  void *base;
  size_t total, old_size;
  read_large_header(ptr, base, total, old_size);

  auto *hdr = reinterpret_cast<LargeHeader *>(
      static_cast<char *>(ptr) - sizeof(LargeHeader));
  uintptr_t k = alloc_state().large_key;
  size_t offset = hdr->base_offset ^ k;

  // Shrink: release tail VA entirely (not just decommit).
  if (new_size <= old_size && new_size > LARGE_THRESHOLD) {
    size_t new_total = round_up_page(new_size + sizeof(LargeHeader) + offset);
    if (new_total < total) {
      char *tail = static_cast<char *>(base) + new_total;
      size_t tail_size = total - new_total;
      LIBC_NAMESPACE::munmap(tail, tail_size);
      // Update encrypted total so large_free releases the correct range.
      hdr->mmap_size = new_total ^ k;
    }
    hdr->user_size = new_size; // plaintext field.
    return ptr;
  }

  // Grow: alloc-copy-free.
  void *new_ptr = (new_size > LARGE_THRESHOLD) ? large_alloc(new_size)
                                                : posix_alloc(new_size);
  if (!new_ptr)
    return nullptr;
  size_t copy_size = (old_size < new_size) ? old_size : new_size;
  inline_memcpy(new_ptr, ptr, copy_size);
  large_free(ptr);
  return new_ptr;
}

// ===----------------------------------------------------------------------===//
// Slab Free Path
// ===----------------------------------------------------------------------===//

static void slab_free(void *ptr) {
  auto *slab = SlabPool::ptr_to_slab(ptr);

  // Harden: canary check + zero + canary place.
  SlabPool::harden_freed_slot(ptr, slab);

  // Try thread cache push.
  ThreadSlabs *ts = nullptr;
  if (alloc_state().tls_index != internal::TLS_OUT_OF_INDEXES)
    ts = static_cast<ThreadSlabs *>(
        internal::teb_tls_get(alloc_state().tls_index));

  if (ts) {
    unsigned ci = slab->class_index;
    uintptr_t ck = slab->freelist_cookie;
    auto *node = static_cast<SlabFreeNode *>(ptr);
    node->next = SlabPool::encode_free_next(
        static_cast<SlabFreeNode *>(ts->bins[ci].head), ck, &node->next);
    ts->bins[ci].head = ptr;
    ts->bins[ci].count++;
    if (ts->bins[ci].count > THREAD_CACHE_MAX)
      drain_cache_bin(ts, ci);
    return;
  }

  // No thread cache — direct to slab.
  SlabPool::raw_free(ptr);
}

// ===----------------------------------------------------------------------===//
// Public API
// ===----------------------------------------------------------------------===//

void posix_alloc_init() {
  for (;;) {
    FutexValueType state = alloc_state_init.load(cpp::MemoryOrder::ACQUIRE);
    if (state == 2)
      return;

    if (state == 0) {
      FutexValueType expected = 0;
      if (!alloc_state_init.compare_exchange_strong(
              expected, 1, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::RELAXED))
        continue;
      break;
    }

    // Another thread is initializing — wait-slot futex wait until done.
    // Wait slots init from .CRT$XIB (before any user code), so the
    // Futex infrastructure is guaranteed available here.
    alloc_state_init.wait(1);
  }

  ::new (alloc_state_storage) AllocatorState{};

  // Initialize per-class pools.
  for (int i = 0; i < NUM_CLASSES; i++)
    alloc_state().pools[i].init(SIZE_CLASSES[i], MALLOC_ALIGN,
                                static_cast<uint8_t>(i));

  alloc_state().tc_pool.init(TC_SLOT_SIZE, alignof(ThreadSlabs));
  alloc_state().tc_list_head = nullptr;
  alloc_state().tc_list_lock.store(0, cpp::MemoryOrder::RELAXED);

  // Seed large-header encryption key (independent of slab cookies).
  ::ProcessPrng(reinterpret_cast<unsigned char *>(&alloc_state().large_key),
                sizeof(alloc_state().large_key));
  if (alloc_state().large_key == 0)
    alloc_state().large_key = 0xA5A5'A5A5'5A5A'5A5A;

  // Allocate TLS for direct TEB access + .CRT$XLC thread-exit cleanup.
  alloc_state().tls_index = internal::tls_alloc();
  if (alloc_state().tls_index != internal::TLS_OUT_OF_INDEXES)
    internal::tls_cleanup_register(alloc_state().tls_index,
                                   thread_slabs_cleanup);

  alloc_state_init.store_and_notify_all(2);
}

void *posix_alloc(size_t size) {
  windows::ScopedRlimitDataPublicCall rlimit_scope;

  if (alloc_state_init.load(cpp::MemoryOrder::ACQUIRE) < 2)
    posix_alloc_init();

  // C17: malloc(0) returns a unique freeable pointer.
  if (size == 0)
    size = 1;

  if (!windows::allows_public_rlimit_data_growth(size)) {
    libc_errno = ENOMEM;
    return nullptr;
  }

  if (size > LARGE_THRESHOLD)
    return large_alloc(size);

  unsigned ci = size_to_class(size);
  ThreadSlabs *ts = get_thread_slabs();

  // Fast path 1: pop from thread cache bin.
  if (ts && ts->bins[ci].head) {
    void *ptr = ts->bins[ci].head;
    auto *slab = SlabPool::ptr_to_slab(ptr);
    auto *node = static_cast<SlabFreeNode *>(ptr);
    ts->bins[ci].head = SlabPool::decode_free_next(
        node->next, slab->freelist_cookie, &node->next);
    ts->bins[ci].count--;
    // Verify then clear canary. Mismatch = UAF write between free and alloc.
    if (slab->slot_size >= kCanaryOffset + sizeof(uintptr_t)) {
      auto *canary_ptr = reinterpret_cast<uintptr_t *>(
          static_cast<char *>(ptr) + kCanaryOffset);
      if (*canary_ptr != SlabPool::make_canary(slab, ptr))
        __builtin_trap();
      *canary_ptr = 0;
    }
    return ptr;
  }

  // Fast path 2: alloc from thread-owned slab (no lock).
  if (ts) {
    void *ptr = SlabPool::alloc(ts->slabs[ci]);
    if (ptr)
      return ptr;
  }

  // Slow path: claim a slab and refill.
  return slow_alloc(ci, ts);
}

void *posix_alloc_zeroed(size_t size) {
  if (size == 0)
    size = 1;

  // Large allocations: mmap guarantees zero-fill.
  if (size > LARGE_THRESHOLD)
    return large_alloc(size);

  void *ptr = posix_alloc(size);
  if (!ptr)
    return nullptr;

  // Zero-on-free guarantees offset 8+ is already zero. The canary at
  // offset 8 was cleared by alloc. Only the stale freelist pointer at
  // offset 0-7 may be non-zero (bump-allocated and recommitted-page
  // slots are fully zero, but one store is cheaper than checking).
  *reinterpret_cast<uint64_t *>(ptr) = 0;
  return ptr;
}

void *posix_alloc_aligned(size_t size, size_t alignment) {
  windows::ScopedRlimitDataPublicCall rlimit_scope;

  if (alignment <= MALLOC_ALIGN)
    return posix_alloc(size);

  if (size == 0)
    size = 1;

  size_t public_request = size > alignment ? size : alignment;
  if (!windows::allows_public_rlimit_data_growth(public_request)) {
    libc_errno = ENOMEM;
    return nullptr;
  }

  if (size > LARGE_THRESHOLD || alignment > 64 * 1024)
    return large_alloc_aligned(size, alignment);

  // For slab allocations: use a class whose slot_size >= alignment and >= size.
  size_t target = public_request;
  unsigned ci = size_to_class(target);
  uint16_t slot_sz = SIZE_CLASSES[ci];

  // Verify slot size satisfies alignment.
  if ((slot_sz & (alignment - 1)) == 0) {
    ThreadSlabs *ts = get_thread_slabs();
    if (ts && ts->bins[ci].head) {
      void *ptr = ts->bins[ci].head;
      auto *slab = SlabPool::ptr_to_slab(ptr);
      auto *node = static_cast<SlabFreeNode *>(ptr);
      ts->bins[ci].head = SlabPool::decode_free_next(
          node->next, slab->freelist_cookie, &node->next);
      ts->bins[ci].count--;
      // Verify then clear canary. Mismatch = UAF write between free and alloc.
      if (slab->slot_size >= kCanaryOffset + sizeof(uintptr_t)) {
        auto *canary_ptr = reinterpret_cast<uintptr_t *>(
            static_cast<char *>(ptr) + kCanaryOffset);
        if (*canary_ptr != SlabPool::make_canary(slab, ptr))
          __builtin_trap();
        *canary_ptr = 0;
      }
      return ptr;
    }
    return slow_alloc(ci, ts);
  }

  // Bump to next power-of-two class.
  target = alignment;
  while (target < size)
    target *= 2;
  if (target <= LARGE_THRESHOLD) {
    ci = size_to_class(target);
    ThreadSlabs *ts = get_thread_slabs();
    if (ts && ts->bins[ci].head) {
      void *ptr = ts->bins[ci].head;
      auto *slab = SlabPool::ptr_to_slab(ptr);
      auto *node = static_cast<SlabFreeNode *>(ptr);
      ts->bins[ci].head = SlabPool::decode_free_next(
          node->next, slab->freelist_cookie, &node->next);
      ts->bins[ci].count--;
      // Verify then clear canary. Mismatch = UAF write between free and alloc.
      if (slab->slot_size >= kCanaryOffset + sizeof(uintptr_t)) {
        auto *canary_ptr = reinterpret_cast<uintptr_t *>(
            static_cast<char *>(ptr) + kCanaryOffset);
        if (*canary_ptr != SlabPool::make_canary(slab, ptr))
          __builtin_trap();
        *canary_ptr = 0;
      }
      return ptr;
    }
    return slow_alloc(ci, ts);
  }

  return large_alloc_aligned(size, alignment);
}

void posix_free(void *ptr) {
  if (!ptr)
    return;

  // Registry-based dispatch: check if the 64KB-aligned base is a
  // registered slab. Unforgeable — the registry is in our memory.
  uintptr_t base =
      reinterpret_cast<uintptr_t>(ptr) & ~static_cast<uintptr_t>(kSlabBytes - 1);
  if (slab_registry.contains(base)) {
    slab_free(ptr);
    return;
  }

  large_free(ptr);
}

void *posix_realloc(void *ptr, size_t size) {
  if (!ptr)
    return posix_alloc(size);

  // C23 7.24.3.7: realloc(ptr, 0) frees and returns NULL.
  if (size == 0) {
    posix_free(ptr);
    return nullptr;
  }

  // Determine slab vs large via registry.
  uintptr_t base =
      reinterpret_cast<uintptr_t>(ptr) & ~static_cast<uintptr_t>(kSlabBytes - 1);
  if (!slab_registry.contains(base))
    return large_realloc(ptr, size);

  // Slab realloc.
  auto *slab = SlabPool::ptr_to_slab(ptr);
  size_t old_usable = slab->slot_size;

  // Same size class — no-op.
  if (size <= old_usable) {
    unsigned new_ci = size_to_class(size);
    if (SIZE_CLASSES[new_ci] == slab->slot_size)
      return ptr;
  }

  // Alloc-copy-free.
  void *new_ptr = posix_alloc(size);
  if (!new_ptr)
    return nullptr;
  size_t copy_size = (old_usable < size) ? old_usable : size;
  inline_memcpy(new_ptr, ptr, copy_size);
  posix_free(ptr);
  return new_ptr;
}

size_t posix_usable_size(void *ptr) {
  if (!ptr)
    return 0;

  uintptr_t base =
      reinterpret_cast<uintptr_t>(ptr) & ~static_cast<uintptr_t>(kSlabBytes - 1);
  if (slab_registry.contains(base)) {
    auto *slab = SlabPool::ptr_to_slab(ptr);
    return slab->slot_size;
  }

  return large_usable_size(ptr);
}

// Fork reinit: reset locks, drain dead threads, release empty slabs.
void posix_alloc_fork_reinit() {
  FutexValueType init_state = alloc_state_init.load(cpp::MemoryOrder::ACQUIRE);
  if (init_state == 0)
    return;

  if (init_state == 1) {
    rollback_partial_posix_alloc_init_after_fork();
    alloc_state_init.store(0, cpp::MemoryOrder::RELAXED);
    return;
  }

  // 1. Reset locks.
  alloc_state().tc_list_lock.store(0, cpp::MemoryOrder::RELAXED);

  // 2. Flush dead thread caches.
  DWORD my_tid = SlabPool::get_current_tid();
  ThreadSlabs *ts = alloc_state().tc_list_head;
  while (ts) {
    ThreadSlabs *next = ts->tc_next;
    if (ts->owner_tid != my_tid) {
      flush_cache_bins(ts);
      for (int i = 0; i < NUM_CLASSES; i++)
        ts->slabs[i] = nullptr;
      // Unlink (single-threaded).
      if (ts->tc_prev)
        ts->tc_prev->tc_next = ts->tc_next;
      else
        alloc_state().tc_list_head = ts->tc_next;
      if (ts->tc_next)
        ts->tc_next->tc_prev = ts->tc_prev;
      SlabPool::free(ts);
    }
    ts = next;
  }

  // 3. Reset each pool (drains dead threads' slabs, releases empties).
  for (int i = 0; i < NUM_CLASSES; i++)
    alloc_state().pools[i].fork_reinit();
  alloc_state().tc_pool.fork_reinit();
}

// Exec reinit: the same process continues with the same c.dll, so all
// SlabPool VA is still live. But the surviving thread's ThreadSlabs and
// per-class slab pointers may reference slabs that fork_reinit() will
// body-release (dead-thread slabs → full_release → placeholder_preserve →
// PAGE_NOACCESS). Unlike fork (COW copy, all pages valid), exec must
// treat ALL threads as dead — the new image will allocate fresh
// ThreadSlabs on its first malloc call.
void posix_alloc_exec_reinit() {
  FutexValueType init_state = alloc_state_init.load(cpp::MemoryOrder::ACQUIRE);
  if (init_state == 0)
    return;

  if (init_state == 1) {
    rollback_partial_posix_alloc_init_after_fork();
    alloc_state_init.store(0, cpp::MemoryOrder::RELAXED);
    return;
  }

  // 1. Reset the tc_pool_slab thread_local BEFORE tc_pool.fork_reinit()
  //    can body-release the slab it points to.
  tc_pool_slab = nullptr;

  // 2. Clear the TEB TLS slot so get_thread_slabs() allocates fresh
  //    ThreadSlabs on the first post-exec malloc call.
  if (alloc_state().tls_index != internal::TLS_OUT_OF_INDEXES)
    internal::teb_tls_set(alloc_state().tls_index, nullptr);

  // 3. Reset locks (single-threaded after quiesce).
  alloc_state().tc_list_lock.store(0, cpp::MemoryOrder::RELAXED);

  // 4. Walk tc_list: flush ALL threads' bins (including current thread),
  //    null all slab pointers, free all ThreadSlabs structs. The new
  //    image gets a completely clean allocator thread cache.
  ThreadSlabs *ts = alloc_state().tc_list_head;
  while (ts) {
    ThreadSlabs *next = ts->tc_next;
    flush_cache_bins(ts);
    for (int i = 0; i < NUM_CLASSES; i++)
      ts->slabs[i] = nullptr;
    SlabPool::free(ts);
    ts = next;
  }
  alloc_state().tc_list_head = nullptr;

  // 5. Reset each pool — drains dead threads' slabs, releases empties.
  for (int i = 0; i < NUM_CLASSES; i++)
    alloc_state().pools[i].fork_reinit();
  alloc_state().tc_pool.fork_reinit();
}

} // namespace LIBC_NAMESPACE_DECL

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

void LIBC_NAMESPACE::internal::alloc_fork_reinit() {
  LIBC_NAMESPACE::posix_alloc_fork_reinit();
}

void LIBC_NAMESPACE::internal::alloc_exec_reinit() {
  LIBC_NAMESPACE::posix_alloc_exec_reinit();
}
