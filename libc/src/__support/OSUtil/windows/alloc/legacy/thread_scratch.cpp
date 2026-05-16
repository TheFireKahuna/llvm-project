//===-- Out-of-line hooks for the per-thread scratch allocator -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Hooks declared in thread_scratch.h (scratch_detail::) that bridge the
// inline allocator code to the subsystems it consumes on arena
// lifecycle events:
//
//   - mapping_table (create/destroy LIBC_INTERNAL entries for each
//     ThreadScratch arena — primary AND each overflow)
//   - Crystalline domain registry (flush the exiting thread's batches
//     before the arena backing is released)
//
// Keeping these out of line lets thread_scratch.h stay free of
// transitive dependencies on mapping_table / crystalline_domain_registry —
// avoiding header cycles with subsystems that themselves consume
// ThreadScratch for their own scratch buffers.
//
// VA ownership: every ThreadScratch arena is reserved directly via
// `page_reserve` and released directly via `page_free`. No substrate
// involvement — `va_substrate.h` is intentionally absent from the
// include set. The substrate's `ConsumerTag::ThreadScratch` is no
// longer used.
//
// Also hosts the .libcfin thunk that unregisters the scratch TLS slot
// on DLL_PROCESS_DETACH.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain_registry.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/memory/legacy/mapping_table.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace scratch_detail {

void scratch_arena_unregister(void *base) {
  // `remove` is a no-op if the base isn't present — matches our need
  // for the fork-child / exec-hollow paths where the mapping table was
  // reset without walking ThreadScratch.
  ::LIBC_NAMESPACE::windows::g_mapping_table.remove(base);
}

void scratch_flush_crystalline_batches(ThreadScratchState *state) {
  // Walk every registered CrystallineDomain. For each, pick
  // `state->crystalline_batches[desc->domain_id]` and run its close+
  // publish sequence (mark the anchor with WFR_RNODE, run try_retire
  // across live pool slots). After this call the batch storage is about
  // to disappear with the arena VA; any batch bookkeeping that survives
  // must live in published slot chains or in the freed-node reclamation
  // path.
  if (state == nullptr)
    return;
  ::LIBC_NAMESPACE::concurrent::registry_flush_thread_all(
      state->crystalline_batches);
}

void scratch_release_crystalline_slots(ThreadScratchState *state) {
  // Walk every registered CrystallineDomain. For each, release the
  // exiting thread's slot (`state->crystalline_slot_idx[desc->domain_id]`)
  // back to the domain's pool. An entry of 0 means the thread never
  // reserved in that domain — skip. Runs after
  // scratch_flush_crystalline_batches so the batches' retires are
  // already republished into the slot chains the released slot is
  // leaving behind.
  if (state == nullptr)
    return;
  ::LIBC_NAMESPACE::concurrent::registry_release_slots_all(
      state->crystalline_slot_idx);
}

// =========================================================================
// Pending internal-region receipts queue (pre-INIT_READY)
// =========================================================================
//
// Two paths enqueue here while the mapping table is still INIT_IN_PROGRESS
// (and `register_mapping_internal` would self-deadlock on a same-thread
// re-entry into `ensure_init`):
//
//   1. ThreadScratch's `create_thread_state` when called pre-INIT_READY —
//      a transient Windows-loader worker spawned during DllMain that
//      reaches `get_thread_scratch` before the mapping table is
//      observable. The arena is reserved directly via `page_reserve`
//      and a `{base, RESERVE_SIZE, BootstrapScratch}` receipt is
//      queued here.
//
//   2. Substrate's `reserve_new_arena` when called pre-INIT_READY (e.g.
//      if `mapping_table_init_fn`'s own `ensure_init` drains the Small
//      seed arena and triggers a fresh substrate reservation). Queues a
//      `{base, arena_size, SubstrateArena}` receipt here.
//
// The walker's `mapping_table_init_fn` (in mapping_table.cpp) drains the
// queue after `ensure_init` succeeds; Pass 2 stamps each entry as
// LIBC_INTERNAL.
//
// Lock serializes path-decision + enqueue with harvest. Without it, an
// enqueuer could load `is_init_ready() == false`, get preempted while the
// walker advances the table to INIT_READY and harvests, then resume and
// enqueue post-harvest — leaving the receipt orphaned. Holding a single
// small spinlock across [check + setup + enqueue] eliminates the race;
// harvest takes the same lock.

struct PendingInternalReceiptEntry {
  void *base;
  size_t size;
  ::LIBC_NAMESPACE::internal::InternalKind kind;
};

// File-scope globals (TU-local via `static`). Not in an anonymous
// namespace so `scratch_fork_reinit` — which is defined at the
// enclosing `internal::` scope — can name them as
// `scratch_detail::g_pending_internal_receipts_*`.
static cpp::Atomic<uint32_t> g_pending_internal_receipts_lock{0};
static PendingInternalReceiptEntry
    g_pending_internal_receipts[kPendingInternalReceiptsCap];
// Plain non-atomic count — every read/write happens under
// g_pending_internal_receipts_lock, so no atomic semantics are needed
// for the counter itself.
static uint32_t g_pending_internal_receipts_count = 0;

// Lock helpers — public within scratch_detail. Used by both the bootstrap-
// tier scratch path (around create_thread_state's path decision) and by
// the walker's post-Pass-2 finalize (mapping_table.cpp), which holds the
// lock across [mark_ready + drain] so any substrate reserve triggered by
// Pass 2's register_mapping_internal is either drained before ready
// latches or sees ready=true under the lock and inline-stamps after.
void pending_internal_receipts_lock() {
  uint32_t expected = 0;
  if (LIBC_LIKELY(g_pending_internal_receipts_lock.compare_exchange_weak(
          expected, 1, cpp::MemoryOrder::ACQUIRE,
          cpp::MemoryOrder::RELAXED)))
    return;
  for (;;) {
    spin_wait::spin_until_changed(&g_pending_internal_receipts_lock, 1u);
    if (g_pending_internal_receipts_lock.load(cpp::MemoryOrder::RELAXED) !=
        0)
      continue;
    expected = 0;
    if (g_pending_internal_receipts_lock.compare_exchange_weak(
            expected, 1, cpp::MemoryOrder::ACQUIRE,
            cpp::MemoryOrder::RELAXED))
      return;
  }
}

void pending_internal_receipts_unlock() {
  g_pending_internal_receipts_lock.store(0, cpp::MemoryOrder::RELEASE);
}

// Caller MUST hold the pending-internal-receipts lock. Trap on cap
// overflow — kPendingInternalReceiptsCap is sized for the plausible
// worst case of LDR workers + mapping-table-init-driven substrate
// reserves.
void enqueue_pending_internal_receipt_locked(
    void *base, size_t size,
    ::LIBC_NAMESPACE::internal::InternalKind kind) {
  if (LIBC_UNLIKELY(g_pending_internal_receipts_count >=
                    kPendingInternalReceiptsCap))
    __builtin_trap();
  g_pending_internal_receipts[g_pending_internal_receipts_count].base = base;
  g_pending_internal_receipts[g_pending_internal_receipts_count].size = size;
  g_pending_internal_receipts[g_pending_internal_receipts_count].kind = kind;
  ++g_pending_internal_receipts_count;
}

// Drain the queue into `out` under the assumption that the caller holds
// the pending-internal-receipts lock. Returns the number of receipts
// drained; cap-checks against `out_cap` and traps on insufficient room
// (the caller's buffer must be sized to kPendingInternalReceiptsCap).
uint32_t drain_pending_internal_receipts_locked(
    ::LIBC_NAMESPACE::internal::Receipt *out, uint32_t out_cap) {
  const uint32_t n = g_pending_internal_receipts_count;
  if (LIBC_UNLIKELY(n > out_cap))
    __builtin_trap();
  for (uint32_t i = 0; i < n; ++i) {
    out[i].base = g_pending_internal_receipts[i].base;
    out[i].size = g_pending_internal_receipts[i].size;
    out[i].kind = g_pending_internal_receipts[i].kind;
    g_pending_internal_receipts[i].base = nullptr;
    g_pending_internal_receipts[i].size = 0;
    g_pending_internal_receipts[i].kind =
        ::LIBC_NAMESPACE::internal::InternalKind::Unknown;
  }
  g_pending_internal_receipts_count = 0;
  return n;
}

void enqueue_pending_internal_receipt(
    void *base, size_t size,
    ::LIBC_NAMESPACE::internal::InternalKind kind) {
  pending_internal_receipts_lock();
  enqueue_pending_internal_receipt_locked(base, size, kind);
  pending_internal_receipts_unlock();
}

uint32_t harvest_pending_internal_receipts(
    ::LIBC_NAMESPACE::internal::Receipt *out, uint32_t cap) {
  pending_internal_receipts_lock();
  const uint32_t n = drain_pending_internal_receipts_locked(out, cap);
  pending_internal_receipts_unlock();
  return n;
}

// =========================================================================
// create_thread_state — single direct-VA path
// =========================================================================

namespace {

// Common control-block initialization. `state` lives at the slot base;
// `slot_base` is the same pointer (kept separate for clarity at the
// call sites). Zeros the ENTIRE ThreadScratchState first so all
// reserved_/pad bytes start clean.
void init_scratch_state(ThreadScratchState *state, char *slot_base) {
  __builtin_memset(state, 0, sizeof(*state));

  state->arena.data_base = slot_base + scratch_detail::DATA_OFFSET;
  state->arena.commit_limit =
      slot_base + scratch_detail::DATA_OFFSET +
      scratch_detail::INITIAL_DATA_COMMIT;
  state->arena.data_limit = slot_base + scratch_detail::TRAILING_GUARD_OFFSET;
  state->arena.free_list_head = nullptr;

  // Per-arena CSPRNG seed. Fail-closed: a transient ProcessPrng failure
  // or a zero draw indicates process corruption. Substituting a
  // compile-time sentinel would defeat the canary / freelist-XOR
  // hardening. Same policy as SlabPool / IndexedPool.
  ::LIBC_NAMESPACE::internal::alloc_primitives::init_seed_or_trap(
      state->arena.canary_seed);

  // Bump start randomization (Lemire's fast range reduction):
  // (rand16 * N) >> 16 maps uniformly onto [0, N-1] with at most
  // 1/65536 bias. Bounded to 25% of the first data page.
  uint16_t rand_val;
  ::ProcessPrng(reinterpret_cast<unsigned char *>(&rand_val),
                sizeof(rand_val));
  size_t random_offset =
      static_cast<size_t>(
          (static_cast<uint32_t>(rand_val) *
           scratch_detail::RANDOM_POSITIONS) >>
          16) *
      scratch_detail::ALLOC_ALIGN;
  state->arena.bump = state->arena.data_base + random_offset;

  state->owner_tid = NtCurrentThreadId();
  state->word.init();
}

// Reserve + commit the per-thread 64 KB region. On failure, page_frees
// any partial reservation and returns nullptr.
char *reserve_and_commit_arena() {
  void *raw =
      ::LIBC_NAMESPACE::internal::page_reserve(scratch_detail::RESERVE_SIZE);
  if (LIBC_UNLIKELY(!raw))
    return nullptr;
  char *b = static_cast<char *>(raw);

  // Three eager commits matching the kArenaLayout's commit_eager regions
  // plus the explicit commit_subrange for the initial data pages. Routed
  // through GuardedRegion's static API so the layout-encoded asserts
  // (kind != GUARD, page-aligned offset/size, offset+size within region)
  // catch any future kArenaLayout drift at the call site.
  if (LIBC_UNLIKELY(
          !alloc_primitives::GuardedRegion::commit_region(
              b, scratch_detail::kArenaLayout,
              scratch_detail::CONTROL_REGION_IDX)) ||
      LIBC_UNLIKELY(
          !alloc_primitives::GuardedRegion::commit_subrange(
              b, scratch_detail::kArenaLayout,
              scratch_detail::DATA_REGION_IDX, /*offset=*/0,
              scratch_detail::INITIAL_DATA_COMMIT))) {
    ::LIBC_NAMESPACE::internal::page_free(raw);
    return nullptr;
  }
  return b;
}

} // namespace

ScratchOverflowArena *create_overflow_arena(ThreadScratchState *state) {
  // Overflow only fires after a thread has used 36 KB of scratch, so by
  // construction the mapping table is INIT_READY long before this runs
  // — every thread that reaches overflow has long since finished its
  // primary `create_thread_state` and therefore observed INIT_READY at
  // some prior allocation. Trap on the impossible case rather than
  // silently emitting a receipt that no walker will harvest.
  if (LIBC_UNLIKELY(
          !::LIBC_NAMESPACE::windows::g_mapping_table.is_init_ready()))
    __builtin_trap();

  char *b = reserve_and_commit_arena();
  if (LIBC_UNLIKELY(!b))
    return nullptr;

  // Stamp LIBC_INTERNAL inline. The mapping table is INIT_READY by the
  // overflow precondition above, so `register_mapping_internal` cannot
  // self-deadlock on `ensure_init`. Failure here unwinds the
  // reservation — leaving an unregistered arena live would silently
  // corrupt /proc-style introspection.
  if (LIBC_UNLIKELY(
          !::LIBC_NAMESPACE::windows::g_mapping_table.register_mapping_internal(
              b, RESERVE_SIZE))) {
    ::LIBC_NAMESPACE::internal::page_free(b);
    return nullptr;
  }

  auto *ov = reinterpret_cast<ScratchOverflowArena *>(b);
  ov->arena.data_base = b + DATA_OFFSET;
  ov->arena.commit_limit = b + DATA_OFFSET + INITIAL_DATA_COMMIT;
  ov->arena.data_limit = b + TRAILING_GUARD_OFFSET;
  ov->arena.free_list_head = nullptr;

  // Per-arena CSPRNG seed. Same fail-closed policy as the primary path.
  alloc_primitives::init_seed_or_trap(ov->arena.canary_seed);

  // Bump-start randomization (Lemire's fast range reduction).
  uint16_t rand_val;
  ::ProcessPrng(reinterpret_cast<unsigned char *>(&rand_val),
                sizeof(rand_val));
  size_t random_offset =
      static_cast<size_t>((static_cast<uint32_t>(rand_val) *
                           RANDOM_POSITIONS) >>
                          16) *
      ALLOC_ALIGN;
  ov->arena.bump = ov->arena.data_base + random_offset;

  // No live allocations yet — the caller's `++ov->live_blocks` after
  // its first `try_alloc_from_arena` brings the counter to 1.
  ov->live_blocks = 0;
  ov->pad = 0;

  // Link into overflow chain (head-insert; cheap and order-irrelevant).
  ov->next = state->overflow;
  state->overflow = ov;

  return ov;
}

void release_empty_overflow(ThreadScratchState *state,
                            ScratchOverflowArena *ov) {
  LIBC_ASSERT(ov->live_blocks == 0 &&
              "release_empty_overflow: arena must hold no live blocks");

  // Splice from the overflow chain. O(N) chain walk; N is small (≤ a
  // handful in practice) and this only fires when an overflow arena
  // empties — rare relative to the alloc/free hot path.
  if (state->overflow == ov) {
    state->overflow = ov->next;
  } else {
    auto *prev = state->overflow;
    while (prev != nullptr && prev->next != ov)
      prev = prev->next;
    LIBC_ASSERT(prev != nullptr &&
                "release_empty_overflow: arena not in chain");
    prev->next = ov->next;
  }

  // Drop the mapping-table entry, then return the 64 KB VA to NT.
  // Order matters: a substrate-style introspection walker that sees
  // the entry removed knows the page_free is imminent or already
  // happened; a walker that sees the entry present can dereference
  // safely. The reverse order would let a walker dereference into
  // freed VA.
  ::LIBC_NAMESPACE::windows::g_mapping_table.remove(ov);
  ::LIBC_NAMESPACE::internal::page_free(ov);
}

ThreadScratchState *create_thread_state(DWORD tls_index) {
  // Single direct-VA path. Branch only at mapping-table-stamp time —
  // either inline `register_mapping_internal` (post-INIT_READY, the
  // common case for every thread spawned past Tier A) or queue a
  // pending receipt for the walker's Pass 2 to stamp.
  //
  // The init-ready latch is monotonic (false → true once, never
  // reverses), so the post-Tier-A hot path takes ZERO locks. The
  // locked branch is entered only during Tier A's narrow bring-up
  // window — transient Windows-loader workers spawned during DllMain
  // before the mapping table reaches INIT_READY.
  //
  // ORDERING IS LOAD-BEARING: `teb_tls_set` runs BEFORE
  // `register_mapping_internal`. The mapping table is itself a substrate
  // consumer (`ensure_slot` → `substrate_acquire(MappingTable)`), and
  // substrate.acquire's Crystalline read goes through
  // `my_thread()` → `get_thread_scratch()`. If TLS were still nullptr
  // at the call to `register_mapping_internal`, get_thread_scratch
  // would recurse into `create_thread_state` — infinite recursion,
  // stack overflow on the first thread that needed mapping-table
  // expansion. The substrate-bootstrap TLS-swap that used to break
  // this cycle is gone; publishing TLS first lets Crystalline read
  // against the freshly-allocated arena's CrystallineThreadRegion
  // (eager-committed and zero-initialized — a valid "no slots pinned,
  // no batches pending" state). On register failure the rollback
  // clears TLS before page_freeing the arena.

  char *b = reserve_and_commit_arena();
  if (LIBC_UNLIKELY(!b))
    return nullptr;

  auto *state =
      reinterpret_cast<ThreadScratchState *>(b + scratch_detail::CONTROL_OFFSET);
  init_scratch_state(state, b);

  // Publish via TLS before any libc call that might recurse through
  // get_thread_scratch (see ORDERING comment above).
  teb_tls_set(tls_index, state);

  bool pre_init_ready = false;

  if (LIBC_LIKELY(::LIBC_NAMESPACE::windows::g_mapping_table.is_init_ready())) {
    if (LIBC_UNLIKELY(
            !::LIBC_NAMESPACE::windows::g_mapping_table
                 .register_mapping_internal(b, RESERVE_SIZE))) {
      teb_tls_set(tls_index, nullptr);
      ::LIBC_NAMESPACE::internal::page_free(b);
      return nullptr;
    }
  } else {
    // Bring-up window. Take the lock and recheck — the walker's
    // finalize may have latched ready=true between our outer load and
    // the lock acquire, in which case we fall through to the inline
    // register path.
    pending_internal_receipts_lock();
    if (::LIBC_NAMESPACE::windows::g_mapping_table.is_init_ready()) {
      pending_internal_receipts_unlock();
      if (LIBC_UNLIKELY(
              !::LIBC_NAMESPACE::windows::g_mapping_table
                   .register_mapping_internal(b, RESERVE_SIZE))) {
        teb_tls_set(tls_index, nullptr);
        ::LIBC_NAMESPACE::internal::page_free(b);
        return nullptr;
      }
    } else {
      // True pre-INIT_READY. Enqueue the receipt; the walker's
      // `mapping_table_init_fn` harvests it after `ensure_init` returns
      // INIT_READY, and Pass 2 stamps it LIBC_INTERNAL. Inline
      // `register_mapping_internal` would self-deadlock on
      // init_state_.wait(INIT_IN_PROGRESS).
      enqueue_pending_internal_receipt_locked(
          b, scratch_detail::RESERVE_SIZE,
          ::LIBC_NAMESPACE::internal::InternalKind::BootstrapScratch);
      pending_internal_receipts_unlock();
      pre_init_ready = true;
    }
  }

  state->is_pre_init_ready = pre_init_ready ? 1 : 0;
  registry_insert(state);
  return state;
}

// =========================================================================
// scratch_thread_cleanup — direct page_free for primary + every overflow
// =========================================================================

void scratch_thread_cleanup_inner(ThreadScratchState *state) {
  // Phase 1: flush pending Crystalline retire batches into each domain's
  // pool slot chains BEFORE the arena backing disappears. The batch
  // first/last/list pointers thread through CrystallineNode objects the
  // owner allocated within the arena's data region; once the arena is
  // released, those addresses fault. After this call every retire that
  // hasn't already been reaped is published into a domain pool slot
  // chain reachable to live threads.
  scratch_flush_crystalline_batches(state);

  // Phase 2: release each per-(thread × domain) Crystalline slot back
  // to its domain's pool. The release path tombstones the slot to its
  // constructor-init state (inv_ptr in first[], zero epochs/state) so
  // any peer walker iterating the pool sees a fully-inactive slot, then
  // pushes the index onto the Treiber freelist for reuse.
  scratch_release_crystalline_slots(state);

  // Phase 3: drop from per-process registry. ThreadScratch keeps this
  // SLL for its own lifecycle bookkeeping (fork-child VA recovery);
  // Crystalline no longer reads it.
  registry_remove(state);

  // Phase 4: full VA release. Cross-thread Crystalline walks now iterate
  // the per-domain CrystallineSlotPool storage (process-lifetime VA),
  // not the dying thread's arena, so peer threads cannot fault through
  // a freed pointer. Capture `next` before the remove/free pair —
  // `ov` becomes invalid after `page_free`. `g_mapping_table.remove`
  // is a no-op if the entry is absent (defensive for fork-child /
  // exec-hollow paths that reset the table without walking ThreadScratch).
  auto *ov = state->overflow;
  while (ov) {
    auto *next_ov = ov->next;
    ::LIBC_NAMESPACE::windows::g_mapping_table.remove(ov);
    ::LIBC_NAMESPACE::internal::page_free(ov);
    ov = next_ov;
  }
  state->overflow = nullptr;

  // Release the primary arena. `state` itself becomes invalid after
  // `page_free` — it lives at offset 0 of the reservation. Capture the
  // base into a local first.
  void *primary_base = state->base();
  ::LIBC_NAMESPACE::windows::g_mapping_table.remove(primary_base);
  ::LIBC_NAMESPACE::internal::page_free(primary_base);
}

// Out-of-line cleanup callback; address-taken for the TLS callback
// pointer in init_tls_slot. Defined inside scratch_detail so the
// declaration in thread_scratch.h matches by ADL.
void scratch_thread_cleanup(void *val) {
  if (!val)
    return;
  scratch_thread_cleanup_inner(static_cast<ThreadScratchState *>(val));
}

} // namespace scratch_detail

// =========================================================================
// scratch_fork_reinit — direct page_free for dead-thread VA
// =========================================================================
//
// Forwarded from libc_fork_reinit_impl.cpp. Single-threaded post-fork
// by contract — only the forking thread runs in the child. Walks the
// CoW-inherited per-process registry, preserves the surviving thread's
// state, and `page_free`s every dead thread's primary + overflow
// reservations.

void scratch_fork_reinit() {
  // Reset locks. They may have been held by dead threads at the fork
  // snapshot, which would otherwise deadlock the surviving thread.
  scratch_detail::g_scratch_init_lock.store(0, cpp::MemoryOrder::RELAXED);
  scratch_detail::g_scratch_list_lock.store(0, cpp::MemoryOrder::RELAXED);
  // The pending-internal-receipts lock is reachable from any thread
  // that enqueued in the parent during Tier A. Reset it too — a dead
  // pre-fork holder would otherwise freeze the post-fork path
  // indefinitely.
  scratch_detail::g_pending_internal_receipts_count = 0;
  for (uint32_t i = 0; i < scratch_detail::kPendingInternalReceiptsCap; ++i) {
    scratch_detail::g_pending_internal_receipts[i].base = nullptr;
    scratch_detail::g_pending_internal_receipts[i].size = 0;
    scratch_detail::g_pending_internal_receipts[i].kind =
        ::LIBC_NAMESPACE::internal::InternalKind::Unknown;
  }
  scratch_detail::g_pending_internal_receipts_lock.store(
      0, cpp::MemoryOrder::RELAXED);

  DWORD my_tid = NtCurrentThreadId();

  // Identify the survivor by TLS, not by TID. In a fork child the
  // current TID is fresh — distinct from the parent thread's TID at
  // fork time — but copy_parent_teb_state has already CoW-copied the
  // parent's TLS slots into the child's TEB. The slot at our scratch
  // index therefore points to the surviving thread's ThreadScratchState
  // (the entry the parent's forking thread was using). A pure TID match
  // would treat that entry as a dead thread and `page_free` its VA,
  // leaving every later get_thread_scratch() call with a CoW pointer
  // into freed memory.
  DWORD tls_idx = scratch_detail::g_scratch_tls_index.load(
      cpp::MemoryOrder::ACQUIRE);
  ThreadScratchState *my_state = nullptr;
  if (tls_idx != scratch_detail::TLS_UNINITIALIZED)
    my_state = static_cast<ThreadScratchState *>(teb_tls_get(tls_idx));

  ThreadScratchState *node =
      scratch_detail::g_scratch_head.load(cpp::MemoryOrder::RELAXED);
  ThreadScratchState *survivor = nullptr;

  while (node) {
    ThreadScratchState *next = node->next;
    if (node == my_state || node->owner_tid == my_tid) {
      // Surviving thread — preserve. Clear link for rebuilt list.
      // Update owner_tid to the child's fresh TID so subsequent
      // operations key off the live TID. Reinit the ThreadLocalWord
      // (child gets new TIDs, clear park state). Reset free lists;
      // their entries may reference pre-fork state that is stale in
      // the child. The bump pointer stays put so live allocations
      // remain valid; orphaned freelist blocks are unreachable but
      // harmless until the bump rewinds past them.
      node->owner_tid = my_tid;
      node->next = nullptr;
      node->word.fork_reinit();
      node->arena.free_list_head = nullptr;
      for (auto *ov = node->overflow; ov; ov = ov->next)
        ov->arena.free_list_head = nullptr;
      survivor = node;
    } else {
      // Dead thread — release every overflow's VA, then the primary's.
      // mapping_table entries inherit CoW from the parent; `remove`
      // is a no-op when absent so it stays correct under whatever
      // state the parent's mapping table was in at fork.
      auto *ov = node->overflow;
      while (ov) {
        auto *next_ov = ov->next;
        ::LIBC_NAMESPACE::windows::g_mapping_table.remove(ov);
        ::LIBC_NAMESPACE::internal::page_free(ov);
        ov = next_ov;
      }
      void *base = node->base();
      ::LIBC_NAMESPACE::windows::g_mapping_table.remove(base);
      ::LIBC_NAMESPACE::internal::page_free(base);
    }
    node = next;
  }

  scratch_detail::g_scratch_head.store(survivor, cpp::MemoryOrder::RELAXED);
}

static void thread_scratch_fini() {
  DWORD idx = scratch_detail::g_scratch_tls_index.load(
      cpp::MemoryOrder::ACQUIRE);
  if (idx == scratch_detail::TLS_UNINITIALIZED)
    return;
  tls_cleanup_unregister(idx);
  tls_free(idx);
  scratch_detail::g_scratch_tls_index.store(scratch_detail::TLS_UNINITIALIZED,
                                            cpp::MemoryOrder::RELAXED);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_FINI(4, thread_scratch,
                   &::LIBC_NAMESPACE::internal::thread_scratch_fini)

LIBC_REGISTER_FORK_REINIT(scratch,
                          ::LIBC_NAMESPACE::internal::kForkPrioScratch,
                          &::LIBC_NAMESPACE::internal::scratch_fork_reinit)
