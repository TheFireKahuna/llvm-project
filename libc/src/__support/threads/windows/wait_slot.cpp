//===--- Lock-free wait slot pool implementation --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/wait_slot.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/legacy/commit_region.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/macros/config.h"
// ThreadLifecycle's full definition. Safe here but NOT from
// wait_slot.h — this .cpp sits below the wait_slot ↔
// thread_lifecycle ↔ futex_utils include chain.
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace wait_slot {

// Demand-committed pool: 4 MB VA reserved at init, pages committed
// on first exhaustion. 1<<16 slots covers 32K+ concurrent threads
// (each needs ≤2: primary + VEH nesting); the 16-bit cap matches
// the head index width. Only the sentinel page is committed at
// startup.
static constexpr uint32_t POOL_CAPACITY = 1u << 16;
static constexpr size_t POOL_BYTES = POOL_CAPACITY * sizeof(WaitSlot);
// Cached at first use; commit granularity must match OS page size.
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

// Treiber stack of available slot indices, packed [gen:16 | head:16]
// to defeat ABA. Hottest atomic in the subsystem — CAS'd on every
// alloc and reclaim.
//
// Wrapped in a 64-byte struct so the entire cache line is
// structurally owned: alignas(64) on the struct forces size to a
// multiple of alignof = 64, and a single uint32_t member rounds up
// to exactly 64. No neighbour static can false-share. Replaces the
// prior `alignas(64) atomic + trailing pad` pattern, which depended
// on compiler-preserved BSS declaration order to keep the pad
// adjacent to the atomic.
struct alignas(64) FreelistLine {
  cpp::Atomic<uint32_t> head{0};
};
static_assert(sizeof(FreelistLine) == 64,
              "FreelistLine must occupy exactly one 64-byte cache line");
static_assert(alignof(FreelistLine) == 64,
              "FreelistLine must be cache-line aligned");
static FreelistLine g_freelist;

static constexpr uint32_t GEN_SHIFT = 16;
static constexpr uint32_t INDEX_MASK = (1u << GEN_SHIFT) - 1;

static uint32_t fl_head(uint32_t packed) { return packed & INDEX_MASK; }
static uint32_t fl_gen(uint32_t packed) { return packed >> GEN_SHIFT; }
static uint32_t fl_pack(uint32_t gen, uint32_t head) {
  return ((gen & 0xFFFF) << GEN_SHIFT) | (head & INDEX_MASK);
}

static DWORD tls_index = internal::TLS_OUT_OF_INDEXES;

// TLS slot ref packs {index, generation} for stale-ref detection.
// Layout in 64-bit void*: bits 0..15 = pool index, 16..31 reserved,
// 32..63 = 32-bit gen. Full 32 bits make ABA wraparound
// astronomical (>130 years at 10K reclaims/sec; 16-bit would wrap
// in seconds under stress).
static void *pack_tls(uint32_t index, uint32_t gen) {
  return reinterpret_cast<void *>(
      (static_cast<uintptr_t>(index) & 0xFFFFu) |
      (static_cast<uintptr_t>(gen) << 32));
}
static uint32_t tls_index_of(void *val) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(val)) & 0xFFFFu;
}
static uint32_t tls_gen_of(void *val) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(val) >> 32);
}

// Push `index` onto the freelist. Caller MUST have transitioned the
// slot to IDLE+CERT and bumped `generation` first (reclaim_slot
// guarantees both). Concurrency: any number of pushers; a popper
// (alloc_slot) racing with a pusher is the standard Treiber-stack
// case, defended by the [gen:16 | head:16] packing.
//
// No hazard / quiescence gating is needed. The state+tag fold in
// the harris walker plus monotonic per-slot tag mean any walker that
// captured a stale snapshot fails its mid-splice CAS on tag/state
// mismatch, and any post-recycle dereference reads VALID pool memory
// (slots are statically allocated; never freed) yielding either the
// IDLE-on-reachable-chain Retry signal or a tag-mismatch CAS failure.
// The retire queue and hazard window that previously gated this push
// were vestigial after the state+tag fold landed.
static void freelist_push(uint32_t index) {
  pool[index].subsystem.store(SubsystemKind::None, cpp::MemoryOrder::RELAXED);
  pool[index].thread_id.store(0, cpp::MemoryOrder::RELAXED);
  pool[index].wait_address.store(0, cpp::MemoryOrder::RELAXED);
  // Park flag belongs to the owner of the prior wait cycle; clear
  // before the slot becomes a free-pool entry so the next allocator
  // observes a clean field. Owner-exclusive while linked, so the
  // single-threaded reset here races only with other freelist_push
  // entrants on the same slot — impossible by reclaim_slot's idempotent
  // entry CAS.
  pool[index].park_state.store(0, cpp::MemoryOrder::RELAXED);
  pool[index].generation.fetch_add(1, cpp::MemoryOrder::RELEASE);
  for (;;) {
    uint32_t old = g_freelist.head.load(cpp::MemoryOrder::ACQUIRE);
    // link_store sets CERT (slot is off any futex chain by being
    // on the freelist) and bumps tag. Tag is monotonic across the
    // slot's full lifecycle, so a free→alloc→push cycle at the
    // same Treiber position still fails a walker's mid-splice CAS
    // against the pre-cycle link value.
    linkage::link_store(pool[index].link, IDLE,
                         static_cast<uint16_t>(fl_head(old)));
    uint32_t desired = fl_pack(fl_gen(old) + 1, index);
    if (g_freelist.head.compare_exchange_weak(old, desired,
                                       cpp::MemoryOrder::RELEASE,
                                       cpp::MemoryOrder::RELAXED))
      return;
  }
}

//===----------------------------------------------------------------------===//
// TLS cleanup and pool growth
//===----------------------------------------------------------------------===//

// TLS cleanup at thread exit splices linked slots out of whatever
// structure they're parked on. Each subsystem registers its own
// walker at startup. Atomic fn pointers so installation can race
// harmlessly with early thread exits (pre-registration).
static cpp::Atomic<HarrisUnlinker> g_harris_unlink{nullptr};
static cpp::Atomic<ParkingLotUnlinker> g_parking_lot_unlink{nullptr};

void register_harris_unlinker(HarrisUnlinker fn) {
  g_harris_unlink.store(fn, cpp::MemoryOrder::RELEASE);
}

void register_parking_lot_unlinker(ParkingLotUnlinker fn) {
  g_parking_lot_unlink.store(fn, cpp::MemoryOrder::RELEASE);
}

// TLS cleanup callback (runs at thread exit via .CRT$XLC).
//
// Dispatch by LINK_CERT_BIT:
//   IDLE+CERT=1 ⇒ provably off-chain. free_slot fast-path; skip
//                 reclaim_slot's state CAS and unlinker dispatch.
//   else        ⇒ may be on-chain. Dispatch the subsystem unlinker.
//
// CERT=1 with state != IDLE is meaningful in Futex (post upgrade)
// but parking-lot only passively preserves CERT — its on-chain
// WAITING/IN_KERNEL may carry stale CERT=1 from a prior
// clear_slot_owned. So we trust CERT only when state==IDLE.
//
// Gen-mismatch ⇒ slot reclaimed behind our back (possibly
// reallocated) — DO NOT touch it.
static void slot_cleanup(void *data) {
  if (!data)
    return;
  uint32_t index = tls_index_of(data);
  uint32_t saved_gen = tls_gen_of(data);

  if (pool[index].generation.load(cpp::MemoryOrder::ACQUIRE) != saved_gen)
    return;

  linkage::Link link_val = pool[index].link.load(cpp::MemoryOrder::ACQUIRE);

  if (link_val.state() == IDLE && link_val.is_certified()) {
    free_slot(index);
    return;
  }

  // Possibly on-chain. The unlinker's harris_unlink either splices
  // (its splice publishes CERT) or walks-to-NULL (already off-chain).
  // Parking-lot's unlinker takes the bucket lock, so its dispatch is
  // authoritative regardless of CERT.
  uintptr_t wa =
      pool[index].wait_address.load(cpp::MemoryOrder::RELAXED);
  SubsystemKind subsys =
      pool[index].subsystem.load(cpp::MemoryOrder::ACQUIRE);
  bool should_reclaim = false;
  if (wa) {
    if (subsys == SubsystemKind::Futex) {
      HarrisUnlinker fn = g_harris_unlink.load(cpp::MemoryOrder::ACQUIRE);
      if (fn)
        should_reclaim = fn(reinterpret_cast<void *>(wa),
                            static_cast<uint16_t>(index), saved_gen);
    } else if (subsys == SubsystemKind::ParkingLot) {
      ParkingLotUnlinker fn =
          g_parking_lot_unlink.load(cpp::MemoryOrder::ACQUIRE);
      if (fn)
        should_reclaim = fn(reinterpret_cast<void *>(wa),
                            static_cast<uint16_t>(index), saved_gen);
    } else {
      // None + wait_address set ⇒ orphan; nothing to unlink.
      should_reclaim = true;
    }
  } else {
    // No wait_address — nothing to unlink.
    should_reclaim = true;
  }

  if (should_reclaim) {
    reclaim_slot(ReclaimAuthority::after_unlinker_dispatch(index));
    return;
  }

  // Unlinker returned false. Re-check gen + CERT: a concurrent
  // splicer may have set CERT mid-dispatch (slot now off-chain), or
  // gen may have bumped (another reclaimer won).
  //
  // Use reclaim_slot, not free_slot: state may still be SIGNALED_*
  // (CERT=1 alone doesn't imply IDLE). reclaim_slot's idempotent
  // state CAS handles both the SIGNALED → IDLE transition and the
  // already-IDLE benign-noop case.
  if (pool[index].generation.load(cpp::MemoryOrder::ACQUIRE) == saved_gen) {
    linkage::Link post_link =
        pool[index].link.load(cpp::MemoryOrder::ACQUIRE);
    if (post_link.is_certified())
      reclaim_slot(ReclaimAuthority::after_unlinker_dispatch(index));
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

  // Chain new slots into the freelist. Fresh pages: state=IDLE,
  // tag=0, next = i+1 (NULL at tail), CERT=1 (off any futex chain
  // by construction). First post-alloc write bumps tag to 1.
  for (uint32_t i = cur; i < end - 1; ++i)
    pool[i].link.store(
        linkage::Link::pack_certified(IDLE, 0, static_cast<uint16_t>(i + 1)),
        cpp::MemoryOrder::RELAXED);
  pool[end - 1].link.store(linkage::Link::pack_certified(IDLE, 0, NULL_INDEX),
                            cpp::MemoryOrder::RELAXED);

  // Splice: new tail's .next → existing freelist head, then CAS
  // freelist head to new chain's first slot. link_store_next
  // preserves CERT (both already CERT=1 from freelist lifecycle).
  for (;;) {
    uint32_t old = g_freelist.head.load(cpp::MemoryOrder::ACQUIRE);
    linkage::link_store_next(pool[end - 1].link,
                              static_cast<uint16_t>(fl_head(old)));
    uint32_t desired = fl_pack(fl_gen(old) + 1, cur);
    if (g_freelist.head.compare_exchange_weak(old, desired,
                                       cpp::MemoryOrder::RELEASE,
                                       cpp::MemoryOrder::RELAXED))
      return true;
  }
}

// Pop from freelist (Treiber stack pop).
static uint32_t alloc_slot() {
  for (;;) {
    uint32_t old = g_freelist.head.load(cpp::MemoryOrder::ACQUIRE);
    uint32_t head = fl_head(old);
    if (head == NULL_INDEX) {
      // Freelist empty — try to commit more slots.
      if (!commit_more_slots())
        return NULL_INDEX; // pool fully committed and exhausted
      continue;
    }
    uint32_t next =
        linkage::link_next(pool[head].link.load(cpp::MemoryOrder::ACQUIRE));
    uint32_t desired = fl_pack(fl_gen(old) + 1, next);
    if (g_freelist.head.compare_exchange_weak(old, desired,
                                       cpp::MemoryOrder::ACQ_REL,
                                       cpp::MemoryOrder::ACQUIRE)) {
      return head;
    }
  }
}

void free_slot(uint32_t index) {
  // Direct freelist push — caller MUST guarantee the slot is already
  // in IDLE state (no state CAS performed here). Used by:
  //   - slot_cleanup fast path (IDLE+CERT, off any chain).
  //   - init / fork_reinit (single-threaded).
  // Wake-path / harris_unlink cleanup MUST use reclaim_slot.
  freelist_push(index);
}

void reclaim_slot(ReclaimAuthority auth) {
  uint32_t index = auth.index();

  // Idempotency: link_exchange_state_to_idle_certify atomically
  // writes IDLE+CERT. First caller sees prev != IDLE and wins;
  // subsequent callers see IDLE and bail. The early bail is REQUIRED
  // to keep freelist_push single-actor (double-push corrupts the
  // freelist Treiber stack). Multiple actors may race here: harris
  // splice, pop_and_signal_one, slot_cleanup, Phase 1.75 self-reclaim.
  //
  // Caller contract: detach proven via the data-structure CAS
  // immediately preceding this call (audited per ReclaimAuthority
  // factory). The certify publishes CERT atomically with IDLE so
  // subsequent clear_slot_owned / TLS-fast-path can rely on it.
  //
  // gen bump and subsystem clear are folded into freelist_push —
  // the eager-bump-then-push pattern was redundant. A walker that
  // captures target.gen between our state CAS and freelist_push's
  // gen bump observes IDLE-on-reachable-chain (Retry) or fails its
  // mid-splice CAS on tag mismatch; both outcomes converge on a
  // gen re-check after freelist_push completes, by which point
  // gen has bumped. Saves one LOCK XADD + one RELAXED store per
  // reclaim with no behavioural change.
  if (linkage::link_exchange_state_certify(pool[index].link, IDLE) == IDLE)
    return;
  freelist_push(index);
}

uint32_t get_stale_slot(uintptr_t &wait_address_out,
                        uint32_t &expected_gen_out) {
  if (tls_index == internal::TLS_OUT_OF_INDEXES)
    return NULL_INDEX;
  void *val = internal::teb_tls_get(tls_index);
  if (!val)
    return NULL_INDEX;
  uint32_t index = tls_index_of(val);
  uint32_t saved_gen = tls_gen_of(val);
  // Only reclaim if the slot is still ours (gen match) and TIMED_OUT.
  if (pool[index].generation.load(cpp::MemoryOrder::ACQUIRE) == saved_gen &&
      linkage::link_load_state(pool[index].link) == TIMED_OUT) {
    wait_address_out =
        pool[index].wait_address.load(cpp::MemoryOrder::RELAXED);
    expected_gen_out = saved_gen;
    return index;
  }
  return NULL_INDEX;
}

void clear_tls_slot() {
  if (tls_index == internal::TLS_OUT_OF_INDEXES)
    return;
  internal::teb_tls_set(tls_index, nullptr);
}

void refresh_tls_slot_generation(uint32_t index, uint32_t new_gen) {
  if (tls_index == internal::TLS_OUT_OF_INDEXES)
    return;
  void *val = internal::teb_tls_get(tls_index);
  if (!val)
    return;
  // No-op when clearing a secondary slot — TLS still references
  // the primary and must not be rewritten to point at the secondary.
  if (tls_index_of(val) != index)
    return;
  // Caller-supplied new_gen avoids a second ACQUIRE on slot.generation.
  internal::teb_tls_set(tls_index, pack_tls(index, new_gen));
}

// Capture the current thread's ThreadHandle into the slot's owner
// fields. Consumed by the validated alert helpers via
// registry_resolve.
//
// Null lifecycle (pre-init, or foreign threads) ⇒ leave the handle
// at the unbound sentinel (task_id == 0); validated alert helpers
// fall through to unvalidated alert in that narrow window.
//
// Owner-only writer; plain stores suffice (publication via the
// subsequent link CAS / push).
//
// Fast-skip: if the stored (tid, task_id) already matches the
// lifecycle's, the handle is current — saves two u32 stores on the
// TLS-reuse hot path. `lc->task_id` is immutable after register; a
// re-register replaces the lifecycle (different task_id), so a
// match is exact.
static void bind_slot_owner_identity(uint32_t index) {
  ThreadLifecycle *lc = get_current_lifecycle();
  if (lc && lc->task_id != 0) {
    if (pool[index].owner_task_id == lc->task_id &&
        pool[index].owner_tid == lc->tid)
      return; // already bound to this lifecycle's current identity
    pool[index].owner_tid = lc->tid;
    pool[index].owner_task_id = lc->task_id;
  } else {
    if (pool[index].owner_task_id == 0)
      return; // already unbound
    pool[index].owner_tid = 0;
    pool[index].owner_task_id = 0;
  }
}

uint32_t alloc_secondary() {
  uint32_t index = alloc_slot();
  if (index == NULL_INDEX)
    return NULL_INDEX;
  pool[index].thread_id.store(NtCurrentThreadId(),
                               cpp::MemoryOrder::RELAXED);
  pool[index].generation.fetch_add(1, cpp::MemoryOrder::RELAXED);
  linkage::link_store(pool[index].link, IDLE, NULL_INDEX);
  bind_slot_owner_identity(index);
  return index;
}

void release_secondary(uint32_t index) { free_slot(index); }

WaitSlot &get_slot(uint32_t index) { return pool[index]; }

uint32_t get_slot_index() {
  if (tls_index == internal::TLS_OUT_OF_INDEXES)
    return NULL_INDEX;

  // Fast path: reuse the existing TLS slot if IDLE+CERT and gen
  // matches. Direct TEB lookup — single instruction, no call.
  //
  // Reuse REQUIRES CERT=1 — IDLE+CERT=0 indicates an invariant
  // violation (clear_slot_owned wrote IDLE without certificate);
  // fall through to fresh alloc rather than reuse a slot of
  // dubious chain status.
  void *val = internal::teb_tls_get(tls_index);
  if (val) {
    uint32_t index = tls_index_of(val);
    uint32_t saved_gen = tls_gen_of(val);
    linkage::Link link_val = pool[index].link.load(cpp::MemoryOrder::ACQUIRE);
    if (link_val.state() == IDLE && link_val.is_certified() &&
        pool[index].generation.load(cpp::MemoryOrder::ACQUIRE) == saved_gen) {
      uint32_t new_gen = saved_gen + 1;
      pool[index].generation.store(new_gen, cpp::MemoryOrder::RELAXED);
      internal::teb_tls_set(tls_index, pack_tls(index, new_gen));
      // Refresh SlotRef binding even on reuse — the registry can
      // rehome a thread's slot (page retirement forcing
      // reassignment), so paying one rebind per reuse keeps the
      // "slot always has a current SlotRef" invariant robust.
      bind_slot_owner_identity(index);
      return index;
    }
    // Stale or linked — fresh alloc. (Stale branch is owned by
    // get_stale_slot / harris_unlink at the caller; here we just
    // issue a fresh slot so the new wait can proceed.)
  }

  uint32_t index = alloc_slot();
  if (index == NULL_INDEX)
    return NULL_INDEX;

  pool[index].thread_id.store(NtCurrentThreadId(),
                               cpp::MemoryOrder::RELAXED);
  uint32_t gen =
      pool[index].generation.fetch_add(1, cpp::MemoryOrder::RELAXED) + 1;
  linkage::link_store(pool[index].link, IDLE, NULL_INDEX);
  bind_slot_owner_identity(index);
  internal::teb_tls_set(tls_index, pack_tls(index, gen));
  return index;
}

void init() {
  // Reserve VA for the full pool; commit the first page only.
  (void)pool_region.init(POOL_BYTES, slots_per_page() * sizeof(WaitSlot));
  pool = pool_region.as<WaitSlot>();

  // Slot 0 = sentinel. Link 1..N-1 into the freelist with tag=0,
  // IDLE, CERT=1 (off any futex chain).
  for (uint32_t i = 1; i < slots_per_page() - 1; ++i)
    pool[i].link.store(
        linkage::Link::pack_certified(IDLE, 0, static_cast<uint16_t>(i + 1)),
        cpp::MemoryOrder::RELAXED);
  pool[slots_per_page() - 1].link.store(
      linkage::Link::pack_certified(IDLE, 0, NULL_INDEX),
      cpp::MemoryOrder::RELAXED);
  g_freelist.head.store(fl_pack(0, 1), cpp::MemoryOrder::RELAXED);
  committed_slots.store(slots_per_page(), cpp::MemoryOrder::RELAXED);

  tls_index = internal::tls_alloc();
  if (tls_index != internal::TLS_OUT_OF_INDEXES)
    internal::tls_cleanup_register(tls_index, slot_cleanup,
                                   internal::kTlsCleanupPhaseWaitSlot);
}

void fini() {
  if (tls_index != internal::TLS_OUT_OF_INDEXES) {
    internal::tls_cleanup_unregister(tls_index);
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
    pool[i].thread_id.store(0, cpp::MemoryOrder::RELAXED);
    pool[i].wait_address.store(0, cpp::MemoryOrder::RELAXED);
    pool[i].park_state.store(0, cpp::MemoryOrder::RELAXED);
    // Parent owner ThreadHandle references the parent registry —
    // invalid in the child. Surviving slot rebinds on next wait entry.
    pool[i].owner_tid = 0;
    pool[i].owner_task_id = 0;
    // Filter clear: not strictly required (filter_fn is a code
    // pointer into a region also valid in the child image, and
    // filter_arg is a pure value), but the slot is re-entering
    // the freelist as IDLE so the "IDLE slot has no filter"
    // hygiene applies.
    pool[i].filter_fn.store(nullptr, cpp::MemoryOrder::RELAXED);
    pool[i].filter_arg.store(0, cpp::MemoryOrder::RELAXED);
    pool[i].wake_bitset = 0xFFFFFFFFu;
    // Bumped tag is defensive — any pre-fork walker gen-cache
    // shouldn't survive fork anyway.
    linkage::link_store(
        pool[i].link, IDLE,
        static_cast<uint16_t>((i + 1 < committed) ? (i + 1) : NULL_INDEX));
  }
  g_freelist.head.store(fl_pack(0, committed > 1 ? 1 : 0),
                 cpp::MemoryOrder::RELAXED);

  // Parent's TLS state is invalid in the child.
  if (tls_index != internal::TLS_OUT_OF_INDEXES)
    internal::tls_free(tls_index);
  tls_index = internal::tls_alloc();
  if (tls_index != internal::TLS_OUT_OF_INDEXES)
    internal::tls_cleanup_register(tls_index, slot_cleanup,
                                   internal::kTlsCleanupPhaseWaitSlot);
}

// ===========================================================================
// Validated alert helpers — stale-TID alert-leak closure
// ===========================================================================
//
// Every waker-side alert routes through one of these. They resolve
// the captured ThreadHandle through the Crystalline-protected
// registry; on a live resolve they issue the alert. On a
// dead/recycled resolve they skip the alert entirely — the captured
// TID is NOT blindly passed to the kernel, which is what closes the
// leak.
//
// "Unbound" handles (task_id == 0) signal a slot allocated before
// the lifecycle subsystem came up, or by a foreign thread. Those
// fall through to unvalidated alerts — same behavior as pre-fix
// code, only reachable in a narrow early-init / pre-registration
// window.
//
// "Alert in flight" publish is structural via LINK_ALERT_FIRED_BIT
// on slot.link, set atomically by the pre-mark CAS that commits
// the wake (see link_cas_state, link_cas_snap, link_cas_state_
// detached in wait_slot.h). The waiter's SIGNALED-observation
// sites read the bit and set expect_late_alert — no per-target
// cross-thread RELEASE bump from the waker is required here.

void mark_expect_late_alert() {
  ThreadLifecycle *lc = get_current_lifecycle();
  if (!lc)
    return;
  // RELAXED is sufficient: this thread is the only writer and the only
  // reader. The subsequent consume happens after a kernel round-trip
  // (NtWaitForAlertByThreadId) which is a cumulative fence.
  lc->expect_late_alert.store(1, cpp::MemoryOrder::RELAXED);
}

bool consume_expect_late_alert() {
  ThreadLifecycle *lc = get_current_lifecycle();
  if (!lc)
    return false;
  // exchange(0): read-and-clear in one op. RELAXED for the same reason
  // as mark_expect_late_alert — owner-only access, kernel-boundary
  // fencing on the enclosing Phase 4 wait.
  return lc->expect_late_alert.exchange(0, cpp::MemoryOrder::RELAXED) != 0;
}

// Single-alert primitive. Routes through NtAlertMultiple with
// count=1, nullptr/0 extended parameters — same kernel entry as
// the batch path. No AutoBoost (single wake policy avoids
// over-preemption in lock-release handoff chains). Call sites that
// want AutoBoost pass a populated PS_ALERT_THREAD_EXTENDED_PARAMETER
// directly via nt_optional().alert_multiple.
//
// Unifying single + batch on Multiple eliminates
// NtAlertThreadByThreadIdEx from the surface — its SRW-lock-release
// feature was never used (we always passed nullptr Lock).
LIBC_INLINE static void alert_one_unvalidated(HANDLE tid_handle) {
  HANDLE tids[1] = {tid_handle};
  nt_optional().alert_multiple(tids, 1, nullptr, 0);
}

// Validated tail. On live resolve: alert. On dead/recycled resolve:
// skip (closes the stale-TID leak). Unbound handles (task_id == 0)
// ⇒ unvalidated fallback (preserves pre-fix behavior).
//
// "Alert in flight" classification on the receiver side is driven
// by LINK_ALERT_FIRED_BIT on slot.link, published atomically by
// the pre-mark CAS that preceded this call — no cross-thread
// counter bump on the target's lifecycle is needed.
static void alert_validated(uint64_t owner_packed, HANDLE tid_handle) {
  ThreadHandle h = ThreadHandle::unpack(owner_packed);
  if (!h.is_valid()) {
    alert_one_unvalidated(tid_handle);
    return;
  }
  ThreadLifecycle *lc = registry_resolve(h);
  if (lc)
    alert_one_unvalidated(tid_handle);
}

void alert_one_if_live(uint64_t owner_packed, uint32_t tid, WaitSlot &slot) {
  HANDLE tid_handle =
      reinterpret_cast<HANDLE>(static_cast<uintptr_t>(tid));

  // Fast path: slot.thread_id still matches captured tid. The slot
  // hasn't been freelist_push'd (zeros tid) or reallocated
  // (rewrites tid), so the captured tid still names the intended
  // owner — skip the registry resolve.
  //
  // Residual window: a TerminateThread'd owner whose TID was
  // recycled by Windows AND whose slot_cleanup never ran. Narrow;
  // matches pre-fix risk exposure.
  if (slot.thread_id.load(cpp::MemoryOrder::RELAXED) == tid) {
    alert_one_unvalidated(tid_handle);
    return;
  }

  alert_validated(owner_packed, tid_handle);
}

void alert_one_if_live(uint64_t owner_packed, uint32_t tid) {
  HANDLE tid_handle =
      reinterpret_cast<HANDLE>(static_cast<uintptr_t>(tid));
  alert_validated(owner_packed, tid_handle);
}

uint32_t alert_multiple_if_live(const BatchTarget *tgts, uint32_t count,
                                 HANDLE *out_filtered, void *ab_ctx,
                                 uint32_t ab_ctx_count) {
  // Crystalline reservations are checkpoint-based: registry_resolve
  // captures the per-thread era stamp on each call and pins
  // everything observable through the lookup, so no separate "begin
  // pin / end pin" phase is needed around the loop. Fast-path
  // entries (TID match) skip resolve entirely.
  uint32_t valid = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const BatchTarget &t = tgts[i];
    HANDLE tid_handle =
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(t.tid));

    // Fast path (see alert_one_if_live for rationale).
    WaitSlot &slot = get_slot(t.slot_idx);
    if (slot.thread_id.load(cpp::MemoryOrder::RELAXED) == t.tid) {
      out_filtered[valid++] = tid_handle;
      continue;
    }

    // Slow path: validate via registry.
    ThreadHandle h = ThreadHandle::unpack(t.owner_packed);
    if (!h.is_valid()) {
      // Pre-lifecycle fallback — accept without validation.
      out_filtered[valid++] = tid_handle;
      continue;
    }
    ThreadLifecycle *lc = registry_resolve(h);
    if (lc)
      out_filtered[valid++] = tid_handle;
  }
  if (valid > 0) {
    nt_optional().alert_multiple(
        out_filtered, valid,
        static_cast<PS_ALERT_THREAD_EXTENDED_PARAMETER *>(ab_ctx),
        ab_ctx_count);
  }
  return valid;
}

uint32_t alert_multiple_if_live_compact(const CompactTarget *tgts,
                                         uint32_t count, HANDLE *out_filtered,
                                         void *ab_ctx, uint32_t ab_ctx_count) {
  // Stack-steal: caller's walk CAS'd IN_KERNEL → SIGNALED_CLEAN,
  // which IS the state-transition authority. TID mismatch ⇒ owner
  // already woke on a stray alert + ran clear_slot_owned + freelist-
  // pushed; skipping is correct. No pin, no resolve, no bump.
  uint32_t valid = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const CompactTarget &t = tgts[i];
    WaitSlot &slot = get_slot(t.slot_idx);
    if (slot.thread_id.load(cpp::MemoryOrder::RELAXED) == t.tid) {
      out_filtered[valid++] =
          reinterpret_cast<HANDLE>(static_cast<uintptr_t>(t.tid));
    }
  }
  if (valid > 0) {
    nt_optional().alert_multiple(
        out_filtered, valid,
        static_cast<PS_ALERT_THREAD_EXTENDED_PARAMETER *>(ab_ctx),
        ab_ctx_count);
  }
  return valid;
}

} // namespace wait_slot
} // namespace LIBC_NAMESPACE_DECL

int LIBC_NAMESPACE::internal::wait_slot_startup_init() {
  LIBC_NAMESPACE::wait_slot::init();
  LIBC_NAMESPACE::wait_slot::install_subsystem_unlinkers();
  return 0;
}

void LIBC_NAMESPACE::internal::wait_slot_fork_reinit() {
  LIBC_NAMESPACE::wait_slot::fork_reinit();
}

// Phase 3 (NOT 4). $P4 subsystems (mapping_table, ofd_pool,
// file_pool, robust_pool) take Futexes in their destroy() paths,
// which route through WaitSlot::alloc(). Intra-bucket order within
// $P4 is undefined, so co-locating wait_slot with them risks
// freeing the TLS index mid-destroy. $P3 runs strictly later in
// the reverse sweep ($P4 dies first), so wait_slot outlives every
// futex user. lifecycle is also $P3 but order-independent within
// the bucket (lifecycle_fini doesn't consult wait_slot).
LIBC_REGISTER_FINI(3, wait_slot, &::LIBC_NAMESPACE::wait_slot::fini)

LIBC_REGISTER_FORK_REINIT(wait_slot,
                          ::LIBC_NAMESPACE::internal::kForkPrioWaitSlot,
                          &::LIBC_NAMESPACE::internal::wait_slot_fork_reinit)
