//===-- Crystalline-W thread registry — implementation ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See thread_registry.h for the contract; lockfree_hash.h for the
// linear-hashing protocol; thread_registry_node.h for the FreeFn
// dispatch shape.
//
// Concurrency model summary
// -------------------------
// Every public API entry that observes registry state issues exactly
// one Crystalline `read()` to enter; subsequent traversal uses plain
// atomic loads. The reservation captures the entry-time epoch, which
// pins every node retired at era >= that epoch — including nodes
// reached via plain loads during the walk. This works because the
// deregister protocol always removes references BEFORE retiring (the
// BucketEntry is unlinked-and-retired before its `lc` is unlinked-
// and-retired, and the bucket head page is not retired while any
// entry it holds is live). So a node observable from the entry-time
// snapshot has not yet been retired at era < entry, and any retire
// of it is pinned by our reservation.
//
// Reservation indices used:
//   * kReservationLookup — hash table walks (find_by_task_id /
//     resolve), and the bucket walk inside split.
//   * kReservationIter   — iter list walks (for_each, collect_tids,
//     alert_all, fork_reinit).
//   The third Crystalline scratch slot stays unused at the registry
//   level — Crystalline reserves it for help-protocol parent/helpee.
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/thread_registry.h"

#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/threads/windows/lockfree_hash.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/threads/windows/thread_registry_node.h"

namespace LIBC_NAMESPACE_DECL {

// =========================================================================
// Crystalline domain — single domain managing all three retirable kinds.
// =========================================================================

namespace {

constexpr uint32_t kReservationLookup = 0;
constexpr uint32_t kReservationIter = 1;

// Crystalline's amortized epoch-bump cadence. Every Freq-th
// `init_node` (across the whole process for this domain) bumps the
// global epoch. 64 balances per-init cost vs. how aggressively old
// retire batches drain.
constexpr uint32_t kRetireFreq = 64;

// Initial L (log2 of round-base bucket count). 2^4 = 16 buckets at
// startup. Splits grow this monotonically.
constexpr uint32_t kInitialL = 4;

constexpr ACCESS_MASK kThreadHandleAccess =
    THREAD_QUERY_LIMITED_INFORMATION | THREAD_TERMINATE | THREAD_SET_CONTEXT |
    THREAD_GET_CONTEXT | THREAD_SET_INFORMATION | SYNCHRONIZE;

} // anonymous namespace

::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    ThreadRegistryNode, &free_thread_registry_node, kRetireFreq>
    g_registry_domain;

// Crystalline `read()` is templated on the domain's NodeT
// (ThreadRegistryNode here). Our atomics, however, hold pointers to
// the node's subclasses (ThreadLifecycle*, BucketEntry*,
// BucketHeadPage*) — preserving the static type at the storage site
// is much clearer than typing every link as a base-class pointer and
// downcasting on every load.
//
// `cpp::Atomic<T>` is `alignas(...) T val;` — a single trivially-
// copyable storage word — so `Atomic<Subclass*>` and
// `Atomic<ThreadRegistryNode*>` are layout-identical for any subclass
// that derives from ThreadRegistryNode. The reinterpret-cast below
// rebinds the reference so `read()` accepts it; the returned base
// pointer is then static_cast back to the publication type
// (semantically lossless because the only writer to that atomic is a
// site that just-stored the same subclass pointer).
template <typename Subclass>
LIBC_INLINE Subclass *crys_read(cpp::Atomic<Subclass *> &obj,
                                  uint32_t index, ThreadRegistryNode *parent) {
  static_assert(sizeof(cpp::Atomic<Subclass *>) ==
                    sizeof(cpp::Atomic<ThreadRegistryNode *>),
                "subclass and base pointer atomics must be layout-compatible");
  auto &node_atom =
      reinterpret_cast<cpp::Atomic<ThreadRegistryNode *> &>(obj);
  return static_cast<Subclass *>(
      g_registry_domain.read(node_atom, index, parent));
}

// Slab pool backing BucketEntry allocations. One BucketEntry per
// registered thread; tiny (~56 bytes) and short-lived. A page_alloc
// per entry would round up to 4KB and waste two orders of magnitude
// of memory at scale, so we slab them.
//
// Reclamation is FULL — not best-effort. We use the pool's
// pool-managed TLS path (init_tls/tls_alloc/fini_tls):
//   * init_tls() registers a .CRT$XLC cleanup callback that calls
//     `abandon()` on every thread exit. abandon() drains the slab's
//     pending xthread frees, compacts sealed pages, and either
//     `full_release`s the slab (returning its 64KB chunk to the
//     substrate) if the slab is fully empty, or pushes it onto the
//     pool's abandoned-adoption stack so a future allocator can
//     adopt it.
//   * Per-slot frees route through SlabPool::free invoked by the
//     FreeFn dispatcher in `free_thread_registry_node` — page
//     occupancy decrements drive page-level seal/decommit, returning
//     physical memory promptly even from slabs that aren't yet
//     fully empty.
//   * fork_reinit() releases dead-thread slabs that are fully empty
//     and pushes the rest onto abandoned for re-adoption.
//
// Net: slot reuse, page-level physical reclaim, slab-level VA reclaim
// on thread exit and adoption. No leak.
internal::SlabPool g_bucket_entry_pool;

void free_bucket_entry_slab(BucketEntry *e) {
  if (e)
    internal::SlabPool::free(e);
}

namespace {

LIBC_INLINE ThreadRegistryState &state() { return g_pcb.thread_registry; }

LIBC_INLINE cpp::Atomic<BucketHeadPage *> *top_level_array() {
  return reinterpret_cast<cpp::Atomic<BucketHeadPage *> *>(
      state().top_level_base.load(cpp::MemoryOrder::ACQUIRE));
}

LIBC_INLINE cpp::Atomic<ThreadLifecycle *> &iter_head_atomic() {
  return reinterpret_cast<cpp::Atomic<ThreadLifecycle *> &>(state().iter_head);
}

// =========================================================================
// Lazy init
// =========================================================================
//
// First entry through `registry_ensure_initialized` allocates the
// 32KB top-level array, registers the Crystalline domain, and
// publishes `init_state == 2`. Concurrent callers spin/wait via a
// futex on `init_state` — the winner's RELEASE store is visible
// after the wake.
//
// On allocation failure, the winner stores 0 back to `init_state`
// and wakes waiters; everyone retries.

bool ensure_initialized_slow();

LIBC_INLINE bool ensure_initialized() {
  if (LIBC_LIKELY(state().init_state.load(cpp::MemoryOrder::ACQUIRE) == 2))
    return true;
  return ensure_initialized_slow();
}

bool ensure_initialized_slow() {
  for (;;) {
    uint32_t cur = state().init_state.load(cpp::MemoryOrder::ACQUIRE);
    if (cur == 2)
      return true;

    if (cur == 1) {
      // Another thread is initializing — wait, then retry. The winner
      // is contractually obligated to publish either 2 (success) or 0
      // (rolled back) before returning, and to wake every waiter on
      // either transition. If neither path runs (e.g., winner trapped),
      // every other thread spinning here would wedge — futex_addr::wait
      // wakes only on a real value change. Watchdog: trap any unknown
      // state value here so a corrupt or future state-machine variant
      // surfaces immediately rather than producing a silent hang.
      futex_addr::wait<uint32_t>(
          reinterpret_cast<const volatile uint32_t *>(&state().init_state), 1,
          nullptr);
      continue;
    }

    if (cur != 0) {
      // Unknown init_state value. Trap rather than silently spin.
      __builtin_trap();
    }

    // cur == 0: try to claim init.
    uint32_t expected = 0;
    if (!state().init_state.compare_exchange_strong(
            expected, 1, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE))
      continue;

    // We won. Allocate the top-level array (32KB).
    constexpr size_t kTopLevelBytes =
        kTopLevelCapacity * sizeof(cpp::Atomic<BucketHeadPage *>);
    void *top = internal::page_alloc(kTopLevelBytes);
    if (!top) {
      state().init_state.store(0, cpp::MemoryOrder::RELEASE);
      futex_addr::wake(
          reinterpret_cast<const volatile uint32_t *>(&state().init_state),
          INT32_MAX);
      return false;
    }
    __builtin_memset(top, 0, kTopLevelBytes);

    // Register the Crystalline domain. Idempotency NOT provided by
    // CrystallineDomain — we're under the init_state CAS so this is
    // necessarily one-shot.
    g_registry_domain.init_registration();

    // Bring up the BucketEntry slab pool. Slot size = sizeof(BucketEntry)
    // (~56 bytes); slabs hold many entries each, so amortized memory
    // is proportional to live entry count rather than per-entry page
    // rounding. `init_tls` allocates a TEB slot and registers the
    // .CRT$XLC cleanup callback that calls `abandon()` on thread exit
    // — this is what makes slab-VA reclamation actually fire on every
    // thread exit, not just on fork-reinit or process teardown.
    g_bucket_entry_pool.init(sizeof(BucketEntry), alignof(BucketEntry));
    // Phase Allocator: BucketEntry slabs back the registry's per-task
    // hash entries. Lifecycle teardown retires entries through the slab,
    // so the slab's TLS abandon must run AFTER lifecycle_cleanup
    // (Allocator < Lifecycle in the descending teardown walk).
    g_bucket_entry_pool.init_tls(internal::kTlsCleanupPhaseAllocator);

    // Publish.
    state().top_level_base.store(reinterpret_cast<uintptr_t>(top),
                                 cpp::MemoryOrder::RELEASE);
    state().ls_state.store(encode_hash_state(kInitialL, 0, false),
                           cpp::MemoryOrder::RELAXED);
    state().live_count.store(0, cpp::MemoryOrder::RELAXED);
    state().split_advisory.store(0, cpp::MemoryOrder::RELAXED);
    state().iter_head.store(0, cpp::MemoryOrder::RELAXED);

    state().init_state.store(2, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(
        reinterpret_cast<const volatile uint32_t *>(&state().init_state),
        INT32_MAX);
    return true;
  }
}

// =========================================================================
// Bucket head page management
// =========================================================================
//
// Pages are demand-allocated as the bucket index space grows during
// splits. The splitter calls `ensure_bucket_page` for the bucket
// that's about to receive migrated entries; on first need for a
// page index it allocates and CAS-publishes into the top-level
// array. Concurrent ensure callers race; loser frees its alloc.

BucketHeadPage *alloc_bucket_head_page(uint32_t page_index) {
  void *mem = internal::page_alloc(sizeof(BucketHeadPage));
  if (!mem)
    return nullptr;
  __builtin_memset(mem, 0, sizeof(BucketHeadPage));
  auto *page = static_cast<BucketHeadPage *>(mem);
  page->kind = ThreadRegistryNodeKind::BucketHeadPage;
  page->page_index = page_index;
  // init_node stamps birth_epoch so future retire's batch-anchor min
  // computation has a meaningful starting point.
  g_registry_domain.init_node(page);
  return page;
}

// Ensure the top-level entry at `page_index` holds a valid
// `BucketHeadPage*`. Returns the page, or nullptr on OOM. Crystalline-
// retired pages from a prior fork would never appear here — fork-
// reinit zeroes the array.
BucketHeadPage *ensure_bucket_page(uint32_t page_index) {
  auto *arr = top_level_array();
  if (!arr)
    return nullptr;

  BucketHeadPage *cur =
      arr[page_index].load(cpp::MemoryOrder::ACQUIRE);
  if (cur)
    return cur;

  BucketHeadPage *fresh = alloc_bucket_head_page(page_index);
  if (!fresh)
    return nullptr;

  BucketHeadPage *expected = nullptr;
  if (arr[page_index].compare_exchange_strong(expected, fresh,
                                              cpp::MemoryOrder::ACQ_REL,
                                              cpp::MemoryOrder::ACQUIRE)) {
    return fresh;
  }

  // Lost the race. Free our alloc — never published, no Crystalline
  // protection needed; just hand the memory back to the page allocator.
  internal::page_free(fresh);
  return expected;
}

// Crystalline-protected read of the top-level entry. Returns nullptr
// for unallocated entries.
LIBC_INLINE BucketHeadPage *load_bucket_page(uint32_t page_index) {
  if (page_index >= kTopLevelCapacity)
    return nullptr;
  auto *arr = top_level_array();
  if (!arr)
    return nullptr;
  return crys_read(arr[page_index], kReservationLookup, nullptr);
}

// =========================================================================
// BucketEntry allocation
// =========================================================================
//
// One entry per registered thread, slab-allocated for memory density
// (~56 bytes per entry vs. 4KB if we page-allocated).
//
// Uses the pool's pool-managed TLS path (`tls_alloc`): the TEB slot
// caches the thread's current slab, and the .CRT$XLC cleanup callback
// registered by `init_tls()` calls `abandon()` on thread exit — that's
// what drives slab-level VA reclamation when a thread terminates.
//
// Per-slot reclamation: Crystalline retire submits the entry; FreeFn
// calls `free_bucket_entry_slab` (above), which returns the slot to
// the pool via `SlabPool::free`. The slot may then be re-handed-out
// by any thread on its next alloc.

BucketEntry *alloc_bucket_entry(uint32_t task_id, ThreadLifecycle *lc) {
  void *slot = g_bucket_entry_pool.tls_alloc();
  if (!slot)
    return nullptr;
  auto *entry = static_cast<BucketEntry *>(slot);
  __builtin_memset(entry, 0, sizeof(BucketEntry));
  entry->kind = ThreadRegistryNodeKind::BucketEntry;
  entry->task_id = task_id;
  entry->lc = lc;
  g_registry_domain.init_node(entry);
  return entry;
}

// =========================================================================
// Hash table — lookup, insert, delete
// =========================================================================

// Walk a bucket chain looking for `task_id`. Physically unlinks marked
// predecessors as it goes (Harris discipline) so dead nodes do not
// accumulate in the chain — restores the "deregister removes references
// BEFORE retiring" invariant the transitive-pinning argument depends on.
//
// Walker steps:
//   * Track `pred_next` — the storage pointing at the current node.
//   * If cur is marked: CAS pred_next past it. On CAS-fail, re-read
//     *pred_next at the SAME position and continue from there. The
//     re-read either yields a different node (concurrent splicer
//     removed cur, or a concurrent prepend put a new node here) — we
//     advance to it; or the same cur (CAS lost a benign weak failure)
//     — we retry one more time.
//   * Critically: NEVER restart from head. A heavily-contended chain
//     (many concurrent thread creations doing harris_prepend) would
//     cause restart-from-head to livelock as each CAS at the head
//     fails against the latest prepend. Re-reading at pred_next
//     advances strictly forward as the chain mutates.
//
// Plain loads after the initial Crystalline-protected head read are
// safe under the existing reservation: any node reachable when we
// pinned the head has not been retired before our era.
LIBC_INLINE ThreadLifecycle *
walk_bucket_for_task_id(BucketHeadPage *page, uint32_t in_idx,
                        uint32_t task_id) {
  cpp::Atomic<BucketEntry *> *pred_next = &page->heads[in_idx];
  BucketEntry *cur =
      crys_read(*pred_next, kReservationLookup, page);
  cur = harris_unmark(cur);
  while (cur) {
    BucketEntry *next_raw = cur->next.load(cpp::MemoryOrder::ACQUIRE);
    bool marked = harris_is_marked(next_raw);
    BucketEntry *next_unmarked = harris_unmark(next_raw);

    if (marked) {
      // Splice cur out of the chain physically. CAS pred_next from
      // cur to its unmarked successor. On CAS-fail, the chain mutated
      // — re-read at the same pred_next and continue. Either:
      //   (a) someone else spliced cur (new value != cur): advance.
      //   (b) someone prepended at this slot (new value != cur but
      //       new node points at our region): advance, that node will
      //       be processed normally.
      //   (c) weak CAS spurious failure (new value still == cur):
      //       loop and retry the splice.
      // No restart-from-head — strict forward progress avoids the
      // livelock of CAS-fail-storm under concurrent prepend.
      if (!harris_try_unlink(*pred_next, cur, next_unmarked)) {
        BucketEntry *reread = harris_unmark(
            pred_next->load(cpp::MemoryOrder::ACQUIRE));
        cur = reread;
        continue;
      }
      cur = next_unmarked;
      continue;
    }

    if (cur->task_id == task_id) {
      // Unmarked entry matching the key; cur->lc is pinned by our
      // reservation (transitive Crystalline pinning).
      return cur->lc;
    }
    pred_next = &cur->next;
    cur = next_unmarked;
  }
  return nullptr;
}

// Linear-hash lookup with split-pending awareness. See lockfree_hash.h
// for the protocol summary.
ThreadLifecycle *find_by_task_id_internal(uint32_t task_id) {
  if (!ensure_initialized())
    return nullptr;

  uint64_t raw = state().ls_state.load(cpp::MemoryOrder::ACQUIRE);
  HashState st = decode_hash_state(raw);
  HashedBucket hb = hash_for_lookup(task_id, st);

  auto walk_one = [&](uint32_t b) -> ThreadLifecycle * {
    BucketLocation loc = locate_bucket(b);
    BucketHeadPage *page = load_bucket_page(loc.page_index);
    if (!page)
      return nullptr;
    return walk_bucket_for_task_id(page, loc.in_page_index, task_id);
  };

  ThreadLifecycle *found = walk_one(hb.bucket);
  if (found)
    return found;

  if (hb.is_splitting_target) {
    // Mid-split: probe the other half too.
    return walk_one(hb.bucket + (1u << st.L));
  }
  return nullptr;
}

// Insert a freshly-allocated bucket entry into its appropriate
// bucket (using the insert-time hash, which lands in the FINAL
// bucket if a split is in progress).
bool insert_entry_into_hash(BucketEntry *entry) {
  uint64_t raw = state().ls_state.load(cpp::MemoryOrder::ACQUIRE);
  HashState st = decode_hash_state(raw);
  uint32_t b = hash_for_insert(entry->task_id, st);
  BucketLocation loc = locate_bucket(b);
  BucketHeadPage *page = ensure_bucket_page(loc.page_index);
  if (!page)
    return false;
  harris_prepend(page->heads[loc.in_page_index], entry);
  return true;
}

// Mark+unlink the BucketEntry referencing `lc` from its bucket chain
// AND retire it via Crystalline. Coordinates with concurrent splitter
// migration through a CAS on `lc->bucket_entry` so neither side
// double-retires the same node.
//
// Returns true if THIS call observably claimed the entry (i.e., this
// caller owns the live_count decrement). Returns false if a concurrent
// split-migration had already swapped lc->bucket_entry to a fresh
// entry (in which case the splitter retires the old one and we have
// nothing to do — the deregister has already been observed by every
// reader because lc itself is about to be unlinked from the iter list
// and retired by the calling chain).
//
// Concurrency contract:
//   * registry_register publishes either a real entry or the
//     registration-pending sentinel into lc->bucket_entry. We never
//     observe nullptr here unless someone else already deregistered.
//   * The splitter's CAS-pair on lc->bucket_entry (entry → fresh) and
//     ours (entry → nullptr) are mutually exclusive on the same expected
//     value; whichever wins owns the retire of `entry`.
bool unlink_bucket_entry_for(ThreadLifecycle *lc) {
  // CAS-pair: claim ownership of the specific BucketEntry currently
  // pointed at by lc->bucket_entry. If a splitter or another deregister
  // mutates it concurrently, our CAS fails and we cooperate with their
  // retire.
  BucketEntry *target =
      lc->bucket_entry.load(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    if (!target)
      return false; // Already deregistered.
    if (is_registration_pending(target)) {
      // Registration was abandoned (OOM rollback) before publishing.
      // The registrar will reset bucket_entry to nullptr; no entry to
      // unlink. Treat as "no work" — the caller should not decrement
      // live_count because no successful registration was completed.
      return false;
    }
    BucketEntry *expected = target;
    if (lc->bucket_entry.compare_exchange_weak(
            expected, nullptr, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE)) {
      break; // We own `target` exclusively now.
    }
    target = expected; // Retry with whatever the splitter just installed.
  }

  // Mark target's next as logically deleted (Harris). Idempotent — if
  // a splitter previously set the mark while migrating, we observe it
  // already-marked and skip.
  for (;;) {
    BucketEntry *next_raw =
        target->next.load(cpp::MemoryOrder::ACQUIRE);
    if (harris_is_marked(next_raw))
      break;
    BucketEntry *next_unmarked = harris_unmark(next_raw);
    if (harris_try_mark(target->next, next_unmarked))
      break;
  }

  // Physically unlink target from its bucket. Convergent: if our CAS
  // races a concurrent insert/split, walk_bucket_for_task_id's
  // physical-unlink discipline (H3) splices past the marked node on
  // the next lookup that traverses the chain. The mark we set above
  // is the structural deletion publish — physical unlink is just
  // chain compaction. Retire below is unconditional and one-shot
  // because we exclusively own `target` via the CAS-pair on
  // lc->bucket_entry.
  uint64_t raw = state().ls_state.load(cpp::MemoryOrder::ACQUIRE);
  HashState st = decode_hash_state(raw);
  HashedBucket hb = hash_for_lookup(target->task_id, st);

  auto try_unlink_in_bucket = [&](uint32_t b) -> bool {
    BucketLocation loc = locate_bucket(b);
    auto *arr = top_level_array();
    if (!arr)
      return false;
    BucketHeadPage *page =
        arr[loc.page_index].load(cpp::MemoryOrder::ACQUIRE);
    if (!page)
      return false;

    BucketEntry *target_next_unmarked = harris_unmark(
        target->next.load(cpp::MemoryOrder::ACQUIRE));

    BucketEntry *head =
        page->heads[loc.in_page_index].load(cpp::MemoryOrder::ACQUIRE);
    if (harris_unmark(head) == target) {
      if (harris_try_unlink(page->heads[loc.in_page_index], target,
                            target_next_unmarked))
        return true;
    }

    BucketEntry *cur = harris_unmark(
        page->heads[loc.in_page_index].load(cpp::MemoryOrder::ACQUIRE));
    while (cur && cur != target) {
      BucketEntry *next = harris_unmark(
          cur->next.load(cpp::MemoryOrder::ACQUIRE));
      if (next == target) {
        if (harris_try_unlink(cur->next, target, target_next_unmarked))
          return true;
        cur = harris_unmark(
            page->heads[loc.in_page_index].load(cpp::MemoryOrder::ACQUIRE));
        continue;
      }
      cur = next;
    }
    return false;
  };

  // Convergent unlink: try the primary bucket and (if mid-split) the
  // peer half. Result intentionally unused — convergence is provided
  // by H3's walker fix-up. The mark above is the structural deletion
  // publish.
  (void)try_unlink_in_bucket(hb.bucket);
  if (hb.is_splitting_target)
    (void)try_unlink_in_bucket(hb.bucket + (1u << st.L));

  g_registry_domain.retire(target);
  return true;
}

// =========================================================================
// Iter list (Harris on next_iter, single chain)
// =========================================================================

// Out-of-line definition of harris_prepend_iter. Lives here because
// only this TU instantiates it.
LIBC_INLINE void
harris_prepend_iter(cpp::Atomic<ThreadLifecycle *> &head,
                    ThreadLifecycle *entry,
                    cpp::Atomic<ThreadLifecycle *> &entry_next_field) {
  for (;;) {
    ThreadLifecycle *cur_head = head.load(cpp::MemoryOrder::ACQUIRE);
    cur_head = harris_unmark(cur_head);
    entry_next_field.store(cur_head, cpp::MemoryOrder::RELAXED);
    if (head.compare_exchange_weak(cur_head, entry,
                                    cpp::MemoryOrder::RELEASE,
                                    cpp::MemoryOrder::RELAXED))
      return;
  }
}

void iter_prepend(ThreadLifecycle *lc) {
  harris_prepend_iter(iter_head_atomic(), lc, lc->next_iter);
}

// Mark + unlink `lc` from the iter chain. `lc` is later retired by
// the caller after all unlink work (BucketEntry + iter) has settled.
void iter_mark_unlink(ThreadLifecycle *lc) {
  // Mark.
  for (;;) {
    ThreadLifecycle *next_raw =
        lc->next_iter.load(cpp::MemoryOrder::ACQUIRE);
    if (harris_is_marked(next_raw))
      break;
    ThreadLifecycle *next_unmarked = harris_unmark(next_raw);
    if (harris_try_mark_iter(lc->next_iter, next_unmarked))
      break;
  }

  ThreadLifecycle *target_next_unmarked =
      harris_unmark(lc->next_iter.load(cpp::MemoryOrder::ACQUIRE));

  // Unlink: try head, then walk for predecessor.
  for (;;) {
    ThreadLifecycle *head = harris_unmark(
        iter_head_atomic().load(cpp::MemoryOrder::ACQUIRE));
    if (head == lc) {
      if (harris_try_unlink_iter(iter_head_atomic(), lc,
                                  target_next_unmarked))
        return;
      continue;
    }
    if (!head)
      return; // Shouldn't happen — lc was inserted; defensive.

    ThreadLifecycle *cur = head;
    while (cur && cur != lc) {
      ThreadLifecycle *next = harris_unmark(
          cur->next_iter.load(cpp::MemoryOrder::ACQUIRE));
      if (next == lc) {
        if (harris_try_unlink_iter(cur->next_iter, lc,
                                    target_next_unmarked))
          return;
        // Retry from head.
        break;
      }
      cur = next;
    }
    if (!cur)
      return; // Walked past tail — lc not in chain (already unlinked).
  }
}

// =========================================================================
// Linear-hash split
// =========================================================================
//
// One splitter at a time, claimed via the `split_pending` bit. The
// splitter walks bucket S, migrates each entry whose `task_id`'s
// bit-L is set into bucket S' = S + 2^L, then publishes (L, S+1)
// (or (L+1, 0) on round completion).
//
// New inserts during the split go directly to the final bucket
// (lookup hash uses L+1 bits when split_pending && L_bits == S),
// so the migration only touches PRE-EXISTING entries.

// Migrate one entry from bucket S into bucket S'. Returns true if
// the entry was eligible (bit L == 1 of task_id) and was either
// migrated by us or by a concurrent helper.
bool migrate_one_entry(BucketEntry *entry, uint32_t L,
                       BucketHeadPage *src_page, uint32_t src_in_idx,
                       BucketHeadPage *dst_page, uint32_t dst_in_idx) {
  uint32_t bit_L = (entry->task_id >> L) & 1u;
  if (bit_L == 0)
    return false; // Stays in bucket S.

  // Allocate a new entry mirroring the old.
  BucketEntry *fresh = alloc_bucket_entry(entry->task_id, entry->lc);
  if (!fresh)
    return false; // OOM — leave the entry in bucket S; subsequent
                  // splits or recovery will retry.

  // CAS-publish: update lc->bucket_entry from old to fresh under a
  // strong CAS. If `entry` no longer matches lc->bucket_entry,
  // someone else handled it (concurrent dereg, prior split, etc.)
  // — free `fresh` and bail.
  ThreadLifecycle *lc = entry->lc;
  BucketEntry *expected = entry;
  if (!lc->bucket_entry.compare_exchange_strong(
          expected, fresh, cpp::MemoryOrder::ACQ_REL,
          cpp::MemoryOrder::ACQUIRE)) {
    // `fresh` was never published — release back to the SLAB pool
    // (alloc_bucket_entry is slab-backed via g_bucket_entry_pool).
    // Calling page_free on a slab slot would corrupt slab bookkeeping
    // and is a latent crash/hang vector.
    free_bucket_entry_slab(fresh);
    return false;
  }

  // Insert fresh into destination bucket.
  harris_prepend(dst_page->heads[dst_in_idx], fresh);

  // Mark + unlink old from source bucket.
  for (;;) {
    BucketEntry *next_raw =
        entry->next.load(cpp::MemoryOrder::ACQUIRE);
    if (harris_is_marked(next_raw))
      break;
    BucketEntry *next_unmarked = harris_unmark(next_raw);
    if (harris_try_mark(entry->next, next_unmarked))
      break;
  }
  BucketEntry *old_next_unmarked =
      harris_unmark(entry->next.load(cpp::MemoryOrder::ACQUIRE));

  // Find predecessor in src bucket and CAS-unlink. Best-effort —
  // if a concurrent op moved entry, our subsequent retire still
  // reclaims correctly.
  for (;;) {
    BucketEntry *head = harris_unmark(
        src_page->heads[src_in_idx].load(cpp::MemoryOrder::ACQUIRE));
    if (head == entry) {
      if (harris_try_unlink(src_page->heads[src_in_idx], entry,
                            old_next_unmarked))
        break;
      continue;
    }
    BucketEntry *cur = head;
    bool retry = false;
    while (cur && cur != entry) {
      BucketEntry *next = harris_unmark(
          cur->next.load(cpp::MemoryOrder::ACQUIRE));
      if (next == entry) {
        if (harris_try_unlink(cur->next, entry, old_next_unmarked))
          break;
        retry = true;
        break;
      }
      cur = next;
    }
    if (retry)
      continue;
    break;
  }

  g_registry_domain.retire(entry);
  return true;
}

// Try to split bucket S. Caller holds (or should attempt to claim)
// the `split_pending` bit. Returns whether a split was performed.
bool try_perform_split() {
  uint64_t raw = state().ls_state.load(cpp::MemoryOrder::ACQUIRE);
  HashState st = decode_hash_state(raw);
  if (st.split_pending)
    return false; // Another splitter active.

  // Decide if we even need a split.
  uint32_t live = state().live_count.load(cpp::MemoryOrder::RELAXED);
  uint32_t buckets = bucket_count_at(st.L, st.S);
  if (live <= kTargetLoadFactor * buckets)
    return false;

  // CAS-claim: flip split_pending false → true at the same (L, S).
  uint64_t claimed = encode_hash_state(st.L, st.S, true);
  if (!state().ls_state.compare_exchange_strong(
          raw, claimed, cpp::MemoryOrder::ACQ_REL,
          cpp::MemoryOrder::ACQUIRE))
    return false; // Lost claim race.

  uint32_t L = st.L;
  uint32_t S = st.S;
  uint32_t S_prime = S + (1u << L);

  BucketLocation src = locate_bucket(S);
  BucketLocation dst = locate_bucket(S_prime);

  BucketHeadPage *src_page = ensure_bucket_page(src.page_index);
  BucketHeadPage *dst_page = ensure_bucket_page(dst.page_index);
  if (!src_page || !dst_page) {
    // OOM — release the claim without advancing.
    state().ls_state.store(encode_hash_state(L, S, false),
                           cpp::MemoryOrder::RELEASE);
    return false;
  }

  // Walk bucket S. For each pre-existing entry with bit-L == 1,
  // migrate to bucket S'.
  //
  // We loop walking from head; the migration-step CAS-prepends
  // the new entry into S' (so its predecessor is the new head)
  // and CAS-unlinks the old. After one migration the old entry
  // is gone from S, so the next walk starting from head sees a
  // shorter chain. New inserts during the walk land in S or S'
  // directly via the L+1-bit insert hash (split_pending is set),
  // so the splitter doesn't migrate them.
  for (;;) {
    BucketEntry *head =
        crys_read(src_page->heads[src.in_page_index],
                  kReservationLookup, src_page);
    head = harris_unmark(head);
    BucketEntry *cur = head;
    bool migrated_any = false;
    while (cur) {
      BucketEntry *next_raw =
          cur->next.load(cpp::MemoryOrder::ACQUIRE);
      bool marked = harris_is_marked(next_raw);
      BucketEntry *next = harris_unmark(next_raw);
      if (!marked) {
        if (migrate_one_entry(cur, L, src_page, src.in_page_index, dst_page,
                              dst.in_page_index)) {
          migrated_any = true;
          break; // Restart walk from head.
        }
      }
      cur = next;
    }
    if (!migrated_any)
      break;
  }

  // Publish: advance (L, S+1, false) or round over to (L+1, 0, false).
  uint32_t new_S = S + 1;
  uint32_t new_L = L;
  if (new_S == (1u << L)) {
    new_L = L + 1;
    new_S = 0;
  }
  state().ls_state.store(encode_hash_state(new_L, new_S, false),
                          cpp::MemoryOrder::RELEASE);
  return true;
}

LIBC_INLINE bool duplicate_thread_handle(HANDLE source, HANDLE *out) {
  NTSTATUS st = ::NtDuplicateObject(NtCurrentProcess(), source,
                                    NtCurrentProcess(), out,
                                    kThreadHandleAccess, 0, 0);
  return NT_SUCCESS(st);
}

// Try to acquire the registration claim on `lc->bucket_entry` (sets
// the sentinel). If the slot already holds a real entry from a previous
// successful registration, returns kIdempotentMatch (caller should
// verify identity and return true). If the slot was claimed by another
// caller, this routine spins on the cache line via UMWAIT/MWAITX/WFE
// until that caller publishes a real entry or rolls back to nullptr.
enum class ClaimOutcome : uint8_t {
  kClaimed,         // We installed the sentinel; we own publish.
  kIdempotentMatch, // Real entry already present; caller short-circuits.
};

// Thread-local "currently registering this lc" marker. Used only as a
// self-reentrancy guard in `acquire_registration_claim` — if the same
// thread re-enters registration for the same `lc` while already
// holding its sentinel, it would spin on its own publish and
// deadlock. Today no path triggers this (init_node, the publish path,
// and NtDuplicateObject are pure / non-alertable), but the cost of
// this trap is one cache-line read on the claim hot path. Catching a
// future regression at the registration-time trap is worth the cost.
thread_local ThreadLifecycle *t_registering_self = nullptr;

LIBC_INLINE ClaimOutcome
acquire_registration_claim(ThreadLifecycle *lc) {
  // Self-reentrancy guard. A thread cannot re-enter
  // registry_register* on an `lc` whose sentinel it already holds —
  // the spin would wait for its own publish forever. Trap immediately
  // so the regression surfaces at the offending call, not as a hang.
  if (t_registering_self == lc)
    __builtin_trap();
  BucketEntry *expected = nullptr;
  for (;;) {
    if (lc->bucket_entry.compare_exchange_weak(
            expected, registration_pending_sentinel(),
            cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE)) {
      return ClaimOutcome::kClaimed;
    }
    if (!expected) {
      continue; // weak CAS spurious failure
    }
    if (is_registration_pending(expected)) {
      // Hardware-monitor spin on the slot until the installer
      // publishes a real entry or rolls back to null. spin_on_raw
      // arms UMWAIT / MWAITX / WFE on the cache line so the wake is
      // immediate on the winner's store — no PAUSE busy-loop, no
      // syscall.
      //
      // Spin is intentionally unbounded. A time-bound recovery
      // (CAS sentinel→null, re-claim) would re-introduce the C1
      // race: a slow-but-alive installer would later overwrite our
      // recovered claim and we'd both run publish_registration on
      // the same lc — cyclic iter list, double live_count, leaked
      // entry. The sentinel-stuck scenario requires an installer to
      // die between sentinel-install and publish/rollback; in our
      // flow that requires an external NtTerminateThread or a fault
      // on a code path that has no faulting operations (pure
      // atomics + NtDuplicateObject which returns NTSTATUS, doesn't
      // fault). If a thread is being killed mid-registration the
      // process is in an unrecoverable state already.
      //
      // CREATE_SUSPENDED in Thread::run guarantees the parent fully
      // publishes before the child runs, so the child's
      // self-register sees a real entry (kIdempotentMatch) without
      // ever entering this spin. Foreign-thread paths in
      // signal_state allocate a fresh lc per caller, so no two
      // registrars share an lc. This loop is unreachable on every
      // normal path; it exists for defense-in-depth.
      while (is_registration_pending(
          lc->bucket_entry.load(cpp::MemoryOrder::ACQUIRE))) {
        spin_wait::spin_on_raw(&lc->bucket_entry.val,
                                registration_pending_sentinel());
      }
      expected = lc->bucket_entry.load(cpp::MemoryOrder::ACQUIRE);
      if (!expected)
        continue; // rolled back; retry our claim
    }
    return ClaimOutcome::kIdempotentMatch;
  }
}

// Common publish path: install `entry` into the hash, replace the
// sentinel in lc->bucket_entry with it, then prepend lc to the iter
// list. lc->task_id and lc->tid must already be set; lc->thread_handle
// must already hold the (caller-owned) handle. Returns false on OOM
// in the page-allocation path of the hash insert; on failure rolls the
// sentinel back to nullptr and returns the entry to the caller (so a
// caller that pre-allocated the entry can release the slab slot).
bool publish_registration(ThreadLifecycle *lc, BucketEntry *entry) {
  if (!insert_entry_into_hash(entry)) {
    lc->bucket_entry.store(nullptr, cpp::MemoryOrder::RELEASE);
    return false;
  }
  // Replace sentinel with the real entry. This unblocks any spinning
  // concurrent registrar — they observe a real entry and short-circuit
  // with the identity-match return.
  lc->bucket_entry.store(entry, cpp::MemoryOrder::RELEASE);

  // Now publish lc into the iter list. Visitors observing lc here can
  // resolve its task_id through the hash because the entry is already
  // searchable (M4 publish ordering).
  iter_prepend(lc);

  state().live_count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);

  // Best-effort split trigger. Single-splitter via the split_pending
  // CAS. Failures are silent.
  (void)try_perform_split();
  return true;
}

} // anonymous namespace

// =========================================================================
// Public API: pre-allocation helpers (H2)
// =========================================================================

BucketEntry *registry_alloc_bucket_entry_for(ThreadLifecycle *lc) {
  if (!lc)
    return nullptr;
  if (!ensure_initialized())
    return nullptr;
  if (lc->task_id == 0)
    lc->task_id = allocate_task_id();
  return alloc_bucket_entry(lc->task_id, lc);
}

void registry_release_unused_entry(BucketEntry *entry) {
  // Entry never published / Crystalline-init_node'd as live; just hand
  // back to the slab. (init_node sets birth_epoch and batch_link=null,
  // making it Crystalline-trackable, but no thread holds a reservation
  // pinning it so direct slab return is safe.)
  if (entry)
    free_bucket_entry_slab(entry);
}

// =========================================================================
// Public API: registration
// =========================================================================
//
// Resolves the parent-pre-register vs child-self-register race via a
// CAS-claim on `lc->bucket_entry`:
//
//   1. CAS bucket_entry from nullptr to `registration_pending_sentinel()`.
//      The CAS winner owns this registration; losers spin on the cache
//      line via spin_on_raw until the winner publishes a real entry
//      (success) or rolls back to nullptr (OOM), then verify identity
//      and return success/failure accordingly.
//   2. Publish bucket entry into the hash table.
//   3. CAS-replace the sentinel with the real entry pointer.
//   4. Prepend lc onto the iter list. Iter-visible implies hash-visible
//      (M4) because the entry is already searchable.
//   5. Bump live_count.
//
// `live_count` is incremented exactly once per successful registration
// and `unlink_bucket_entry_for` returns whether THIS deregister did the
// physical claim, so the matching decrement in registry_deregister
// happens at most once per register (H7).

bool registry_register_with_entry(ThreadLifecycle *lc, BucketEntry *entry,
                                  HANDLE thread_handle, uint32_t tid,
                                  bool duplicate_handle) {
  if (!lc || !entry)
    return false;
  if (!ensure_initialized())
    return false;

  ClaimOutcome outcome = acquire_registration_claim(lc);
  if (outcome == ClaimOutcome::kIdempotentMatch) {
    // Existing registration with matching identity (race winner did
    // the publish). Caller's pre-allocated entry is unused; release it.
    bool ok = (lc->task_id != 0 && lc->tid == tid);
    free_bucket_entry_slab(entry);
    return ok;
  }

  // We hold the claim; bucket_entry == sentinel. Arm the self-
  // reentrancy guard for the duration of the publish path. Any
  // re-entry from this thread into registry_register* on the same
  // lc — through a stray APC, a fault filter, or a future change —
  // will trap at the top of acquire_registration_claim instead of
  // spinning on its own publish.
  t_registering_self = lc;

  // Any failure path below MUST roll the sentinel back to nullptr to
  // unblock spinning losers AND clear t_registering_self.
  //
  // Stamp birth_epoch on lc so Crystalline retire's anchor batch-min
  // computation is meaningful — lifecycles are retirable nodes too,
  // and prior to this fix were retired with a zero birth_epoch which
  // made every reservation's epoch-window pin them spuriously.
  g_registry_domain.init_node(lc);

  lc->tid = tid;

  HANDLE owned_handle = thread_handle;
  if (duplicate_handle) {
    if (!duplicate_thread_handle(thread_handle, &owned_handle)) {
      lc->bucket_entry.store(nullptr, cpp::MemoryOrder::RELEASE);
      t_registering_self = nullptr;
      free_bucket_entry_slab(entry);
      return false;
    }
  }
  // thread_handle store: RELEASE so cross-thread borrowers observing
  // a non-null handle also observe lc->task_id and lc->tid set.
  lc->thread_handle.store(owned_handle, cpp::MemoryOrder::RELEASE);

  if (!publish_registration(lc, entry)) {
    if (duplicate_handle && owned_handle)
      ::NtClose(owned_handle);
    lc->thread_handle.store(nullptr, cpp::MemoryOrder::RELAXED);
    t_registering_self = nullptr;
    free_bucket_entry_slab(entry);
    return false;
  }
  // Disarm the self-reentrancy guard. publish_registration has stored
  // a real entry into lc->bucket_entry; any future re-registration on
  // this lc observes a real entry and short-circuits via
  // kIdempotentMatch — no spin window remains where re-entry could
  // self-deadlock.
  t_registering_self = nullptr;
  return true;
}

bool registry_register(ThreadLifecycle *lc, HANDLE thread_handle, uint32_t tid,
                       bool duplicate_handle) {
  // Internal-alloc fallback (used by self-register; the parent path
  // pre-allocates via registry_register_with_entry to make OOM
  // recoverable BEFORE NtCreateThreadEx).
  if (!lc)
    return false;
  if (!ensure_initialized())
    return false;
  if (lc->task_id == 0)
    lc->task_id = allocate_task_id();
  BucketEntry *entry = alloc_bucket_entry(lc->task_id, lc);
  if (!entry)
    return false;
  return registry_register_with_entry(lc, entry, thread_handle, tid,
                                       duplicate_handle);
}

bool registry_register_self(ThreadLifecycle *lc) {
  return registry_register(lc, NtCurrentThread(), NtCurrentThreadId(),
                            /*duplicate_handle=*/true);
}

void registry_deregister(ThreadLifecycle *lc) {
  if (!lc)
    return;
  BucketEntry *cur = lc->bucket_entry.load(cpp::MemoryOrder::ACQUIRE);
  if (cur == nullptr || is_registration_pending(cur))
    return; // Not registered (or in-flight registration not yet published).

  // unlink_bucket_entry_for returns true iff THIS thread won the CAS-pair
  // and owns the entry's retire. Live_count and iter unlink are tied to
  // that win so both happen at most once per successful registration.
  if (!unlink_bucket_entry_for(lc))
    return;
  iter_mark_unlink(lc);
  state().live_count.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
}

void registry_deregister_and_retire(ThreadLifecycle *lc) {
  if (!lc)
    return;
  registry_deregister(lc);
  g_registry_domain.retire(lc);
}

// =========================================================================
// Public API: lookups
// =========================================================================

ThreadLifecycle *registry_find_by_task_id(uint32_t task_id) {
  if (task_id == 0)
    return nullptr;
  return find_by_task_id_internal(task_id);
}

ThreadLifecycle *registry_resolve(ThreadHandle h) {
  if (!h.is_valid())
    return nullptr;
  ThreadLifecycle *lc = find_by_task_id_internal(h.task_id);
  if (!lc)
    return nullptr;
  // Belt-and-suspenders: tid sanity check. task_id alone is
  // authoritative (never recycles), but a tid mismatch indicates
  // the captured handle was for a different thread that has since
  // been recycled by NT — refuse rather than alert wrong-thread.
  if (lc->tid != h.tid)
    return nullptr;
  return lc;
}

ThreadHandle current_thread_handle() {
  ThreadLifecycle *lc = get_current_lifecycle();
  if (!lc || lc->task_id == 0)
    return ThreadHandle::invalid();
  return ThreadHandle{/*tid=*/lc->tid, /*task_id=*/lc->task_id};
}

// =========================================================================
// Public API: iteration
// =========================================================================

// Walk the global iter list, invoking visitor on each unmarked
// (logically-live) lifecycle. Physically unlinks marked predecessors
// as it goes — same Harris-with-no-restart discipline as
// walk_bucket_for_task_id (H3 + livelock fix): on splice CAS-fail,
// re-read at the SAME pred_next and continue from there. NEVER
// restart from iter_head, which would CAS-storm under concurrent
// thread-creation prepends.
//
// Crystalline reservation discipline: kReservationIter pins every
// node reachable from the iter head at entry-time epoch. Splicing
// out marked nodes is a structural operation; the reservation
// continues to pin them until they are retired by the original
// deregister path, at which point no live reader observes them
// anymore (we just removed the last reference).
bool registry_for_each_impl(RegistryVisitorFn visitor, void *ctx,
                             uint32_t skip_tid) {
  if (!ensure_initialized())
    return false;

  cpp::Atomic<ThreadLifecycle *> *pred_next = &iter_head_atomic();
  ThreadLifecycle *cur =
      crys_read(*pred_next, kReservationIter, nullptr);
  cur = harris_unmark(cur);
  while (cur) {
    ThreadLifecycle *next_raw =
        cur->next_iter.load(cpp::MemoryOrder::ACQUIRE);
    bool marked = harris_is_marked(next_raw);
    ThreadLifecycle *next_unmarked = harris_unmark(next_raw);

    if (marked) {
      if (!harris_try_unlink_iter(*pred_next, cur, next_unmarked)) {
        // Re-read at the same slot. Concurrent splice or prepend has
        // updated pred_next; advance to whatever's there now.
        cur = harris_unmark(
            pred_next->load(cpp::MemoryOrder::ACQUIRE));
        continue;
      }
      cur = next_unmarked;
      continue;
    }

    if (cur->tid != skip_tid) {
      if (visitor(ctx, cur))
        return true;
    }
    pred_next = &cur->next_iter;
    cur = next_unmarked;
  }
  return false;
}

namespace {

// Issue NtAlertMultipleThreadByThreadId on a batch of TIDs, falling
// back to per-thread NtAlertThreadByThreadId if the batched syscall
// is unavailable or fails. Used as the inline flush from
// registry_alert_all.
LIBC_INLINE void alert_tid_batch(HANDLE *batch, uint32_t n) {
  if (n == 0)
    return;
  NTSTATUS st = nt_optional().alert_multiple(batch, n, nullptr, 0);
  if (NT_SUCCESS(st))
    return;
  for (uint32_t i = 0; i < n; ++i)
    ::NtAlertThreadByThreadId(batch[i]);
}

} // anonymous namespace

// Alert every live thread. Walks the iter list once with a fixed-size
// stack buffer; calls alert_tid_batch (NtAlertMultipleThreadByThreadId
// + per-thread fallback) whenever the buffer fills, then resets and
// continues. No heap allocation, no live_count snapshot, no pagination
// state — exactly the pattern futex_utils uses for its alert batches.
//
// Snapshot semantics: the walk pins the iter head via a Crystalline
// reservation; freshly-registered threads landing concurrently are
// observed via newer iter_head values across walker restarts
// triggered by physical-unlink CAS misses. Threads registered after
// the walk completes are not alerted (necessarily — their registration
// is a happens-after event), but every thread visible to any reader
// during the broadcast is.
void registry_alert_all(uint32_t skip_tid) {
  if (!ensure_initialized())
    return;

  // Buffer size matches futex_utils' kAlertBatch — 128 HANDLEs (1 KB)
  // is large enough that a single batch covers a typical small-thread
  // process in one syscall, but small enough that the on-stack
  // reservation is comfortable at every cleanup callsite.
  constexpr uint32_t kBatch = 128;
  HANDLE buf[kBatch];
  uint32_t fill = 0;

  cpp::Atomic<ThreadLifecycle *> *pred_next = &iter_head_atomic();
  ThreadLifecycle *cur =
      crys_read(*pred_next, kReservationIter, nullptr);
  cur = harris_unmark(cur);

  while (cur) {
    ThreadLifecycle *next_raw =
        cur->next_iter.load(cpp::MemoryOrder::ACQUIRE);
    bool marked = harris_is_marked(next_raw);
    ThreadLifecycle *next_unmarked = harris_unmark(next_raw);

    if (marked) {
      if (!harris_try_unlink_iter(*pred_next, cur, next_unmarked)) {
        cur = harris_unmark(
            pred_next->load(cpp::MemoryOrder::ACQUIRE));
        continue;
      }
      cur = next_unmarked;
      continue;
    }

    if (cur->tid != skip_tid) {
      buf[fill++] = reinterpret_cast<HANDLE>(
          static_cast<uintptr_t>(cur->tid));
      if (fill == kBatch) {
        alert_tid_batch(buf, fill);
        fill = 0;
      }
    }
    pred_next = &cur->next_iter;
    cur = next_unmarked;
  }

  alert_tid_batch(buf, fill);
}

// =========================================================================
// Public API: suspend / resume
// =========================================================================

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

// =========================================================================
// Public API: lifecycle / fork / TLS / stats
// =========================================================================

bool registry_ensure_initialized() { return ensure_initialized(); }

void registry_thread_quiesce() { g_registry_domain.clear_all(); }

uint32_t registry_live_count_impl() {
  return state().live_count.load(cpp::MemoryOrder::ACQUIRE);
}

namespace robust_mutex {
void lifecycle_destroy(ThreadLifecycle *lc);
} // namespace robust_mutex

void registry_fork_reinit(ThreadLifecycle *self) {
  // Crystalline's `fork_reinit_trampoline` runs separately (chained
  // through the global Crystalline domain registry, BEFORE this).
  // It zeroes the surviving thread's per-domain reservation slots
  // and resets epoch/slow_counter to fresh values. After that, the
  // child is single-threaded and can safely walk the iter list /
  // hash without Crystalline reservation discipline.

  // Order assertion: g_registry_domain.current_epoch() == 1 iff the
  // Crystalline fork-reinit pass already ran (it stores 1 there).
  // If we observe a higher epoch, the orchestrator has been reordered
  // and this walker would dereference parent-thread reservation state
  // — trap rather than corrupt the child.
  if (g_registry_domain.current_epoch() != 1)
    __builtin_trap();

  if (!ensure_initialized())
    return;

  // Walk the iter list, cleaning up every non-self lifecycle and
  // freeing its slab slot directly (no Crystalline retire — child
  // is single-threaded; bypass the FreeFn dispatcher).
  ThreadLifecycle *cur = harris_unmark(
      iter_head_atomic().load(cpp::MemoryOrder::RELAXED));
  while (cur) {
    ThreadLifecycle *next = harris_unmark(
        cur->next_iter.load(cpp::MemoryOrder::RELAXED));
    if (cur != self) {
      // Stale parent-thread state. Handles aren't inherited (we used
      // NtDuplicateObject without OBJ_INHERIT), so there's nothing
      // to NtClose. Robust mutexes need OWNER_DIED marking via
      // lifecycle_destroy.
      cur->thread_handle.store(nullptr, cpp::MemoryOrder::RELAXED);
      cur->bucket_entry.store(nullptr, cpp::MemoryOrder::RELAXED);
      cur->next_iter.store(nullptr, cpp::MemoryOrder::RELAXED);
      robust_mutex::lifecycle_destroy(cur);
      free_lifecycle(cur);
    }
    cur = next;
  }

  // Reset iter head: only `self` survives.
  if (self) {
    self->next_iter.store(nullptr, cpp::MemoryOrder::RELAXED);
    iter_head_atomic().store(self, cpp::MemoryOrder::RELAXED);
  } else {
    iter_head_atomic().store(nullptr, cpp::MemoryOrder::RELAXED);
  }

  // Wipe the top-level array and tear down every BucketHeadPage.
  // Stale BucketEntry slots are NOT individually returned to the
  // slab pool — `g_bucket_entry_pool.fork_reinit()` below handles
  // pool-wide cleanup atomically (releases dead-thread slabs and
  // resets the pool's internal locks). Walking the chains to free
  // each entry would be redundant work and would also race against
  // the surviving thread's own slab cache (which we reset below).
  auto *arr = top_level_array();
  if (arr) {
    for (uint32_t p = 0; p < kTopLevelCapacity; ++p) {
      BucketHeadPage *page = arr[p].load(cpp::MemoryOrder::RELAXED);
      if (!page)
        continue;
      arr[p].store(nullptr, cpp::MemoryOrder::RELAXED);
      internal::page_free(page);
    }
  }

  // Reset the slab pool. The pool's fork_reinit:
  //   * clears the abandoned-adoption stack,
  //   * resets internal locks possibly held by dead threads,
  //   * walks all_slabs_head_, claiming dead-thread slabs under the
  //     surviving thread's TID, then full_release-ing fully-empty ones
  //     and pushing the rest onto abandoned for re-adoption.
  // The surviving thread's TEB-cached slab (managed by the pool's
  // pool-managed TLS) is preserved by TID match in fork_reinit's walk.
  g_bucket_entry_pool.fork_reinit();

  // Re-register self.
  if (self && self->task_id != 0) {
    BucketEntry *entry = alloc_bucket_entry(self->task_id, self);
    if (entry) {
      self->bucket_entry.store(entry, cpp::MemoryOrder::RELAXED);
      uint64_t raw = encode_hash_state(kInitialL, 0, false);
      state().ls_state.store(raw, cpp::MemoryOrder::RELAXED);
      HashState st = decode_hash_state(raw);
      uint32_t b = hash_for_insert(self->task_id, st);
      BucketLocation loc = locate_bucket(b);
      BucketHeadPage *page = ensure_bucket_page(loc.page_index);
      if (page) {
        page->heads[loc.in_page_index].store(entry,
                                              cpp::MemoryOrder::RELAXED);
      }
    }
  } else {
    state().ls_state.store(encode_hash_state(kInitialL, 0, false),
                            cpp::MemoryOrder::RELAXED);
  }

  state().live_count.store(self ? 1u : 0u, cpp::MemoryOrder::RELAXED);
  state().split_advisory.store(0, cpp::MemoryOrder::RELAXED);
}

// =========================================================================
// Process-fini hook
// =========================================================================
//
// Releases the BucketEntry slab pool's TEB TLS slot and unregisters
// its .CRT$XLC cleanup callback. Per-thread slabs themselves stay
// alive (the OS reaps them at process exit); this just tears down
// the pool's TLS plumbing so the slot can be reused if libc is later
// re-initialized in the same address space.
//
// Phase 4 — runs alongside other thread-subsystem fini hooks (the
// lifecycle pool, the futex pools, etc.).
namespace internal {
static void thread_registry_fini() {
  ::LIBC_NAMESPACE::g_bucket_entry_pool.fini_tls();
}
} // namespace internal

} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_FINI(4, thread_registry,
                   &::LIBC_NAMESPACE::internal::thread_registry_fini)
