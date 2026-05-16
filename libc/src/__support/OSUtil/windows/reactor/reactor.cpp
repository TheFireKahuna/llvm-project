//===-- Process-wide IOCP reactor -- implementation -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single IOCP + drain thread pool that multiplexes all runtime-internal and
// application-facing (epoll) asynchronous events. The NT kernel's native
// completion model promoted to process-wide infrastructure.
//
// Completion key encoding (64-bit):
//   [gen_hi:15][tag:1][gen_lo:24][idx:24]
//   Bit 48 is always set -- impossible for a valid user-mode pointer.
//   Bits [23:0] are the slot index into the IndexedPool. Bits [47:24]
//   and [63:49] jointly carry the 32-bit generation counter split across
//   the tag bit. gen_lo holds bits [23:0] of the counter; gen_hi holds
//   bits [31:24] in its low 8 bits (the upper 7 bits of gen_hi are
//   reserved zero). Gen width exactly matches slot->generation
//   (uint32_t), so dispatch compares without masking either side.
//
// Slot allocation:
//   Reactor slots are dynamically allocated from an IndexedPool. Slot
//   memory lives at a stable VA for the slot's lifetime; the pool
//   releases physical pages via MEM_RESET when a chunk becomes empty,
//   preserving the VA reservation. A stale kernel-queued completion
//   key whose target slot was freed can therefore never fault on
//   dereference — MEM_RESET pages stay readable (zero-filled on
//   reclaim). IndexedPool's own occupancy bitmap doubles as the
//   iteration source for fini/fork_reinit, so no intrusive active list
//   is needed.
//
// Synchronization model:
//   - Registration (watch/unwatch): mutex-protected, rare (subsystem init)
//   - Dispatch (drain pool + epoll_wait inline): lock-free, hot path
//   - Per-slot exclusion: CAS on dispatching flag serializes callbacks for
//     the same registration across drain threads. Different registrations
//     dispatch concurrently. A loser drain thread publishes a
//     pending_redispatch flag via Dekker handoff so the current winner
//     re-runs the callback rather than dropping the completion.
//   - Unwatch safety: double-check generation + spin-wait on
//     `dispatch_inflight` refcount guarantees no dispatcher is still
//     touching the slot (including the post-release Dekker close) after
//     unwatch() returns
//
// Routing model:
//   Drain pool threads are the primary IOCP consumers. They dispatch
//   reactor-keyed completions via slot pointer lookup. Non-reactor completions
//   (epoll events) are forwarded to an installed CompletionRouter callback,
//   which pushes them to per-epoll-instance pending queues. epoll_wait also
//   does non-blocking IOCP flushes, dispatching reactor completions inline
//   via dispatch_inline().
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/indexed_pool.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_ipc_api.h"
#include "src/__support/OSUtil/windows/nt/nt_job.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/tls/teb_fixup.h"
#include "src/__support/threads/raw_mutex.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace reactor {

namespace {

// =========================================================================
// Constants
// =========================================================================

// Maximum completions dequeued per NtRemoveIoCompletionEx call.
constexpr ULONG DRAIN_BATCH_SIZE = 32;

// Drain thread stack size. Callbacks must be short (pend + return),
// so 64KB is generous.
constexpr SIZE_T DRAIN_STACK_SIZE = 0x10000;

// =========================================================================
// Completion key encoding
// =========================================================================
//
// The IOCP CompletionKey (PVOID, 64-bit) encodes a slot *index* into the
// reactor's IndexedPool plus a truncated generation counter.
//
// Layout: [gen_hi:15][tag:1][gen_lo:24][idx:24]
//
//   Bit 48:       reactor discriminant (always set -- impossible for a
//                 user-mode pointer on x86-64).
//   Bits [23:0]:  slot index (idx == 0 is the INVALID_TOKEN sentinel).
//   Bits [47:24]: gen_lo -- low 24 bits of the 32-bit generation counter.
//   Bits [63:49]: gen_hi -- high 8 bits of the generation in its low 8
//                 bits; top 7 bits are reserved zero.
//
// The full 32-bit generation is reconstructed as (gen_hi << 24) | gen_lo
// and compared directly against slot->generation (uint32_t) with no
// masking — the key carries the generation at the same width the slot
// stores it. Rollover at 2^32 is the only ABA window; at 1 M watches/s
// that's ~71 minutes of continuous churn, well past any realistic IOCP
// entry residency.
//
// Using a slot index rather than a pointer means a stale completion
// dereferences through IndexedPool::slot_for(idx), which returns a
// pointer into VA that the pool keeps reserved for the slot's lifetime
// — so no fault, even when the chunk's physical backing has been
// returned to the OS via MEM_RESET.

constexpr unsigned KEY_GEN_HI_SHIFT = 49;
constexpr uintptr_t KEY_GEN_HI_MASK = 0x7FFFULL;    // 15 bits, top 7 reserved
constexpr unsigned KEY_GEN_LO_SHIFT = 24;
constexpr uintptr_t KEY_GEN_LO_MASK = 0xFFFFFFULL;  // 24 bits
constexpr uintptr_t KEY_IDX_MASK = 0xFFFFFFULL;     // 24 bits

PVOID pack_key(uint32_t idx, uint32_t generation) {
  uintptr_t gen_hi = (generation >> 24) & KEY_GEN_HI_MASK;
  uintptr_t gen_lo = generation & KEY_GEN_LO_MASK;
  return reinterpret_cast<PVOID>(REACTOR_KEY_TAG |
                                  (gen_hi << KEY_GEN_HI_SHIFT) |
                                  (gen_lo << KEY_GEN_LO_SHIFT) |
                                  (static_cast<uintptr_t>(idx) & KEY_IDX_MASK));
}

uint32_t unpack_idx(PVOID key) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key) & KEY_IDX_MASK);
}

uint32_t unpack_generation(PVOID key) {
  uintptr_t k = reinterpret_cast<uintptr_t>(key);
  uint32_t lo = static_cast<uint32_t>((k >> KEY_GEN_LO_SHIFT) & KEY_GEN_LO_MASK);
  uint32_t hi = static_cast<uint32_t>((k >> KEY_GEN_HI_SHIFT) & KEY_GEN_HI_MASK);
  return (hi << 24) | lo;
}

// =========================================================================
// Slot types
// =========================================================================

enum SlotKind : uint8_t {
  SLOT_FREE = 0, // Available for allocation (or deferred-free pending).
  SLOT_WCP = 1,  // WaitCompletionPacket-based watch.
  SLOT_ALPC = 2, // ALPC completion port association.
  SLOT_JOB = 3,  // Job object IOCP association.
};

// =========================================================================
// Reactor slot -- one per registration, IndexedPool-allocated
// =========================================================================
//
// Slots are allocated by claiming a free bit in the pool's per-chunk
// occupancy bitmap and released via IndexedPool::mark_dead(). Slot VA
// stays valid for the process lifetime; the pool trims physical backing
// via MEM_RESET when a chunk drains to zero live slots. Iteration for
// fini/fork_reinit goes through IndexedPool::for_each_live, keyed by
// the same bitmap — no intrusive list.

} // anonymous namespace (SlotKind needs to be visible to ReactorSlot)

struct ReactorSlot {
  ReactorCallback callback; // Dispatch target.
  void *context;            // Opaque context for callback.
  HANDLE target;            // Watched handle (WCP) or ALPC port.
  HANDLE wcp;               // WaitCompletionPacket handle (null for ALPC).
  HANDLE reserve;           // Per-slot MemoryReserveIoCompletion handle. Used
                            // by watch()/rearm() to manually post an IOCP
                            // entry when NtAssociateWaitCompletionPacket
                            // returns AlreadySignaled=TRUE — the kernel
                            // sampled the wait object as signaled but did
                            // NOT queue a packet, so we queue one ourselves
                            // to keep the wake from being lost. SLOT_WCP
                            // only; null for SLOT_ALPC / SLOT_JOB.

  // Atomic generation counter. Incremented on every unwatch/detach.
  // The drain thread compares this 32-bit value directly against the
  // 32-bit generation decoded from the completion key (no masking —
  // the key carries the full width) to detect stale completions.
  cpp::Atomic<uint32_t> generation{0};

  // True while a callback is executing on the drain thread. Taken via CAS
  // on dispatcher entry (Step 3) and released at Step 6 (and may be briefly
  // retaken during the Dekker close at Step 6b). Narrower-lived than
  // `dispatch_inflight` — flickers off between Step 6 release and a
  // possible Step 6b retake. Use `dispatch_inflight` for the "a dispatcher
  // is still touching this slot" wait condition.
  cpp::Atomic<bool> dispatching{false};

  // Set by a loser drain thread whose CAS on `dispatching` failed — i.e.
  // a new IOCP entry for this slot arrived while another drain thread was
  // already running the callback. The winning dispatcher re-invokes the
  // callback before releasing `dispatching` (and again after release, as
  // the Dekker close). This replaces the previous "drop the completion on
  // CAS fail" behaviour that silently lost wakes when the AlreadySignaled
  // handler or the WCP rearm posted an IOCP entry during the callback.
  cpp::Atomic<bool> pending_redispatch{false};

  // Refcount covering the full window of slot access by a dispatcher —
  // from Step 3 CAS success (or loser-becomes-winner) through Step 7
  // deferred free. Spans the Dekker-close load/retake after `dispatching`
  // has been briefly released, which `dispatching` alone does not cover.
  // unwatch() spin-waits on `dispatch_inflight == 0` instead of on
  // `dispatching == false` to guarantee no dispatcher is still touching
  // the slot when unwatch proceeds to set SLOT_FREE and call free_slot(idx).
  cpp::Atomic<uint32_t> dispatch_inflight{0};

  // Slot kind. Written under g_lock by watch/unwatch/detach and outside
  // any lock by fork_reinit/drain_cleanup_slots (single-threaded contexts).
  // Read lock-free at dispatch_reactor_completion Step 7 to detect the
  // detach-inside-callback deferred-free signal.
  //
  // Atomic: detach's RELEASE store pairs with Step 7's ACQUIRE load so
  // the lock-free reader has a formally synchronised view. All lock-held
  // accesses use RELAXED — the g_lock critical section provides the
  // happens-before with any other lock-held reader, and cross-thread
  // visibility to the one lock-free reader (Step 7) is established
  // through the explicit ACQUIRE/RELEASE pair.
  //
  // Type is uint32_t rather than uint8_t: Atomic<uint8_t> would rely on
  // sub-word RMW codegen support; uint32_t is uniformly supported and
  // costs one extra word at no runtime penalty on x86-64 / AArch64.
  cpp::Atomic<uint32_t> kind{SLOT_FREE};
};

namespace {

// =========================================================================
// File-local statics — implementation-private, NOT in the PCB
// =========================================================================
//
// The PCB holds fixed kernel objects and metadata (iocp, reserve,
// drain_thread, shutdown, drain_cleaned, heartbeat, generation, router)
// that represent observable process-wide state. The slot pool and
// registration lock are implementation mechanisms that no external
// subsystem accesses — they stay file-local.
//
// ChunkShift=10 → 1024 slots per chunk. A typical process with a handful
// of reactor registrations (waitpid, alpc_bus, itimer, epoll WCPs) lives
// entirely in chunk 0; additional chunks demand-allocate on overflow.
constexpr unsigned REACTOR_CHUNK_SHIFT = 10;
using ReactorPool = internal::IndexedPool<ReactorSlot, REACTOR_CHUNK_SHIFT>;

ReactorPool g_slot_pool;
RawMutex g_lock;

// Alloc-scan hint. Seeded by the last successful allocation's chunk
// index so the next allocator starts where the previous one finished
// rather than rescanning chunk 0 every time. RELAXED: the hint is a
// pure performance optimisation; a stale read causes at most one extra
// chunk probe, never a correctness issue. Reset to 0 in fork_reinit.
cpp::Atomic<uint32_t> g_next_scan_chunk{0};

// Validate CompletionRouter ↔ uintptr_t ABI compatibility. The PCB stores
// the router as Atomic<uintptr_t> to avoid pulling reactor.h into
// process_control_block.h while providing atomic access.
static_assert(sizeof(CompletionRouter) == sizeof(uintptr_t),
              "CompletionRouter must be pointer-sized for PCB uintptr_t storage");

// =========================================================================
// Drain thread handle accessor
// =========================================================================
//
// ReactorState stores drain_thread (index 0) followed by
// drain_threads_reserved[MAX-1] (indices 1..MAX-1). This helper provides
// uniform indexed access without UB (no pointer arithmetic across members).

HANDLE &drain_handle(ReactorState &rr, uint32_t i) {
  return i == 0 ? rr.drain_thread : rr.drain_threads_reserved[i - 1];
}

// =========================================================================
// Active processor count (from KUSER_SHARED_DATA)
// =========================================================================
//
// KUSER_SHARED_DATA is mapped read-only at a fixed VA in every process.
// Offset 0x03C0: ActiveProcessorCount — same source ntdll uses internally.

uint32_t active_processor_count() {
  return *reinterpret_cast<const volatile uint32_t *>(0x7FFE03C0ull);
}

// =========================================================================
// Slot allocation
// =========================================================================
//
// Walks the IndexedPool directory, pins each chunk, and scans the
// occupancy bitmap for a free bit. `try_acquire` on the bitmap is the
// atomic claim — the winner gains exclusive ownership of the slot and
// inherits the scan-pin as the slot's contribution to the chunk's
// live_count (matching region_pool::scan_chunk). idx == 0 is reserved
// as the INVALID_TOKEN sentinel.
//
// Returns nullptr on failure (pool exhausted or chunk alloc failure).
// On success, returns the slot pointer and stores its idx in *idx_out.
//
// Wraps around from `g_next_scan_chunk` so sustained alloc/free churn
// reuses the chunk the previous allocator settled on rather than
// restarting at chunk 0 each time. Matches region_pool::scan_chunk.
ReactorSlot *alloc_slot(uint32_t *idx_out) {
  constexpr unsigned DIRECTORY_CAP = ReactorPool::DIRECTORY_CAPACITY;
  uint32_t start = g_next_scan_chunk.load(cpp::MemoryOrder::RELAXED);
  if (start >= DIRECTORY_CAP)
    start = 0;

  for (unsigned offset = 0; offset < DIRECTORY_CAP; ++offset) {
    unsigned ci = (start + offset) % DIRECTORY_CAP;
    ReactorSlot *chunk_base = g_slot_pool.acquire_for_scan(ci);
    if (!chunk_base)
      return nullptr;

    auto *meta = ReactorPool::meta_for(chunk_base);

    for (unsigned w = 0; w < ReactorPool::BITMAP_WORDS; ++w) {
      uint64_t occupied =
          meta->bitmap.template word_at<cpp::MemoryOrder::ACQUIRE>(w);
      uint64_t free_bits = ~occupied;

      // Reserve idx 0 (chunk 0, local 0) as the INVALID_TOKEN sentinel.
      if (ci == 0 && w == 0)
        free_bits &= ~uint64_t{1};

      while (free_bits != 0) {
        unsigned b = static_cast<unsigned>(__builtin_ctzll(free_bits));
        unsigned local_idx = w * 64u + b;

        if (!meta->bitmap.try_acquire(local_idx)) {
          free_bits &= free_bits - 1;
          continue;
        }

        // Won the bit. The scan-pin (live_count bump from
        // acquire_for_scan) becomes this slot's live reference — matching
        // region_pool's scan_chunk pattern. Record the winning chunk
        // as the next scan hint (only on a genuine move, to avoid a
        // write on the common same-chunk fast path).
        if (ci != start)
          g_next_scan_chunk.store(ci, cpp::MemoryOrder::RELAXED);
        *idx_out = (ci << ReactorPool::CHUNK_SHIFT) | local_idx;
        return &chunk_base[local_idx];
      }
    }

    // No free slot in this chunk — release the scan pin and move on.
    g_slot_pool.release_scan_ref(ci);
  }
  return nullptr;
}

// Return a slot to the pool. Clears the occupancy bit and decrements
// the chunk's live_count; on last-free, the chunk's data pages are
// MEM_RESET back to the OS. Slot VA remains reserved.
void free_slot(uint32_t idx) {
  g_slot_pool.mark_dead(idx);
}

// =========================================================================
// Shared dispatch logic
// =========================================================================
//
// Used by both drain pool threads and dispatch_inline(). Core correctness
// mechanism: CAS on `dispatching` for per-slot exclusion, generation
// re-check to catch unwatch/detach races, and Dekker handoff via
// `pending_redispatch` so a loser drain thread never drops an IOCP entry.
//
// Loser protocol (Dekker, SEQ_CST on both paired ops):
//   LOSER:  pending.store(true, SEQ_CST)
//           dispatching.CAS(false→true, SEQ_CST)
//             success ⇒ become the new winner, clear our own pending
//             failure ⇒ return; current winner's post-callback check or
//                       Dekker-close re-check will observe our pending
//   WINNER: inner loop: callback(); if pending.exchange(false)==true, loop
//           release: dispatching.store(false, SEQ_CST)
//           Dekker close: if pending.load==true, retry-CAS; if success
//                         rerun inner loop; else another drain will handle it
//
// Pairing guarantees: under total SEQ_CST order, either the loser's
// pending.store is observed by the winner's exchange (or Dekker-close load)
// OR the winner's dispatching.store(false) is observed by the loser's CAS
// (letting the loser become the new winner). No completion is ever dropped.

// Inner body of dispatch: runs the outer Dekker-handoff re-dispatch loop
// assuming the caller already holds `slot->dispatching == true`. Returns
// with dispatching cleared. All exits (normal, gen-mismatch from unwatch
// or detach-inside-callback) flow out of this helper so the caller can
// uniformly handle deferred SLOT_FREE cleanup (Step 7) at one site.
LIBC_INLINE void
run_dispatch_outer_loop(ReactorSlot *slot, NTSTATUS status,
                         ULONG_PTR information, uint32_t key_gen) {
  // Outer loop: re-dispatch as long as losers keep accumulating work in
  // the pending flag. Each iteration ends with a SEQ_CST release of
  // dispatching followed by a Dekker-close check; if a loser's store
  // raced the release, we re-take the slot and loop. Livelock-safe: each
  // iteration consumes at most one logical "round" of accumulated
  // losers, bounded by the number of IOCP entries actually queued for
  // this slot.
  bool expected;
  for (;;) {
    // Step 4: Generation re-check after acquiring dispatch rights.
    // Catches unwatch/detach that bumped generation between Steps 1-3.
    if (slot->generation.load(cpp::MemoryOrder::ACQUIRE) != key_gen) {
      slot->dispatching.store(false, cpp::MemoryOrder::RELEASE);
      return;
    }

    // Step 5: Dispatch loop. exchange(false) atomically reads-and-clears
    // the pending flag; if another drain thread set it during the
    // callback, we re-run. Callbacks must be idempotent —
    // ioring_cq_waker (alert_if_parked + rearm), ALPC/JOB drain-all
    // callbacks all satisfy this.
    for (;;) {
      slot->callback(slot->context, status, information);
      if (!slot->pending_redispatch.exchange(false,
                                             cpp::MemoryOrder::ACQ_REL))
        break;
      // Generation may have changed while we were inside the callback
      // (detach from inside the callback, or unwatch from another thread).
      if (slot->generation.load(cpp::MemoryOrder::ACQUIRE) != key_gen) {
        slot->dispatching.store(false, cpp::MemoryOrder::RELEASE);
        return;
      }
    }

    // Step 6: Release dispatching. SEQ_CST pairs with the loser's
    // SEQ_CST pending.store + SEQ_CST CAS — guarantees any loser whose
    // store is ordered before our release is observable by our
    // Dekker-close load, and any loser whose CAS is ordered after our
    // release sees dispatching cleared and becomes the new winner
    // itself.
    slot->dispatching.store(false, cpp::MemoryOrder::SEQ_CST);

    // Step 6b: Dekker close. A loser may have set pending AFTER our
    // final exchange in Step 5 but BEFORE our release above. That
    // loser's CAS would have failed (we still held dispatching), so it
    // returned expecting us to handle its request. Re-check and re-take
    // the slot.
    if (!slot->pending_redispatch.load(cpp::MemoryOrder::SEQ_CST))
      return; // Clean exit — no further work.
    expected = false;
    if (!slot->dispatching.compare_exchange_strong(
            expected, true, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::RELAXED))
      return; // Another drain won the retry CAS; it will handle pending.
    // Do not clear the pending flag: the inner-loop exchange on the
    // next iteration will consume it atomically. Clearing here would
    // race a concurrent loser's SEQ_CST store (see the
    // loser-becomes-winner path in the caller for the same reason).
    // Loop back to Step 4 — we're the winner again for this round.
  }
}

void dispatch_reactor_completion(PVOID key, NTSTATUS status,
                                 ULONG_PTR information) {
  if (!(reinterpret_cast<uintptr_t>(key) & REACTOR_KEY_TAG))
    return;

  uint32_t idx = unpack_idx(key);
  if (idx == 0)
    return; // INVALID_TOKEN sentinel — never a live registration.

  // slot_for returns nullptr if the chunk has never been allocated. A
  // stale key for an allocated-then-never-touched chunk still resolves,
  // but its memory is either the original (gen mismatch) or MEM_RESET
  // zeros (gen mismatch) — both rejected by the gen check below.
  ReactorSlot *slot = g_slot_pool.slot_for(idx);
  if (!slot)
    return;

  uint32_t key_gen = unpack_generation(key);

  // Step 1-2: Generation check (lock-free, pre-CAS fast bail).
  uint32_t slot_gen = slot->generation.load(cpp::MemoryOrder::ACQUIRE);
  if (slot_gen != key_gen)
    return;

  // Step 3: Acquire exclusive dispatch rights via CAS. On failure, use the
  // Dekker loser protocol: publish pending and retry the CAS exactly once
  // to cover the window where the current winner released between our first
  // CAS and our pending.store.
  bool expected = false;
  if (!slot->dispatching.compare_exchange_strong(
          expected, true, cpp::MemoryOrder::ACQ_REL,
          cpp::MemoryOrder::RELAXED)) {
    slot->pending_redispatch.store(true, cpp::MemoryOrder::SEQ_CST);
    expected = false;
    if (!slot->dispatching.compare_exchange_strong(
            expected, true, cpp::MemoryOrder::SEQ_CST,
            cpp::MemoryOrder::SEQ_CST))
      return; // Winner will observe our pending and re-dispatch.
    // We took the slot as the new winner. Our own pending flag is still
    // true (from the SEQ_CST store above) and will be consumed atomically
    // by the inner-loop exchange below — one spurious extra callback
    // iteration at most. We must NOT clear it here: a concurrent loser Z
    // may have stored pending=true after our Y_l2 but before any clear,
    // and the modification order of the atomic admits interleavings that
    // would let our clear shadow Z's SEQ_CST store. Idempotent callbacks
    // make the extra iteration a cost, not a bug.
  }

  // Pin the slot against unwatch for the full duration of the dispatch —
  // including the Dekker-close load/retake which happens AFTER `dispatching`
  // is briefly released at Step 6. unwatch() spin-waits on this counter.
  slot->dispatch_inflight.fetch_add(1, cpp::MemoryOrder::ACQ_REL);

  // Run the Dekker-handoff re-dispatch loop. Returns with dispatching
  // cleared; any gen-mismatch exit caused by unwatch leaves
  // kind != SLOT_FREE (unwatch sets SLOT_FREE after its own spin-wait on
  // `dispatch_inflight`, which follows our decrement below), so Step 7's
  // check is a safe no-op on the unwatch path; detach-inside-callback
  // sets SLOT_FREE before bumping generation, so Step 7 correctly frees
  // the slot on the detach path.
  run_dispatch_outer_loop(slot, status, information, key_gen);

  // Step 7: Deferred free if detach() fired during any callback invocation.
  // Read `kind` BEFORE the refcount decrement so the read happens while
  // the slot is still pinned against unwatch — unwatch's spin is gated on
  // `dispatch_inflight == 0`, so it cannot set SLOT_FREE or reclaim the
  // slot underneath us yet.
  //
  // Multiple dispatchers can overlap when a second drain thread CASes
  // `dispatching` between our Step 6 release and our Step 7 (it gets its
  // own +1 on `dispatch_inflight`). Only the last one out — the thread
  // whose decrement transitions the counter to zero — owns the free.
  // Earlier exiters observed `kind == SLOT_FREE` too, but skip the free
  // to avoid a double free.
  // ACQUIRE pairs with detach's RELEASE store on the SLOT_FREE
  // transition: ensures any slot-state writes detach did before setting
  // SLOT_FREE are visible here. The only other writer that could race
  // this lock-free read is unwatch's post-spin store, but that store is
  // gated on `dispatch_inflight == 0` — and our own +1 still holds the
  // counter nonzero, so unwatch cannot have written yet.
  bool was_free =
      (slot->kind.load(cpp::MemoryOrder::ACQUIRE) == SLOT_FREE);
  uint32_t prev = slot->dispatch_inflight.fetch_sub(
      1, cpp::MemoryOrder::ACQ_REL);
  if (prev == 1 && was_free)
    free_slot(idx);
  // After the decrement, unwatch (if any) may observe refcount == 0 and
  // proceed to set SLOT_FREE + free_slot; we must not touch the slot
  // beyond this point.
}

// Route a single completion: reactor-keyed -> dispatch, else -> router.
// router_fn is the CompletionRouter stored as uintptr_t in the PCB; cast
// here to recover the function pointer type.
void route_completion(const FILE_IO_COMPLETION_INFORMATION &ci,
                      uintptr_t router_fn) {
  PVOID key = ci.KeyContext;

  // Null key = wakeup sentinel (shutdown, manual wake).
  if (!key)
    return;

  if (is_reactor_key(key)) {
    dispatch_reactor_completion(key, ci.IoStatusBlock.Status,
                                ci.IoStatusBlock.Information);
  } else if (router_fn) {
    reinterpret_cast<CompletionRouter>(router_fn)(
        key, ci.ApcContext, ci.IoStatusBlock.Status,
        ci.IoStatusBlock.Information);
  }
  // If no router installed, non-reactor completions are silently dropped.
  // This is correct: before epoll is initialized, there are no epoll
  // completions on the IOCP.
}

// =========================================================================
// Drain thread self-cleanup
// =========================================================================
//
// The drain thread owns slot cleanup on shutdown. This eliminates the
// need for fini() to ever touch slots, removing the entire class of
// "fini frees while drain thread references" races. The drain thread
// is the last consumer of slots — it cleans up after itself.

void drain_cleanup_slots() {
  g_lock.lock();
  // Walk every live slot via the IndexedPool's occupancy bitmap.
  // for_each_live pins each chunk during iteration so concurrent
  // mark_dead from the callback is safe: the pin prevents the chunk's
  // live_count from transiently hitting zero before the cb finishes.
  auto cb = +[](unsigned idx, ReactorSlot *slot, void *) {
    uint32_t k = slot->kind.load(cpp::MemoryOrder::RELAXED);
    if (k == SLOT_WCP && slot->wcp) {
      ::NtCancelWaitCompletionPacket(slot->wcp, 1);
      ::NtClose(slot->wcp);
      slot->wcp = nullptr;
    }
    if (slot->reserve) {
      ::NtClose(slot->reserve);
      slot->reserve = nullptr;
    }
    slot->kind.store(SLOT_FREE, cpp::MemoryOrder::RELAXED);
    // Bump generation so any in-flight kernel-queued completion is
    // rejected on the drain-thread's gen check.
    slot->generation.fetch_add(1, cpp::MemoryOrder::RELEASE);
    g_slot_pool.mark_dead(idx);
  };
  g_slot_pool.for_each_live(cb, nullptr);
  g_lock.unlock();
}

// =========================================================================
// Drain thread
// =========================================================================

NTAPI DWORD drain_thread_proc(PVOID param) {
  // Reserve handler-stack budget — drain dispatches user-installed
  // completion routers (epoll callbacks, timer callbacks, AIO completion)
  // which can recursively trigger more dispatch and overflow the stack
  // on adversarial workloads.
  windows::apply_libc_stack_guarantee();

  uint32_t my_index =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param));
  ReactorState &rr = g_pcb.reactor;
  FILE_IO_COMPLETION_INFORMATION entries[DRAIN_BATCH_SIZE];

  while (!rr.shutdown.load(cpp::MemoryOrder::ACQUIRE)) {
    ULONG count = 0;
    NTSTATUS st = ::NtRemoveIoCompletionEx(
        rr.iocp, entries, DRAIN_BATCH_SIZE, &count,
        nullptr, // Infinite wait -- woken by completions or shutdown post.
        1);   // Alertable -- allows APC delivery to this thread.

    if (st == STATUS_USER_APC || st == STATUS_ALERTED)
      continue;

    if (!NT_SUCCESS(st))
      continue;

    // Snapshot the router once per batch (ACQUIRE pairs with RELEASE in
    // set_completion_router). A single load per batch is sufficient — the
    // router is set once during epoll init and never changed afterward.
    uintptr_t router_fn = rr.router.load(cpp::MemoryOrder::ACQUIRE);
    for (ULONG i = 0; i < count; ++i)
      route_completion(entries[i], router_fn);

    // Bump per-thread epoch (RCU fence target) and global heartbeat.
    // Per-thread epoch: fence_drain_cycle() snapshots and waits for
    // each thread's epoch to advance, giving an airtight guarantee
    // that every thread has cycled. Cache-line aligned, no contention.
    // Global heartbeat: drain_thread_healthy() checks any-thread
    // progress — simpler interface for health monitoring.
    rr.drain_epochs[my_index].value.fetch_add(1, cpp::MemoryOrder::RELEASE);
    rr.heartbeat.fetch_add(1, cpp::MemoryOrder::RELAXED);

    // Wake any fence_drain_cycle() caller parked on this epoch.
    // Hot-path cost when no waiter: hash + bucket.live_count load → 0
    // → return. One extra cache-line read per batch, negligible.
    futex_addr::wake(
        reinterpret_cast<volatile uint32_t *>(
            &rr.drain_epochs[my_index].value.val),
        1);
  }

  // Shutdown: coordinate exit across drain pool threads.
  // The last thread out owns cleanup — all others have left the dispatch
  // loop, so no concurrent dispatch is possible after this point.
  uint32_t prev = rr.drain_exit_count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  if (prev + 1 == rr.drain_thread_count) {
    drain_cleanup_slots();
    rr.drain_cleaned.store(1, cpp::MemoryOrder::RELEASE);
    // Wake fini() if it's parked in futex_addr::wait on drain_cleaned.
    futex_addr::wake(&rr.drain_cleaned, 1);
  }

  return 0;
}

// =========================================================================
// Drain thread lifecycle
// =========================================================================

HANDLE start_drain_thread(uint32_t index) {
  HANDLE thread = nullptr;
  auto oa = windows::internal_oa();
  NTSTATUS st = ::NtCreateThreadEx(
      &thread,
      THREAD_ALL_ACCESS,
      &oa,               // Non-inheritable — internal drain thread.
      NtCurrentProcess(),
      reinterpret_cast<PVOID>(drain_thread_proc),
      reinterpret_cast<PVOID>(static_cast<uintptr_t>(index)),
      THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH |
          THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER,
      0,                 // ZeroBits
      DRAIN_STACK_SIZE,
      DRAIN_STACK_SIZE,
      nullptr);          // AttributeList
  if (!NT_SUCCESS(st))
    return nullptr;

  // Thread is created running (not suspended). SKIP_THREAD_ATTACH prevents
  // the loader from sending DLL_THREAD_ATTACH notifications, avoiding a
  // deadlock when this is called during DLL_PROCESS_ATTACH (loader lock held).
  // HIDE_FROM_DEBUGGER keeps the drain thread out of casual debugger views.
  return thread;
}

void wake_drain_thread(ReactorState &rr) {
  if (rr.iocp && rr.reserve) {
    ::NtSetIoCompletionEx(rr.iocp, rr.reserve,
                          nullptr, // Null key = wakeup sentinel.
                          nullptr, STATUS_SUCCESS, 0);
  }
}

// No-op APC routine. Delivery itself is the effect — it causes any
// alertable wait to return STATUS_USER_APC, breaking the drain loop
// back to the shutdown check.
NTAPI void shutdown_apc(PVOID, PVOID, PVOID) {}

} // anonymous namespace

// =========================================================================
// Public API -- Lifecycle
// =========================================================================

int init() {
  ReactorState &rr = g_pcb.reactor;

  // First-time init: start generation at 1 so generation 0 is never assigned
  // (matches zero-init state of freed slots for stale-completion rejection).
  // After fork, the child inherits the parent's counter — this is a no-op.
  if (rr.generation.load(cpp::MemoryOrder::RELAXED) == 0)
    rr.generation.store(1, cpp::MemoryOrder::RELAXED);

  // Create the process-wide IOCP. NumberOfConcurrentThreads=0 (unlimited)
  // because both the drain thread and epoll_wait threads may dequeue
  // concurrently during non-blocking flush operations.
  auto iocp_oa = windows::internal_oa();
  NTSTATUS st = ::NtCreateIoCompletion(&rr.iocp, IO_COMPLETION_ALL_ACCESS,
                                       &iocp_oa, 0);
  if (!NT_SUCCESS(st))
    return -1;

  // Allocate reserve object for guaranteed-delivery posts (shutdown
  // wakeup, synthetic epoll completions). Cannot fail under memory pressure.
  auto rsv_oa = windows::internal_oa();
  st = ::NtAllocateReserveObject(&rr.reserve, &rsv_oa,
                                 MemoryReserveIoCompletion);
  if (!NT_SUCCESS(st)) {
    ::NtClose(rr.iocp);
    rr.iocp = nullptr;
    return -1;
  }

  // Initialize the slot pool. Slot size is fixed at compile time by the
  // IndexedPool template instantiation (sizeof(ReactorSlot)). No TLS —
  // alloc goes through a chunk-walking scan that's only taken on the
  // registration path (rare; under the reactor's own g_lock).
  g_slot_pool.init();

  // Start the drain pool. Size is clamped to [2, MAX_DRAIN_THREADS]:
  //   - Floor of 2 gives resilience against a single stuck callback
  //   - Ceiling of MAX keeps resource use bounded
  // IOCP naturally load-balances completions across waiting threads.
  rr.shutdown.store(0, cpp::MemoryOrder::RELEASE);
  rr.drain_exit_count.store(0, cpp::MemoryOrder::RELAXED);

  uint32_t n = active_processor_count();
  if (n < 2)
    n = 2;
  if (n > ReactorState::MAX_DRAIN_THREADS)
    n = ReactorState::MAX_DRAIN_THREADS;
  rr.drain_thread_count = n;

  for (uint32_t i = 0; i < n; ++i) {
    drain_handle(rr, i) = start_drain_thread(i);
    if (!drain_handle(rr, i)) {
      // Failed to start thread i. If we have at least one thread,
      // proceed with a reduced pool. Otherwise, fail init entirely.
      rr.drain_thread_count = i;
      if (i == 0) {
        ::NtClose(rr.reserve);
        rr.reserve = nullptr;
        ::NtClose(rr.iocp);
        rr.iocp = nullptr;
        return -1;
      }
      break;
    }
  }

  return 0;
}

void fini() {
  ReactorState &rr = g_pcb.reactor;
  rr.shutdown.store(1, cpp::MemoryOrder::RELEASE);

  if (rr.drain_thread_count > 0) {
    // Wake all drain threads through both available mechanisms:
    //   - IOCP sentinels: one per thread, wakes NtRemoveIoCompletionEx
    //   - APCs: one per thread, wakes any alertable wait inside a callback
    for (uint32_t i = 0; i < rr.drain_thread_count; ++i) {
      wake_drain_thread(rr);
      if (drain_handle(rr, i))
        ::NtQueueApcThreadEx2(drain_handle(rr, i), nullptr,
                              QUEUE_USER_APC_FLAGS_NONE, shutdown_apc,
                              nullptr, nullptr, nullptr);
    }

    // Adaptive convergence: the last drain thread to exit cleans up all
    // active slots, stores drain_cleaned=1, and wakes us via futex. A
    // healthy pool reaches that point in microseconds after being woken.
    //
    //   Phase 1: Hardware-monitored spin (UMWAIT/MWAITX if available,
    //            wakes on cache-line write — near-zero power, sub-μs)
    //   Phase 2: futex_addr park on drain_cleaned (covers scheduling
    //            delays and in-flight callbacks, 100ms timeout bound)
    //
    // If all drain threads are stuck in buggy callbacks, they never
    // reach exit coordination. Slots are abandoned — the OS reclaims
    // all process memory on exit. No force-termination, no corruption.

    // Phase 1: Hardware spin on drain_cleaned (value 0 → non-zero).
    spin_wait::spin_on_raw(&rr.drain_cleaned.val, 0u);

    // Phase 2: If the spin didn't converge, park in the futex parking
    // lot. The last drain thread out stores drain_cleaned=1 and calls
    // futex_addr::wake to unpark us.
    if (!rr.drain_cleaned.load(cpp::MemoryOrder::ACQUIRE)) {
      LARGE_INTEGER timeout;
      timeout.QuadPart = -1000000LL; // 100ms relative.
      futex_addr::wait_nt(&rr.drain_cleaned, 0u, &timeout);
    }

    // Close all drain thread handles.
    for (uint32_t i = 0; i < rr.drain_thread_count; ++i) {
      if (drain_handle(rr, i)) {
        ::NtClose(drain_handle(rr, i));
        drain_handle(rr, i) = nullptr;
      }
    }
    rr.drain_thread_count = 0;
  }

  if (rr.reserve) {
    ::NtClose(rr.reserve);
    rr.reserve = nullptr;
  }
  if (rr.iocp) {
    ::NtClose(rr.iocp);
    rr.iocp = nullptr;
  }

  rr.router.store(0, cpp::MemoryOrder::RELAXED);
}

void fork_reinit() {
  ReactorState &rr = g_pcb.reactor;

  // In the fork child, all reactor handles (IOCP, reserve, WCPs, drain
  // threads) were created with internal_oa() (non-inheritable), so they
  // don't exist in the child's handle table. Null all pointers.
  for (uint32_t i = 0; i < rr.drain_thread_count; ++i)
    drain_handle(rr, i) = nullptr;
  rr.drain_thread_count = 0;

  // Walk every live slot via the IndexedPool's occupancy bitmap and
  // return each to the pool. The child inherits no kernel handles (IOCP,
  // WCPs, reserves were all created non-inheritable), so null the handle
  // fields to match reality and mark_dead to clear the bitmap. Bumping
  // generation ensures any stale kernel-queued completion (if there
  // were one) would be rejected on gen mismatch — in fork, the whole
  // IOCP is gone, but belt-and-suspenders.
  //
  // Post-fork is single-threaded, so for_each_live's chunk-pin +
  // in-cb mark_dead pattern is safe.
  auto cb = +[](unsigned idx, ReactorSlot *slot, void *) {
    slot->wcp = nullptr;
    slot->reserve = nullptr;
    slot->generation.fetch_add(1, cpp::MemoryOrder::RELAXED);
    slot->kind.store(SLOT_FREE, cpp::MemoryOrder::RELAXED);
    g_slot_pool.mark_dead(idx);
  };
  g_slot_pool.for_each_live(cb, nullptr);

  // Reset IndexedPool internals: per-chunk transition locks, directory
  // lock, and reclaim any chunks that drained during the walk above.
  g_slot_pool.fork_reinit();

  // Reset the alloc-scan hint so the child starts fresh at chunk 0.
  g_next_scan_chunk.store(0, cpp::MemoryOrder::RELAXED);

  rr.reserve = nullptr;
  rr.iocp = nullptr;

  g_lock.reset_for_fork();

  // Reset all runtime flags for the child.
  rr.shutdown.store(0, cpp::MemoryOrder::RELAXED);
  rr.drain_exit_count.store(0, cpp::MemoryOrder::RELAXED);
  rr.drain_cleaned.store(0, cpp::MemoryOrder::RELAXED);
  // Preserve the router -- epoll reinstalls it in its fork_reinit.
  // (Actually, the function pointer is still valid across fork.)

  // init() reinitializes the slot pool (init + init_tls) and starts
  // a fresh drain pool for the child.
  init();
}

// =========================================================================
// Public API -- Watch Registration
// =========================================================================

namespace {

// Shared slot-init: zero transient state and publish `gen` as the slot's
// live generation. Called before any field that a dispatcher might read
// is written, and before the slot is associated with the kernel IOCP.
LIBC_INLINE void init_slot_fields(ReactorSlot *slot, uint32_t gen) {
  slot->callback = nullptr;
  slot->context = nullptr;
  slot->target = nullptr;
  slot->wcp = nullptr;
  slot->reserve = nullptr;
  slot->generation.store(gen, cpp::MemoryOrder::RELAXED);
  slot->dispatching.store(false, cpp::MemoryOrder::RELAXED);
  slot->pending_redispatch.store(false, cpp::MemoryOrder::RELAXED);
  slot->dispatch_inflight.store(0, cpp::MemoryOrder::RELAXED);
  slot->kind.store(SLOT_FREE, cpp::MemoryOrder::RELAXED);
}

} // namespace

ReactorToken watch(HANDLE waitable, ReactorCallback cb, void *context) {
  ReactorState &rr = g_pcb.reactor;
  if (!rr.iocp || !cb || !waitable)
    return INVALID_TOKEN;

  if (rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return INVALID_TOKEN;

  uint32_t idx = 0;
  ReactorSlot *slot = alloc_slot(&idx);
  if (!slot)
    return INVALID_TOKEN;

  // Assign a globally unique generation before any field init. A stale
  // completion from a prior occupant of this idx will carry a different
  // key-generation, rejected by dispatch_reactor_completion's gen check.
  uint32_t gen = rr.generation.fetch_add(1, cpp::MemoryOrder::RELAXED);
  init_slot_fields(slot, gen);

  g_lock.lock();

  HANDLE wcp = nullptr;
  auto wcp_oa = windows::internal_oa();
  NTSTATUS st = ::NtCreateWaitCompletionPacket(
      &wcp, WAIT_COMPLETION_PACKET_ALL_ACCESS, &wcp_oa);
  if (!NT_SUCCESS(st)) {
    g_lock.unlock();
    free_slot(idx);
    return INVALID_TOKEN;
  }

  // Per-slot MemoryReserveIoCompletion: pre-paid kernel completion packet
  // used to manually queue an IOCP entry on the AlreadySignaled path. Each
  // slot owns its own so concurrent rearm() calls across slots never
  // contend on a shared reserve, and the same slot is naturally serialized
  // by the dispatching CAS (one drain in flight per slot at a time).
  HANDLE reserve = nullptr;
  auto rsv_oa = windows::internal_oa();
  st = ::NtAllocateReserveObject(&reserve, &rsv_oa,
                                 MemoryReserveIoCompletion);
  if (!NT_SUCCESS(st)) {
    ::NtClose(wcp);
    g_lock.unlock();
    free_slot(idx);
    return INVALID_TOKEN;
  }

  // Publish all slot fields BEFORE NtAssociateWaitCompletionPacket so the
  // dispatch thread can never see a half-initialised slot if the kernel
  // queues a packet immediately on association.
  slot->callback = cb;
  slot->context = context;
  slot->target = waitable;
  slot->wcp = wcp;
  slot->reserve = reserve;
  slot->kind.store(SLOT_WCP, cpp::MemoryOrder::RELAXED);

  PVOID key = pack_key(idx, gen);
  BOOLEAN already_signaled = FALSE;
  st = ::NtAssociateWaitCompletionPacket(
      wcp, rr.iocp, waitable, key,
      nullptr, STATUS_SUCCESS, 0, &already_signaled);
  if (!NT_SUCCESS(st)) {
    slot->kind.store(SLOT_FREE, cpp::MemoryOrder::RELAXED);
    ::NtClose(reserve);
    ::NtClose(wcp);
    g_lock.unlock();
    free_slot(idx);
    return INVALID_TOKEN;
  }

  // AlreadySignaled=TRUE means the kernel sampled the wait object as
  // signaled but did NOT queue a packet via the WCP. Queue one ourselves
  // through the per-slot reserve so the wake is never lost. Done under
  // g_lock so a concurrent unwatch() cannot NtClose the reserve handle
  // out from under us.
  if (already_signaled) {
    ::NtSetIoCompletionEx(rr.iocp, reserve, key,
                          nullptr, STATUS_SUCCESS, 0);
  }

  g_lock.unlock();

  return ReactorToken{idx, gen};
}

ReactorToken watch_alpc(HANDLE alpc_port, ReactorCallback cb, void *context) {
  ReactorState &rr = g_pcb.reactor;
  if (!rr.iocp || !cb || !alpc_port)
    return INVALID_TOKEN;

  if (rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return INVALID_TOKEN;

  uint32_t idx = 0;
  ReactorSlot *slot = alloc_slot(&idx);
  if (!slot)
    return INVALID_TOKEN;

  uint32_t gen = rr.generation.fetch_add(1, cpp::MemoryOrder::RELAXED);
  init_slot_fields(slot, gen);

  g_lock.lock();

  ALPC_PORT_ASSOCIATE_COMPLETION_PORT assoc;
  assoc.CompletionKey = pack_key(idx, gen);
  assoc.CompletionPort = rr.iocp;

  NTSTATUS st = ::NtAlpcSetInformation(
      alpc_port, AlpcAssociateCompletionPortInformation,
      &assoc, sizeof(assoc));
  if (!NT_SUCCESS(st)) {
    g_lock.unlock();
    free_slot(idx);
    return INVALID_TOKEN;
  }

  slot->callback = cb;
  slot->context = context;
  slot->target = alpc_port;
  slot->wcp = nullptr;
  slot->kind.store(SLOT_ALPC, cpp::MemoryOrder::RELAXED);

  g_lock.unlock();

  return ReactorToken{idx, gen};
}

ReactorToken watch_job(HANDLE job, ReactorCallback cb, void *context) {
  ReactorState &rr = g_pcb.reactor;
  if (!rr.iocp || !cb || !job)
    return INVALID_TOKEN;

  if (rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return INVALID_TOKEN;

  uint32_t idx = 0;
  ReactorSlot *slot = alloc_slot(&idx);
  if (!slot)
    return INVALID_TOKEN;

  uint32_t gen = rr.generation.fetch_add(1, cpp::MemoryOrder::RELAXED);
  init_slot_fields(slot, gen);

  g_lock.lock();

  // Associate the job with the reactor's IOCP. The packed reactor key
  // becomes the CompletionKey for all job notifications.
  JOBOBJECT_ASSOCIATE_COMPLETION_PORT assoc;
  assoc.CompletionKey = pack_key(idx, gen);
  assoc.CompletionPort = rr.iocp;

  NTSTATUS st = ::NtSetInformationJobObject(
      job, JobObjectAssociateCompletionPortInformation,
      &assoc, sizeof(assoc));
  if (!NT_SUCCESS(st)) {
    g_lock.unlock();
    free_slot(idx);
    return INVALID_TOKEN;
  }

  slot->callback = cb;
  slot->context = context;
  slot->target = job;
  slot->wcp = nullptr;
  slot->kind.store(SLOT_JOB, cpp::MemoryOrder::RELAXED);

  g_lock.unlock();

  return ReactorToken{idx, gen};
}

void unwatch(ReactorToken token) {
  if (!token.valid())
    return;

  ReactorState &rr = g_pcb.reactor;

  // During shutdown, the drain thread owns all slot cleanup. Touching
  // the slot here would race with drain_cleanup_slots().
  if (rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return;

  ReactorSlot *slot = g_slot_pool.slot_for(token.idx);
  if (!slot)
    return;

  g_lock.lock();

  uint32_t k = slot->kind.load(cpp::MemoryOrder::RELAXED);
  if (slot->generation.load(cpp::MemoryOrder::RELAXED) != token.generation ||
      k == SLOT_FREE) {
    g_lock.unlock();
    return;
  }

  if (k == SLOT_WCP && slot->wcp) {
    ::NtCancelWaitCompletionPacket(slot->wcp, 1);
    ::NtClose(slot->wcp);
    slot->wcp = nullptr;
  }
  if (slot->reserve) {
    ::NtClose(slot->reserve);
    slot->reserve = nullptr;
  }

  // Do NOT set kind = SLOT_FREE yet. dispatch_reactor_completion Step 7
  // checks (kind == SLOT_FREE) after setting dispatching=false to handle
  // detach()'s deferred free. If we set SLOT_FREE here while a dispatch
  // is in-flight, both Step 7 and our free below would fire — double free.
  // Instead, mark the slot as dead (null callback + bumped generation) so
  // no new dispatch can match, then set SLOT_FREE after the spin-wait.
  slot->callback = nullptr;
  slot->context = nullptr;
  slot->target = nullptr;
  slot->generation.fetch_add(1, cpp::MemoryOrder::RELEASE);

  g_lock.unlock();

  // Spin-wait for any in-flight dispatch to complete before freeing.
  // `dispatch_inflight` covers the full window of slot access by a
  // dispatcher, including the Dekker-close load and retake CAS that
  // happen after `dispatching` is briefly released at Step 6. Spinning on
  // the narrower `dispatching` alone would admit a race where unwatch
  // frees the slot between Step 6 release and Step 6b's access.
  // Address-monitor park (UMWAIT C0.2 / MWAITX) on the dispatch_inflight
  // cache line — wakes on the dispatcher's fetch_sub without burning cycles.
  for (;;) {
    uint32_t inflight = slot->dispatch_inflight.load(cpp::MemoryOrder::ACQUIRE);
    if (inflight == 0)
      break;
    spin_wait::spin_on_raw(
        reinterpret_cast<uint32_t *>(&slot->dispatch_inflight.val),
        inflight, 4096);
  }

  // RELAXED: post-spin we know no dispatcher holds the slot, and
  // free_slot's bitmap.clear RELEASE fence publishes the final state
  // to any future allocator that acquires the idx.
  slot->kind.store(SLOT_FREE, cpp::MemoryOrder::RELAXED);
  free_slot(token.idx);
}

void detach(ReactorToken token) {
  if (!token.valid())
    return;

  ReactorState &rr = g_pcb.reactor;

  if (rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return;

  ReactorSlot *slot = g_slot_pool.slot_for(token.idx);
  if (!slot)
    return;

  g_lock.lock();

  uint32_t k = slot->kind.load(cpp::MemoryOrder::RELAXED);
  if (slot->generation.load(cpp::MemoryOrder::RELAXED) != token.generation ||
      k == SLOT_FREE) {
    g_lock.unlock();
    return;
  }

  if (k == SLOT_WCP && slot->wcp) {
    ::NtCancelWaitCompletionPacket(slot->wcp, 1);
    ::NtClose(slot->wcp);
    slot->wcp = nullptr;
  }
  if (slot->reserve) {
    ::NtClose(slot->reserve);
    slot->reserve = nullptr;
  }

  // Mark free but do NOT call free_slot(). The dispatch loop still holds
  // a reference to this slot (the caller is inside the callback).
  // dispatch_reactor_completion() checks kind == SLOT_FREE after the
  // callback returns and completes the deferred free.
  //
  // RELEASE: this store is observed lock-free by dispatch Step 7's
  // ACQUIRE load, so the SLOT_FREE transition needs an explicit fence
  // pair. RELAXED would leave the Step 7 reader with no formal
  // happens-before against our nulled handle fields above.
  slot->kind.store(SLOT_FREE, cpp::MemoryOrder::RELEASE);
  slot->callback = nullptr;
  slot->context = nullptr;
  slot->target = nullptr;
  slot->generation.fetch_add(1, cpp::MemoryOrder::RELEASE);

  g_lock.unlock();
  // No dispatching spin -- caller IS the dispatch context.
  // No free_slot -- deferred to dispatch_reactor_completion step 7.
}

int rearm(ReactorToken token) {
  if (!token.valid())
    return -1;

  ReactorState &rr = g_pcb.reactor;

  if (rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return -1;

  ReactorSlot *slot = g_slot_pool.slot_for(token.idx);
  if (!slot)
    return -1;

  g_lock.lock();

  if (slot->generation.load(cpp::MemoryOrder::RELAXED) != token.generation ||
      slot->kind.load(cpp::MemoryOrder::RELAXED) != SLOT_WCP || !slot->wcp) {
    g_lock.unlock();
    return -1;
  }

  PVOID key = pack_key(token.idx, token.generation);

  BOOLEAN already_signaled = FALSE;
  NTSTATUS st = ::NtAssociateWaitCompletionPacket(
      slot->wcp, rr.iocp, slot->target,
      key, nullptr, STATUS_SUCCESS, 0, &already_signaled);

  // Closes the AlreadySignaled wake-loss window: the kernel sampled the
  // wait object as signaled but does not auto-queue a packet via the WCP
  // in that case. Post one through the per-slot reserve so every kernel
  // signal is paired with exactly one IOCP entry. If this post arrives at
  // a drain thread while the same slot's callback is already running, the
  // Dekker handoff in dispatch_reactor_completion (pending_redispatch)
  // ensures the current winner re-runs the callback rather than dropping
  // the IOCP entry. Done under g_lock so a concurrent unwatch() cannot
  // NtClose the reserve handle out from under us.
  if (NT_SUCCESS(st) && already_signaled && slot->reserve) {
    ::NtSetIoCompletionEx(rr.iocp, slot->reserve, key,
                          nullptr, STATUS_SUCCESS, 0);
  }

  g_lock.unlock();

  return NT_SUCCESS(st) ? 0 : -1;
}

// =========================================================================
// Public API -- Completion Routing
// =========================================================================

void set_completion_router(CompletionRouter router) {
  g_pcb.reactor.router.store(reinterpret_cast<uintptr_t>(router),
                             cpp::MemoryOrder::RELEASE);
}

// =========================================================================
// Public API -- Inline Dispatch
// =========================================================================

void dispatch_inline(PVOID key, NTSTATUS status, ULONG_PTR information) {
  dispatch_reactor_completion(key, status, information);
}

// =========================================================================
// Public API -- IOCP Access
// =========================================================================

HANDLE iocp_handle() { return g_pcb.reactor.iocp; }
HANDLE reserve_handle() { return g_pcb.reactor.reserve; }

bool drain_thread_healthy() {
  // Load the current heartbeat and compare against the previous snapshot.
  // Uses a thread-local to store the last-seen value so each caller gets
  // independent health tracking.
  static thread_local uint64_t last_seen = 0;
  uint64_t current = g_pcb.reactor.heartbeat.load(cpp::MemoryOrder::RELAXED);
  bool advanced = (current != last_seen);
  last_seen = current;
  return advanced;
}

uint64_t current_heartbeat() {
  return g_pcb.reactor.heartbeat.load(cpp::MemoryOrder::ACQUIRE);
}

void fence_drain_cycle() {
  ReactorState &rr = g_pcb.reactor;
  if (!rr.iocp || rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return;

  uint32_t n = rr.drain_thread_count;
  if (n == 0)
    return;

  // RCU-style fence: snapshot each drain thread's per-thread epoch,
  // then wait for every thread to advance past its snapshot. This
  // guarantees that every drain thread has completed at least one full
  // batch since the fence point — no in-flight completion from before
  // the fence can still be executing.
  //
  // Scales from 1 to MAX_DRAIN_THREADS with no behavioral change.
  // Per-thread epochs are cache-line aligned, so there's zero
  // contention on the hot path (each thread writes its own line).

  // Phase 0: Snapshot all per-thread epochs.
  uint64_t snapshots[ReactorState::MAX_DRAIN_THREADS];
  for (uint32_t i = 0; i < n; ++i)
    snapshots[i] = rr.drain_epochs[i].value.load(cpp::MemoryOrder::ACQUIRE);

  // Wake all drain threads so they cycle even if idle (blocked on
  // IOCP with infinite timeout). One sentinel per thread.
  for (uint32_t i = 0; i < n; ++i)
    wake_drain_thread(rr);

  // Wait for each thread to advance past its snapshot. For each
  // unconverged thread: hardware spin (UMWAIT/MWAITX on the epoch
  // cache line), then futex_addr park with a per-thread timeout
  // slice. No yield loops, no busy polling.
  //
  // Total timeout budget: 10ms spread across N threads. Each thread
  // gets 10ms/N. If all threads are healthy, each converges in
  // microseconds (the hardware spin catches it). The futex park is
  // only reached if a thread is mid-callback.
  LARGE_INTEGER per_thread_timeout;
  per_thread_timeout.QuadPart =
      -100000LL / static_cast<long long>(n); // 10ms / N, relative.

  for (uint32_t i = 0; i < n; ++i) {
    // Fast check: already advanced?
    if (rr.drain_epochs[i].value.load(cpp::MemoryOrder::ACQUIRE) !=
        snapshots[i])
      continue;

    // Hardware spin on the low 32 bits of the epoch. UMWAIT/MWAITX
    // monitors the cache line — wakes on write with near-zero power.
    uint32_t snap_lo = static_cast<uint32_t>(snapshots[i]);
    if (spin_wait::spin_on_raw(
            reinterpret_cast<uint32_t *>(&rr.drain_epochs[i].value.val),
            snap_lo))
      continue;

    // Futex park: sleep until the drain thread bumps the epoch.
    // futex_addr::wait_nt checks *addr != expected on entry, does
    // its own pre-spin, then parks in the kernel if still equal.
    // The drain thread calls futex_addr::wake after each epoch
    // bump, unparking us immediately. Timeout is a safety net.
    futex_addr::wait_nt(
        reinterpret_cast<volatile uint32_t *>(
            &rr.drain_epochs[i].value.val),
        snap_lo, &per_thread_timeout);
  }
}

// =========================================================================
// Subsystem entries
// =========================================================================
//
// Init:  reactor_startup_init() — Phase 7, after fd_table, before signals.
// Fini:  registered into `.libcfin$P7` (after signal/alpc_bus finis whose
//        ALPC handler dispatch the reactor drives).
// Fork:  reactor_fork_reinit() — after fd_table, before signal_fork_reinit().

} // namespace reactor
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

int LIBC_NAMESPACE::internal::reactor_startup_init() {
  LIBC_NAMESPACE::internal::reactor::init();
  return 0;
}

void LIBC_NAMESPACE::internal::reactor_fork_reinit() {
  LIBC_NAMESPACE::internal::reactor::fork_reinit();
}

LIBC_REGISTER_FINI(7, reactor, &::LIBC_NAMESPACE::internal::reactor::fini)

LIBC_REGISTER_FORK_REINIT(reactor,
                          ::LIBC_NAMESPACE::internal::kForkPrioReactor,
                          &::LIBC_NAMESPACE::internal::reactor_fork_reinit)
