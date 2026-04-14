//===-- Per-thread demand-commit scratch allocator ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thread-local bump allocator with demand-commit, canary validation,
// high-watermark decommit, and zero-on-release. Designed for short-lived
// temporary buffers that must not live on the stack (worker threads have
// small initial commits). Used for path conversion, RegionWalker iteration,
// reparse data, NT info struct queries, topology enumeration, and any other
// thread-local scratch need.
//
// Memory geometry (one 64 KB reservation per thread):
//
//   Page 0  [0x0000-0x0FFF]  Control block (ThreadScratchState)  committed
//   Page 1  [0x1000-0x1FFF]  Leading guard                       never committed
//   Page 2  [0x2000-0x2FFF]  Data page 0                         committed
//   Page 3  [0x3000-0x3FFF]  Data page 1                         committed
//   Pages 4..14              Data growth                          demand-commit
//   Page 15 [0xF000-0xFFFF]  Trailing guard                      never committed
//
// The control block on page 0 is isolated from data by the leading guard
// page. An underflow from data faults on the guard; an overflow past
// the data region faults on the trailing guard. Neither direction can
// reach the control block. Strictly stronger than SlabPool's single
// leading guard (which shares the header/slot boundary).
//
// Security properties:
//   - No inline metadata: bump pointer + watermark in control page, never
//     in the data region. Strictly stronger than XOR-encoded freelists.
//   - Canary between allocations: 8-byte value derived from per-thread
//     CSPRNG seed XOR allocation address, plus ~canary complement.
//     Checked on release; mismatch traps immediately.
//   - Zero-on-release: user data volatile-zeroed before bump rewind.
//     Prevents stale paths (usernames, temp paths) from leaking.
//   - Bump start randomization: CSPRNG-seeded offset within initial
//     data page, aligned to ALLOC_ALIGN. Resists heap spray.
//   - Leading + trailing guard pages: MMU-enforced, zero runtime cost.
//   - Uncommitted fault zone: pages between commit watermark and trailing
//     guard fault on any access — sliding defense zone.
//   - High-watermark decommit: demand-committed pages are returned to the
//     OS when the bump pointer rewinds past them (physical memory freed,
//     VA preserved, UAF protection via PAGE_NOACCESS).
//   - Thread-exit VA release: registered via tls_cleanup, entire 64 KB
//     reservation freed on thread detach.
//   - Fork safety: global registry enables dead-thread VA reclamation.
//     Locks reset. Surviving thread state preserved.
//   - Pointer range validation on release: traps on wild pointers.
//   - Runtime page size validation: traps if OS page size != 4096.
//
// Performance:
//   - Hot path: one compare + pointer add + canary write. No syscall,
//     no atomic, no lock. Single-owner thread-local — no contention.
//   - Demand-commit cold path: one NtAllocateVirtualMemory(MEM_COMMIT)
//     per new page. Committed pages stay committed until release.
//   - High-watermark decommit: only fires when demand-committed pages
//     exist and bump rewinds below them. Common single-buffer case
//     (within initial 8KB) never triggers decommit.
//   - LIFO discipline: RAII ScratchBuf destructor rewinds bump pointer.
//     Signal handlers / APCs nest correctly (bump past interrupted region).
//
// Depends only on page_alloc.h, page_size.h, teb_tls.h, tls_cleanup.h,
// bcryptprimitives.h. No libc, no mmap, no malloc.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_THREAD_SCRATCH_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_THREAD_SCRATCH_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/threads/windows/thread_local_word.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// ---------------------------------------------------------------------------
// Geometry constants
// ---------------------------------------------------------------------------

namespace scratch_detail {

// Windows allocation granularity — minimum NtAllocateVirtualMemory reserve.
inline constexpr size_t ALLOC_GRANULARITY = 65536;
inline constexpr size_t PAGE_SIZE = 4096;

// Total VA reservation per thread. One allocation granularity unit (64 KB).
inline constexpr size_t RESERVE_SIZE = ALLOC_GRANULARITY;

// Page layout:
//   Page 0:    Control block (committed)
//   Page 1:    Leading guard (never committed — isolates control from data)
//   Pages 2-14: Data (13 pages, 53248 bytes, demand-commit)
//   Page 15:   Trailing guard (never committed — catches forward overflow)
inline constexpr size_t CONTROL_OFFSET = 0;
inline constexpr size_t LEADING_GUARD_OFFSET = PAGE_SIZE;
inline constexpr size_t DATA_OFFSET = 2 * PAGE_SIZE;
inline constexpr size_t TRAILING_GUARD_OFFSET = RESERVE_SIZE - PAGE_SIZE;

// Usable data region: pages 2 through 14.
inline constexpr size_t DATA_CAPACITY = TRAILING_GUARD_OFFSET - DATA_OFFSET;
static_assert(DATA_CAPACITY == 13 * PAGE_SIZE,
              "Data region should be 13 pages (53248 bytes)");

// Initial commit: control page + 2 data pages = 12 KB physical.
// Provides 8 KB of data — enough for one full PATH_MAX WCHAR buffer
// (4096 WCHARs = 8192 bytes) plus canary + randomization overhead.
// Pages committed on creation: 0 (control), 2, 3 (first two data pages).
// Pages 1 and 15 are NEVER committed (guard pages).
inline constexpr size_t INITIAL_DATA_COMMIT = 2 * PAGE_SIZE; // pages 2, 3

// Canary overhead per allocation: 16 bytes (8-byte canary + 8-byte complement).
// Keeps user data 16-byte aligned when the allocation base is 16-byte aligned.
inline constexpr size_t CANARY_OVERHEAD = 16;

// Alignment for all allocations. Sufficient for WCHAR, SSE, and general use.
inline constexpr size_t ALLOC_ALIGN = 16;

// Maximum randomization offset within the first committed data page.
// Uses at most 25% of the first page (1024 bytes / ALLOC_ALIGN = 64 positions).
// This limits waste while providing meaningful spray resistance.
inline constexpr size_t MAX_RANDOM_OFFSET = PAGE_SIZE / 4;
inline constexpr size_t RANDOM_POSITIONS = MAX_RANDOM_OFFSET / ALLOC_ALIGN;

} // namespace scratch_detail

// ---------------------------------------------------------------------------
// Pending-flag bits for cross-thread signaling via ThreadLocalWord
// ---------------------------------------------------------------------------
//
// Cross-thread writers set bits via ThreadLocalWord::signal_or().
// The owner checks any_pending() on the alloc hot path — one RELAXED
// load + TEST + predicted-not-taken Jcc. Zero cost when no flags are set.

namespace scratch_flags {

// Memory pressure: decommit demand-committed pages above the initial level.
// Set by a global memory pressure detector or explicit cross-thread request.
// Cleared by the owner after decommitting.
inline constexpr uint32_t DECOMMIT = 1u << 0;

} // namespace scratch_flags

// ---------------------------------------------------------------------------
// ThreadScratchState — per-thread control block, lives on page 0
// ---------------------------------------------------------------------------
//
// Two cache lines. Line 0: hot-path allocator state (bump, limits, canary).
// Line 1: ThreadLocalWord for cross-thread signaling (memory pressure,
// decommit requests). Separated from data by the leading guard page.
// Forward overflow from data hits the trailing guard; backward underflow
// from data hits the leading guard. Neither direction can reach this
// control block.

struct ThreadScratchState {
  // -- Cache line 0: Hot path (alloc/release) --
  char *data_base;        // Start of data region (page 2).
  char *bump;             // Current allocation frontier within data region.
  char *commit_limit;     // End of committed data pages.
  char *data_limit;       // End of data region (start of trailing guard page).
  uintptr_t canary_seed;  // Per-thread CSPRNG seed for canary derivation.
  char *reserve_base;     // Start of entire VA reservation (page 0 = this).

  // -- Cold fields: registry and fork --
  ThreadScratchState *next; // Global registry linked-list link.
  uint32_t owner_tid;       // Thread ID for fork dead-thread identification.
  uint32_t pad_;            // Pad to cache-line boundary.

  // -- Cache line 1: Cross-thread signaling --
  // ThreadLocalWord for pending-flag notification. Cross-thread writers
  // set scratch_flags bits via ThreadLocalWord::signal_or(). The owner
  // checks word.any_pending() on the alloc hot path — one RELAXED load
  // + TEST + predicted-not-taken Jcc per allocation. Zero cost when clear.
  ThreadLocalWord word;
};

static_assert(sizeof(ThreadScratchState) == 128,
              "ThreadScratchState must be two cache lines (128 bytes)");
static_assert(sizeof(ThreadScratchState) <= scratch_detail::PAGE_SIZE,
              "ThreadScratchState must fit in control page");
static_assert(__builtin_offsetof(ThreadScratchState, word) == 64,
              "ThreadLocalWord must be on cache line 1");

// ---------------------------------------------------------------------------
// Canary helpers
// ---------------------------------------------------------------------------
//
// Each allocation is prefixed with a 16-byte canary block:
//   [0..8)  canary     = seed ^ address
//   [8..16) complement = ~canary
//
// The complement doubles validation strength: a single-bit corruption in
// the canary region is detected by either the canary or its complement.
// The address dependency prevents relocation and replay attacks.
// Matches the SlabPool pattern (canary_key ^ slot_address) but adds the
// complement for strictly stronger corruption detection.

namespace scratch_detail {

LIBC_INLINE uintptr_t make_canary(uintptr_t seed, const void *addr) {
  return seed ^ reinterpret_cast<uintptr_t>(addr);
}

LIBC_INLINE void write_canary(uintptr_t seed, char *alloc_base) {
  uintptr_t c = make_canary(seed, alloc_base);
  auto *slot = reinterpret_cast<uintptr_t *>(alloc_base);
  slot[0] = c;
  slot[1] = ~c;
}

LIBC_INLINE void verify_canary(uintptr_t seed, char *alloc_base) {
  uintptr_t expected = make_canary(seed, alloc_base);
  auto *slot = reinterpret_cast<const uintptr_t *>(alloc_base);
  if (LIBC_UNLIKELY(slot[0] != expected || slot[1] != ~expected))
    __builtin_trap();
}

} // namespace scratch_detail

// ---------------------------------------------------------------------------
// Global registry — enables fork_reinit to reclaim dead-thread VA
// ---------------------------------------------------------------------------
//
// Singly-linked list of all ThreadScratchState objects. Protected by a
// spinlock. Thread creation/destruction is infrequent, so contention
// is negligible. The spinlock (not a futex) avoids circular dependency
// with the signal/futex subsystem — same rationale as SlabPool.

namespace scratch_detail {

inline cpp::Atomic<ThreadScratchState *> g_scratch_head{nullptr};
inline cpp::Atomic<uint32_t> g_scratch_list_lock{0};

LIBC_INLINE void registry_lock() {
  uint32_t expected = 0;
  if (LIBC_LIKELY(g_scratch_list_lock.compare_exchange_weak(
          expected, 1, cpp::MemoryOrder::ACQUIRE, cpp::MemoryOrder::RELAXED)))
    return;
  // Contended: hardware address monitor on the lock word. UMWAIT/MWAITX
  // sleeps at near-zero power until the cache line is written (lock
  // release), then retries CAS. Replaces NtYieldExecution which was a
  // full context switch per iteration.
  for (;;) {
    spin_wait::spin_until_changed(&g_scratch_list_lock, 1u);
    expected = 0;
    if (g_scratch_list_lock.compare_exchange_weak(
            expected, 1, cpp::MemoryOrder::ACQUIRE, cpp::MemoryOrder::RELAXED))
      return;
  }
}

LIBC_INLINE void registry_unlock() {
  g_scratch_list_lock.store(0, cpp::MemoryOrder::RELEASE);
}

LIBC_INLINE void registry_insert(ThreadScratchState *state) {
  registry_lock();
  state->next = g_scratch_head.load(cpp::MemoryOrder::RELAXED);
  g_scratch_head.store(state, cpp::MemoryOrder::RELAXED);
  registry_unlock();
}

LIBC_INLINE void registry_remove(ThreadScratchState *state) {
  registry_lock();
  auto *head = g_scratch_head.load(cpp::MemoryOrder::RELAXED);
  if (head == state) {
    g_scratch_head.store(state->next, cpp::MemoryOrder::RELAXED);
  } else {
    auto *prev = head;
    while (prev && prev->next != state)
      prev = prev->next;
    if (prev)
      prev->next = state->next;
  }
  registry_unlock();
}

} // namespace scratch_detail

// ---------------------------------------------------------------------------
// TLS slot management — process-wide, one-time init
// ---------------------------------------------------------------------------

namespace scratch_detail {

// Sentinel: TLS index not yet allocated.
inline constexpr DWORD TLS_UNINITIALIZED = TLS_OUT_OF_INDEXES;

// Process-wide TLS slot index. Allocated once by the first thread that
// needs scratch space. The atomic guards against concurrent first-use
// from multiple threads (e.g., thread pool burst at startup).
inline cpp::Atomic<DWORD> g_scratch_tls_index{TLS_UNINITIALIZED};

// One-time init lock. 0 = unlocked, 1 = locked.
inline cpp::Atomic<uint32_t> g_scratch_init_lock{0};

// Thread-exit cleanup callback. Called by tls_cleanup_run_all() via
// the .CRT$XLC TLS callback on DLL_THREAD_DETACH.
// Not LIBC_INLINE: address-taken for the callback pointer.
// NOLINTBEGIN(llvmlibc-inline-function-decl)
inline void NTAPI scratch_thread_cleanup(void *val) {
  if (!val)
    return;
  auto *state = static_cast<ThreadScratchState *>(val);
  // Remove from global registry before freeing VA.
  registry_remove(state);
  // Release the entire 64 KB VA reservation.
  page_free(state->reserve_base);
}
// NOLINTEND(llvmlibc-inline-function-decl)

// Allocate the process-wide TLS slot and register the cleanup callback.
// Returns the TLS index, or TLS_UNINITIALIZED on failure.
// Serialized by g_scratch_init_lock for the rare concurrent-first-use case.
LIBC_INLINE DWORD init_tls_slot() {
  // Fast check: already initialized?
  DWORD idx = g_scratch_tls_index.load(cpp::MemoryOrder::ACQUIRE);
  if (idx != TLS_UNINITIALIZED)
    return idx;

  // Acquire init lock.
  uint32_t expected = 0;
  if (!g_scratch_init_lock.compare_exchange_strong(
          expected, 1, cpp::MemoryOrder::ACQUIRE, cpp::MemoryOrder::RELAXED)) {
    // Another thread is initializing. Hardware monitor spin on the TLS
    // index — UMWAIT/MWAITX sleeps until the cache line is written
    // (init complete), then re-checks. Replaces NtYieldExecution.
    // Uses spin_on_raw_u32 because DWORD (unsigned long) != uint32_t
    // (unsigned int) on Windows — same size, different type.
    while (g_scratch_tls_index.load(cpp::MemoryOrder::ACQUIRE) ==
           TLS_UNINITIALIZED) {
      spin_wait::spin_on_raw_u32(&g_scratch_tls_index.val, TLS_UNINITIALIZED);
    }
    return g_scratch_tls_index.load(cpp::MemoryOrder::ACQUIRE);
  }

  // We hold the lock. Double-check.
  idx = g_scratch_tls_index.load(cpp::MemoryOrder::RELAXED);
  if (idx != TLS_UNINITIALIZED) {
    g_scratch_init_lock.store(0, cpp::MemoryOrder::RELEASE);
    return idx;
  }

  // Runtime page size validation. The geometry constants are compiled
  // against 4KB pages / 64KB granularity. A mismatch means guard pages,
  // commit ranges, and the control/data layout are silently wrong.
  // Cannot be static_assert (runtime OS query); must survive NDEBUG.
  // Same trap pattern as SlabPool::init.
  if (LIBC_UNLIKELY(windows::get_page_size() != PAGE_SIZE ||
                    windows::get_alloc_granularity() != ALLOC_GRANULARITY))
    __builtin_trap();

  // Allocate a TEB inline TLS slot.
  idx = tls_alloc();
  if (idx == TLS_OUT_OF_INDEXES) {
    g_scratch_init_lock.store(0, cpp::MemoryOrder::RELEASE);
    return TLS_UNINITIALIZED;
  }

  // Register thread-exit cleanup. The cleanup callback will be called
  // on every thread detach (including the main thread at process exit).
  tls_cleanup_register(idx, scratch_thread_cleanup);

  // Publish. RELEASE ensures the cleanup registration is visible before
  // other threads see the index and start creating per-thread state.
  g_scratch_tls_index.store(idx, cpp::MemoryOrder::RELEASE);
  g_scratch_init_lock.store(0, cpp::MemoryOrder::RELEASE);
  return idx;
}

} // namespace scratch_detail

// ---------------------------------------------------------------------------
// Per-thread state creation (cold path, once per thread)
// ---------------------------------------------------------------------------

namespace scratch_detail {

// Create a new ThreadScratchState for the calling thread.
// Reserves 64 KB VA, commits control page + 2 data pages (12 KB),
// generates CSPRNG canary seed, randomizes bump start, registers in
// global registry, stores pointer in TLS slot.
// Returns the state pointer, or nullptr on failure.
LIBC_INLINE ThreadScratchState *create_thread_state(DWORD tls_index) {
  // Reserve 64 KB of contiguous VA.
  void *base = page_reserve(RESERVE_SIZE);
  if (!base)
    return nullptr;

  auto *b = static_cast<char *>(base);

  // Commit page 0 (control block).
  if (!page_commit(b, PAGE_SIZE)) {
    page_free(base);
    return nullptr;
  }

  // Commit pages 2-3 (first two data pages).
  // Page 1 (leading guard) stays uncommitted — any access faults.
  if (!page_commit(b + DATA_OFFSET, INITIAL_DATA_COMMIT)) {
    page_free(base);
    return nullptr;
  }

  // Initialize the control block at page 0.
  auto *state = reinterpret_cast<ThreadScratchState *>(b + CONTROL_OFFSET);
  state->data_base = b + DATA_OFFSET;
  state->commit_limit = b + DATA_OFFSET + INITIAL_DATA_COMMIT;
  state->data_limit = b + TRAILING_GUARD_OFFSET;
  state->reserve_base = b;

  // Generate per-thread canary seed from CSPRNG. The seed is used to
  // derive per-allocation canaries. Non-zero enforcement: a zero seed
  // would make canaries equal to the address, which is predictable.
  // Same pattern as SlabPool::init_slab (freelist_cookie + canary_key).
  ::ProcessPrng(reinterpret_cast<unsigned char *>(&state->canary_seed),
                sizeof(state->canary_seed));
  if (state->canary_seed == 0)
    state->canary_seed = 0xA5A5'A5A5'DEAD'BEEFull;

  // Bump start randomization: CSPRNG offset within the first data page.
  // Resists heap spray attacks by making allocation addresses unpredictable.
  // Same concept as SlabPool::bump_offset. Bounded to 25% of first page
  // (64 positions × 16-byte alignment = 1024 bytes max waste).
  uint16_t rand_val;
  ::ProcessPrng(reinterpret_cast<unsigned char *>(&rand_val),
                sizeof(rand_val));
  size_t random_offset =
      static_cast<size_t>(rand_val % RANDOM_POSITIONS) * ALLOC_ALIGN;
  state->bump = state->data_base + random_offset;

  // Record owner TID for fork dead-thread identification.
  state->owner_tid = NtCurrentThreadId();

  // Zero-initialize cold fields.
  state->next = nullptr;
  state->pad_ = 0;

  // Initialize the ThreadLocalWord for cross-thread signaling.
  // Sets owner_tid_ from TEB, clears value/generation/park_state.
  state->word.init();

  // Register in global list for fork_reinit dead-thread reclamation.
  registry_insert(state);

  // Store in TLS for fast retrieval.
  teb_tls_set(tls_index, state);

  return state;
}

} // namespace scratch_detail

// ---------------------------------------------------------------------------
// get_thread_scratch — fast TLS read + lazy init
// ---------------------------------------------------------------------------

// Returns the calling thread's scratch state. Lazy: first call per thread
// allocates the VA reservation and initializes the control block.
// Returns nullptr only on catastrophic failure (OOM or TLS exhaustion).
LIBC_INLINE ThreadScratchState *get_thread_scratch() {
  DWORD idx = scratch_detail::g_scratch_tls_index.load(
      cpp::MemoryOrder::ACQUIRE);
  if (LIBC_UNLIKELY(idx == scratch_detail::TLS_UNINITIALIZED)) {
    idx = scratch_detail::init_tls_slot();
    if (idx == scratch_detail::TLS_UNINITIALIZED)
      return nullptr;
  }

  auto *state =
      static_cast<ThreadScratchState *>(teb_tls_get(idx));
  if (LIBC_LIKELY(state != nullptr))
    return state;

  // First use on this thread — create the per-thread state.
  return scratch_detail::create_thread_state(idx);
}

// ---------------------------------------------------------------------------
// scratch_alloc / scratch_release — core alloc primitives
// ---------------------------------------------------------------------------

namespace scratch_detail {

// Demand-commit cold path. Called when the bump pointer would advance past
// the current commit watermark. Commits pages up to the new bump position.
// Returns the user pointer (past canary), or nullptr on failure.
LIBC_INLINE void *alloc_slow(ThreadScratchState *state, size_t total) {
  char *alloc_base = state->bump;
  char *new_bump = alloc_base + total;

  // Check against data region limit (trailing guard boundary).
  if (LIBC_UNLIKELY(new_bump > state->data_limit))
    return nullptr;

  // Round commit target up to page boundary.
  char *new_commit = reinterpret_cast<char *>(
      (reinterpret_cast<uintptr_t>(new_bump) + PAGE_SIZE - 1) &
      ~(PAGE_SIZE - 1));

  // Clamp to data limit (don't commit into the trailing guard page).
  if (new_commit > state->data_limit)
    new_commit = state->data_limit;

  // Commit the gap.
  size_t commit_size =
      static_cast<size_t>(new_commit - state->commit_limit);
  if (commit_size > 0) {
    if (!page_commit(state->commit_limit, commit_size))
      return nullptr;
    state->commit_limit = new_commit;
  }

  // Write canary and advance bump.
  write_canary(state->canary_seed, alloc_base);
  state->bump = new_bump;
  return alloc_base + CANARY_OVERHEAD;
}

// Handle pending cross-thread signals. Cold path — only called when
// word.any_pending() returns true (predicted not-taken on the alloc path).
LIBC_INLINE void scratch_handle_pending(ThreadScratchState *state) {
  if (state->word.test_flags(scratch_flags::DECOMMIT)) {
    // Decommit demand-committed pages above the initial level.
    // Same logic as the high-watermark decommit in scratch_release,
    // but triggered by cross-thread memory pressure rather than LIFO rewind.
    char *floor = state->data_base + INITIAL_DATA_COMMIT;
    if (state->commit_limit > floor) {
      char *needed = reinterpret_cast<char *>(
          (reinterpret_cast<uintptr_t>(state->bump) + PAGE_SIZE - 1) &
          ~(PAGE_SIZE - 1));
      if (needed < floor)
        needed = floor;
      if (state->commit_limit > needed) {
        page_decommit(needed,
                      static_cast<size_t>(state->commit_limit - needed));
        state->commit_limit = needed;
      }
    }
    state->word.clear_flags(scratch_flags::DECOMMIT);
  }
}

} // namespace scratch_detail

// Allocate `size` bytes from the thread's scratch region.
// Returns a 16-byte aligned pointer, or nullptr on failure.
// The allocation is prefixed with a canary block (not visible to caller).
LIBC_INLINE void *scratch_alloc(ThreadScratchState *state, size_t size) {
  // Check for pending cross-thread signals. Zero cost when clear:
  // one RELAXED load (MOV) + TEST + predicted-not-taken Jcc.
  if (LIBC_UNLIKELY(state->word.any_pending()))
    scratch_detail::scratch_handle_pending(state);

  // Total = canary overhead + user size, rounded up to alignment.
  size_t total =
      (scratch_detail::CANARY_OVERHEAD + size +
       scratch_detail::ALLOC_ALIGN - 1) &
      ~(scratch_detail::ALLOC_ALIGN - 1);

  char *alloc_base = state->bump;
  char *new_bump = alloc_base + total;

  // Hot path: within committed pages.
  if (LIBC_LIKELY(new_bump <= state->commit_limit)) {
    scratch_detail::write_canary(state->canary_seed, alloc_base);
    state->bump = new_bump;
    return alloc_base + scratch_detail::CANARY_OVERHEAD;
  }

  // Cold path: demand-commit.
  return scratch_detail::alloc_slow(state, total);
}

// Release a scratch allocation. Validates the canary, checks pointer
// bounds, zeroes user data, and rewinds the bump pointer. MUST be called
// in LIFO order (enforced by RAII ScratchBuf destructor ordering).
//
// High-watermark decommit: if the bump pointer rewinds below demand-committed
// pages, those pages are decommitted (physical memory returned to OS, VA
// preserved, UAF protection via PAGE_NOACCESS). The initial 2 data pages
// are never decommitted (avoids commit/decommit churn for the common
// single-buffer case). Same principle as SlabPool's seal→decommit lifecycle.
LIBC_INLINE void scratch_release(ThreadScratchState *state, void *ptr,
                                  size_t size) {
  char *alloc_base =
      static_cast<char *>(ptr) - scratch_detail::CANARY_OVERHEAD;

  // Pointer range validation — trap on wild pointers. Same defense as
  // SlabPool::validate_slot checking pointer falls within slab bounds.
  if (LIBC_UNLIKELY(alloc_base < state->data_base ||
                    alloc_base >= state->data_limit))
    __builtin_trap();

  // Validate canary — traps on corruption.
  scratch_detail::verify_canary(state->canary_seed, alloc_base);

  // Zero user data to prevent info leaks.
  // volatile to prevent the compiler from optimizing away the memset
  // ("dead store elimination") since the memory is about to be logically
  // freed. Same motivation as SlabPool::harden_slot.
  volatile unsigned char *vptr = static_cast<volatile unsigned char *>(ptr);
  for (size_t i = 0; i < size; ++i)
    vptr[i] = 0;

  // Zero the canary block itself.
  volatile uintptr_t *cptr =
      reinterpret_cast<volatile uintptr_t *>(alloc_base);
  cptr[0] = 0;
  cptr[1] = 0;

  // Rewind bump pointer.
  state->bump = alloc_base;

  // High-watermark decommit: return demand-committed pages above the
  // initial level. Only fires when usage has exceeded the initial 8KB
  // and then drops back. Common single-buffer case (within initial
  // 2 data pages) never enters this path.
  //
  // Floor: initial data commit boundary. Pages below this are always
  // committed to avoid churn on the hot single-path-buffer pattern.
  char *floor = state->data_base + scratch_detail::INITIAL_DATA_COMMIT;
  if (state->commit_limit > floor) {
    // Round bump up to next page boundary to find the minimum needed commit.
    char *needed = reinterpret_cast<char *>(
        (reinterpret_cast<uintptr_t>(state->bump) +
         scratch_detail::PAGE_SIZE - 1) &
        ~(scratch_detail::PAGE_SIZE - 1));
    // Never decommit below the initial level.
    if (needed < floor)
      needed = floor;
    // Decommit the excess. Physical memory returned, VA preserved.
    // Decommitted pages are PAGE_NOACCESS — accesses fault cleanly,
    // providing UAF protection identical to SlabPool's decommitted state.
    if (state->commit_limit > needed) {
      page_decommit(needed,
                    static_cast<size_t>(state->commit_limit - needed));
      state->commit_limit = needed;
    }
  }
}

// ---------------------------------------------------------------------------
// ScratchBuf<T> — RAII guard for scratch allocations
// ---------------------------------------------------------------------------
//
// Non-copyable, non-movable. Stack-scoped RAII ensures LIFO release order
// (C++ destructor ordering guarantees reverse construction order).
//
// Usage:
//   ScratchBuf<WCHAR> buf(4096);  // 4096 WCHARs = 8 KB
//   if (!buf) return -ENOMEM;
//   size_t len = to_nt_path(path, buf.data(), buf.size());

template <typename T>
class ScratchBuf {
  ThreadScratchState *state_;
  T *ptr_;
  size_t count_;

public:
  LIBC_INLINE explicit ScratchBuf(size_t count)
      : state_(nullptr), ptr_(nullptr), count_(count) {
    state_ = get_thread_scratch();
    if (LIBC_LIKELY(state_ != nullptr)) {
      void *raw = scratch_alloc(state_, count * sizeof(T));
      ptr_ = static_cast<T *>(raw);
    }
  }

  LIBC_INLINE ~ScratchBuf() {
    if (LIBC_LIKELY(ptr_ != nullptr))
      scratch_release(state_, ptr_, count_ * sizeof(T));
  }

  // Non-copyable, non-movable — LIFO discipline requires fixed stack position.
  ScratchBuf(const ScratchBuf &) = delete;
  ScratchBuf &operator=(const ScratchBuf &) = delete;
  ScratchBuf(ScratchBuf &&) = delete;
  ScratchBuf &operator=(ScratchBuf &&) = delete;

  /// True if the allocation succeeded.
  LIBC_INLINE explicit operator bool() const { return ptr_ != nullptr; }

  /// Pointer to the allocated buffer.
  LIBC_INLINE T *data() { return ptr_; }
  LIBC_INLINE const T *data() const { return ptr_; }

  /// Number of T elements in the buffer.
  LIBC_INLINE size_t size() const { return count_; }

  /// Byte size of the allocation.
  LIBC_INLINE size_t size_bytes() const { return count_ * sizeof(T); }

  /// Element access.
  LIBC_INLINE T &operator[](size_t i) {
    LIBC_ASSERT(i < count_ && "ScratchBuf: index out of bounds");
    return ptr_[i];
  }
  LIBC_INLINE const T &operator[](size_t i) const {
    LIBC_ASSERT(i < count_ && "ScratchBuf: index out of bounds");
    return ptr_[i];
  }
};

// ---------------------------------------------------------------------------
// Convenience factories and size constants
// ---------------------------------------------------------------------------
//
// Each factory returns a ScratchBuf that bumps within the same per-thread
// 64 KB arena. Multiple outstanding ScratchBufs nest correctly (LIFO).
// Adding more call sites has zero additional memory cost — they all share
// the same reservation.

// POSIX PATH_MAX in WCHARs (UTF-16). One full path conversion buffer.
// 4096 UTF-8 bytes → at most 4096 WCHARs (one UTF-8 byte → at most one
// UTF-16 code unit for BMP characters, which covers all path characters).
// Plus room for the \??\ prefix and NUL terminator.
inline constexpr size_t PATH_SCRATCH_WCHARS = 4096 + 8;

// -- path_scratch() ----------------------------------------------------------

// Allocate a WCHAR buffer for one path conversion (~8 KB).
//
// Usage:
//   auto s = path_scratch();
//   if (!s) return -ENOMEM;
//   size_t len = to_nt_path(path, s.data(), s.size());
LIBC_INLINE ScratchBuf<WCHAR> path_scratch() {
  return ScratchBuf<WCHAR>(PATH_SCRATCH_WCHARS);
}

// -- byte_scratch(n) ---------------------------------------------------------

// Generic byte-buffer scratch. Covers walk buffers, topology queries,
// security descriptors, reparse data, and other temporary allocations.
// Returned pointer is 16-byte aligned (sufficient for SSE, NT structs).
//
// Usage:
//   auto s = byte_scratch(8192);
//   if (!s) return -ENOMEM;
//   NtQuerySystemInformation(..., s.data(), s.size(), ...);
LIBC_INLINE ScratchBuf<char> byte_scratch(size_t bytes) {
  return ScratchBuf<char>(bytes);
}

// -- info_scratch<S>() -------------------------------------------------------

// Scratch for NT info struct with a trailing wide-char filename field.
// Allocates sizeof(S) + PATH_SCRATCH_WCHARS * sizeof(WCHAR) bytes.
// Covers FILE_LINK_INFORMATION, FILE_RENAME_INFORMATION,
// OBJECT_NAME_INFORMATION, and similar variable-length NT structures.
//
// Usage:
//   auto s = info_scratch<FILE_RENAME_INFORMATION>();
//   if (!s) return -ENOMEM;
//   auto *info = reinterpret_cast<FILE_RENAME_INFORMATION *>(s.data());
//   info->FileNameLength = ...;
template <typename S>
LIBC_INLINE ScratchBuf<char> info_scratch() {
  static_assert(alignof(S) <= scratch_detail::ALLOC_ALIGN,
                "struct alignment exceeds scratch allocation alignment");
  return ScratchBuf<char>(sizeof(S) + PATH_SCRATCH_WCHARS * sizeof(WCHAR));
}

// ---------------------------------------------------------------------------
// Cross-thread signaling
// ---------------------------------------------------------------------------
//
// These functions operate on another thread's scratch state. The caller
// obtains the ThreadScratchState pointer from the global registry walk
// or a thread registry lookup. Lock-free, O(1).

/// Request a thread to decommit demand-committed scratch pages on its
/// next allocation. Non-blocking. If the target thread is kernel-parked
/// in wait_for_change(), the alert wakes it immediately; otherwise the
/// cache-line write wakes any hardware monitor, or the flag is picked up
/// on the next scratch_alloc() call.
LIBC_INLINE void scratch_request_decommit(ThreadScratchState *target) {
  ThreadLocalWord::signal_or(&target->word, scratch_flags::DECOMMIT);
}

// ---------------------------------------------------------------------------
// Fork safety
// ---------------------------------------------------------------------------

// Reset scratch state after fork. Only the forking thread survives.
//
// Actions:
//   1. Reset all locks (may have been held by dead threads at fork snapshot).
//   2. Walk global registry:
//      - Surviving thread's state: preserved (same VA, same bump position).
//      - Dead threads' states: VA released (64 KB freed per dead thread).
//   3. Rebuild registry with only the surviving thread's entry.
//
// Single-threaded at this point (only the forking thread runs in the child).
// No lock acquisition needed for the walk — we reset the locks first.
LIBC_INLINE void scratch_fork_reinit() {
  // Reset locks. They may have been held by dead threads at the fork
  // snapshot, which would deadlock the surviving thread.
  scratch_detail::g_scratch_init_lock.store(0, cpp::MemoryOrder::RELAXED);
  scratch_detail::g_scratch_list_lock.store(0, cpp::MemoryOrder::RELAXED);

  DWORD my_tid = NtCurrentThreadId();

  // Walk registry: free dead threads' VA, keep only the surviving thread.
  ThreadScratchState *node =
      scratch_detail::g_scratch_head.load(cpp::MemoryOrder::RELAXED);
  ThreadScratchState *survivor = nullptr;

  while (node) {
    ThreadScratchState *next = node->next;
    if (node->owner_tid == my_tid) {
      // Surviving thread — preserve. Clear link for rebuilt list.
      // Reinit the ThreadLocalWord (child gets new TIDs, clear park state).
      node->next = nullptr;
      node->word.fork_reinit();
      survivor = node;
    } else {
      // Dead thread — release entire 64 KB VA reservation.
      page_free(node->reserve_base);
    }
    node = next;
  }

  // Rebuild registry with only the survivor.
  scratch_detail::g_scratch_head.store(survivor, cpp::MemoryOrder::RELAXED);
}

} // namespace internal

namespace windows {

using internal::PATH_SCRATCH_WCHARS;

LIBC_INLINE internal::ScratchBuf<WCHAR> path_scratch() {
  return internal::path_scratch();
}

LIBC_INLINE internal::ScratchBuf<char> byte_scratch(size_t bytes) {
  return internal::byte_scratch(bytes);
}

template <typename S> LIBC_INLINE internal::ScratchBuf<char> info_scratch() {
  return internal::info_scratch<S>();
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_THREAD_SCRATCH_H
