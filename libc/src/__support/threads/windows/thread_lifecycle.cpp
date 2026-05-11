//===-- ThreadLifecycle TLS root management -----------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Owns the single TEB TLS slot that roots all per-thread state. Every
// subsystem (signals, thread ring, atexit, cancellation, robust mutexes)
// hangs off the ThreadLifecycle pointer stored in this slot.
//
// Cleanup ordering on thread exit:
//   1. Robust mutex list — mark held mutexes OWNER_DIED (must run
//      before signal deregistration so waiters can probe the
//      registered thread handle for liveness).
//   2. Coordinator death — if we're the stop coordinator, broadcast
//      COORD_DEAD to parked threads (must run before deregistration).
//   3. Signal state — registry_deregister hides the node from
//      cross-thread lookups; the lifecycle remains alive.
//   4. Thread ring — close IoRing, NtClose event, free allocation.
//   5. Quiesce + retire — drop the calling thread's Crystalline
//      reservations and submit the lifecycle for deferred reclaim
//      (detached threads only; joinable threads transition to
//      EXITING and the joiner runs the retire).
//
// Init: lifecycle_startup_init() — Phase 4, after pcb_startup_init(),
// before signal.
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/OSUtil/windows/io.h"
#include "src/__support/OSUtil/windows/alloc/legacy/slab_pool.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/threads/windows/robust_list_cleanup.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

// Thread ring cleanup — defined in thread_ring.cpp.
namespace LIBC_NAMESPACE_DECL {
namespace ioring {
void thread_ring_destroy(void *ptr);
} // namespace ioring
} // namespace LIBC_NAMESPACE_DECL

// __cxa_thread_finalize — runs thread_local destructors for this thread.
// Pass nullptr for full cleanup (thread exit); pass a DSO handle for
// selective per-DSO cleanup (dlclose).
extern "C" void __cxa_thread_finalize(void *dso);

namespace LIBC_NAMESPACE_DECL {

// Forward declarations — signal subsystem cleanup called from here.
namespace signal_state {
void deregister_thread_state(struct ThreadSignalState *state);
namespace process_control {
void on_coordinator_exit(uint32_t tid);
} // namespace process_control
} // namespace signal_state

// ---------------------------------------------------------------------------
// Process-wide lifecycle pool
// ---------------------------------------------------------------------------

static internal::SlabPool lifecycle_pool;

ThreadLifecycle *alloc_lifecycle() {
  // Pool-managed TLS routes the slab through the FLS abandon callback at
  // thread exit, so abandoned slabs reach the Crystalline retire path
  // (sealed-with-live-slots → last-freer self-retire). Previously this
  // pool used a raw `thread_local` slab pointer with no exit hook —
  // dead-thread slabs sat forever owned by the dead TID and never
  // reclaimed.
  void *slot = lifecycle_pool.tls_alloc();
  if (!slot)
    return nullptr;
  auto *lc = static_cast<ThreadLifecycle *>(slot);
  // zero_lifecycle stamps the ThreadRegistryNode header with kind =
  // Lifecycle so a never-registered (and therefore never-Crystalline-
  // init_node'd) lifecycle still routes correctly through
  // free_thread_registry_node if anything ever sends it down that
  // path. Direct slab return via free_lifecycle bypasses the
  // FreeFn dispatcher entirely.
  zero_lifecycle(lc);
  // signal points at the inline sig_state by default. Foreign-thread
  // paths overwrite this with a separately-allocated state. RELAXED
  // is sufficient: alloc_lifecycle's caller is the owning thread and
  // observes the store on the same thread; cross-thread observers
  // see it through registry publication's release fence.
  lc->signal.store(&lc->sig_state, cpp::MemoryOrder::RELAXED);
  return lc;
}

void free_lifecycle(ThreadLifecycle *lc) {
  if (lc)
    internal::SlabPool::free(lc);
}

// The single TEB TLS slot index for the lifecycle root.
static DWORD lifecycle_tls_index = internal::TLS_OUT_OF_INDEXES;

// TLS cleanup callback — the single cleanup entry point for all per-thread
// state. Runs on thread exit via .CRT$XLC. Ordering is explicit and
// deterministic, not dependent on registration order.
static void lifecycle_cleanup(void *data) {
  if (!data)
    return;
  auto *lc = static_cast<ThreadLifecycle *>(data);

  // 0a. Drain ALL queued user APCs before we start tearing down thread
  //     state. A concurrent pthread_kill() / cross-thread signal send /
  //     pthread_cancel may have queued a signal APC just before we
  //     entered cleanup; the kernel silently drops queued APCs on
  //     thread termination. NtTestAlert forces the kernel-to-user APC
  //     dispatch synchronously and dispatches AT MOST ONE APC per call
  //     (that's the documented behavior — the kernel's APC frame in
  //     user mode runs one entry then returns). An APC handler may
  //     itself queue another APC (e.g., a signal handler invoking
  //     pthread_kill on its own thread), so we loop until a call to
  //     NtTestAlert observes no further APC dispatch.
  //
  //     Bounded by user-code behavior: a handler that perpetually
  //     re-queues an APC is buggy; we cap at 64 iterations as a
  //     pragmatic ceiling. After the cap we log-and-continue so the
  //     remaining cleanup still runs (signals already pended via
  //     transfer_all on the next phase).
  for (uint32_t i = 0; i < 64; ++i)
    ::NtTestAlert();

  // 0b. __cxa_thread_finalize — run thread_local destructors and POSIX TSS
  //     destructors. Must run after the APC drain: any destructors that
  //     rely on signal handlers not firing mid-tear-down are safe because
  //     step 0a already delivered all pending signals.
  __cxa_thread_finalize(nullptr);

  // 1. Robust mutex cleanup — mark held mutexes OWNER_DIED.
  //    Must run before signal deregistration so waiters can still probe
  //    our thread_handle via the registry to confirm death.
  robust_mutex::robust_list_cleanup(lc);

  // 2. Coordinator death broadcast — if this thread is the stop coordinator,
  //    wake all parked threads so they can elect a replacement. Must run
  //    before deregistration: parked threads need us visible in the registry
  //    to resolve the CAS-steal. This is the primary death-detection path;
  //    the reactor WCP watch is backup for external NtTerminateThread.
  signal_state::process_control::on_coordinator_exit(lc->tid);

  // 3. Signal state. The signal subsystem may have its own dereg path
  //    that calls registry_deregister; otherwise we deregister here.
  //    Either way, the lifecycle remains alive (no Crystalline retire)
  //    until step 5 decides whether THIS thread or the joiner owns it.
  signal_state::ThreadSignalState *sig =
      lc->signal.load(cpp::MemoryOrder::ACQUIRE);
  if (sig) {
    signal_state::deregister_thread_state(sig);
  } else {
    registry_deregister(lc);
  }

  // 4. Thread ring — close IoRing handle + event, free allocation.
  if (lc->thread_ring)
    ioring::thread_ring_destroy(lc->thread_ring);

  // 5. Drop any Crystalline reservations this thread holds for the
  //    registry's domain BEFORE we hand the lifecycle to Crystalline
  //    — otherwise we'd briefly pin a chain that includes our own
  //    retire-target, which is benign but wastes one batch's worth
  //    of pinning. Per-thread retire batches are drained separately
  //    by Crystalline's scratch teardown chained off this same TLS
  //    cleanup phase.
  registry_thread_quiesce();

  // 6. Hand the lifecycle to Crystalline for deferred reclaim — but
  //    only when THIS thread owns the retire. The retire-ownership
  //    state machine on detach_state arbitrates among:
  //      - DETACHED: nobody can join; this thread runs retire.
  //      - EXITING:  thread parked for joiner; joiner CAS EXITING →
  //                  JOINING and runs retire. Don't retire here.
  //      - JOINING:  joiner / late-detacher already claimed retire.
  //                  Don't retire here.
  //      - JOINABLE: should never occur at this point — thread_entry_
  //                  impl / thread_exit always CAS to EXITING / fails.
  //                  Trap on this corruption.
  // C2 fix: structurally prevents double-retire by making retire
  // ownership a single-CAS-claim atomic.
  // Switch on the raw uint32_t (not the enum) so the `default:` is a
  // genuine runtime guard against a corrupted load — the four enum
  // values cover the legal domain, but `detach_state` is uint32_t-wide,
  // so any other bit pattern means memory corruption. (Switching on
  // DetachState would trip -Wcovered-switch-default.)
  uint32_t ds = lc->detach_state.load(cpp::MemoryOrder::ACQUIRE);
  switch (ds) {
  case static_cast<uint32_t>(DetachState::DETACHED):
    registry_deregister_and_retire(lc);
    break;
  case static_cast<uint32_t>(DetachState::EXITING):
  case static_cast<uint32_t>(DetachState::JOINING):
    // Joiner / late-detacher owns retire; do nothing.
    break;
  case static_cast<uint32_t>(DetachState::JOINABLE):
    // Should be unreachable — thread_entry_impl and thread_exit both
    // CAS to EXITING before NtTerminateThread, and the CAS only fails
    // when the state is already DETACHED or JOINING. If we observe
    // JOINABLE here, the exit path was bypassed (e.g., external
    // NtTerminateThread on a joinable thread). Retire ourselves —
    // there is no joiner CAS coming. This is the safe fallback rather
    // than a leak.
    registry_deregister_and_retire(lc);
    break;
  default:
    // Corrupted detach_state — trap rather than silently leak or
    // double-retire.
    __builtin_trap();
  }
}

void lifecycle_init() {
  lifecycle_pool.init(sizeof(ThreadLifecycle), alignof(ThreadLifecycle));
  // kTlsCleanupPhaseAllocator is the documented choice for pools backing
  // Phase-4 lifecycle objects (slab_pool.h:init_tls contract). Slab-abandon
  // must run AFTER lifecycle_cleanup has retired any node living inside
  // this slab.
  lifecycle_pool.init_tls(internal::kTlsCleanupPhaseAllocator);
  lifecycle_tls_index = internal::tls_alloc();
  if (lifecycle_tls_index == internal::TLS_OUT_OF_INDEXES) {
    write_to_stderr("libc fatal: tls_alloc failed for lifecycle root\n");
    __builtin_trap();
  }
  internal::tls_cleanup_register(lifecycle_tls_index, lifecycle_cleanup,
                                 internal::kTlsCleanupPhaseLifecycle);
}

// Every other subsystem's fini reads get_current_lifecycle(); this runs after
// all of them so the TLS slot stays live for their whole teardown.
namespace internal {
static void lifecycle_fini() {
  if (lifecycle_tls_index == TLS_OUT_OF_INDEXES)
    return;
  tls_cleanup_unregister(lifecycle_tls_index);
  tls_free(lifecycle_tls_index);
  lifecycle_tls_index = TLS_OUT_OF_INDEXES;
}
} // namespace internal

DWORD get_lifecycle_tls_index() { return lifecycle_tls_index; }

// Direct TEB inline slot access — single instruction, always valid.
//
// TEB inline slots (gs:0xE10 on x64, x18+0x1480 on AArch64) are part of the
// TEB structure itself, allocated by the kernel for every thread. They exist
// unconditionally — even threads created with THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH
// (which skip LdrpAllocateTls and leave gs:0x58 / ThreadLocalStoragePointer NULL)
// still have valid inline TLS slots. The slot is zero-initialized by the kernel,
// so threads that never call set_current_lifecycle() correctly return nullptr.
//
// This replaces the previous three-layer design:
//   Old: has_tls_array() guard → thread_local lifecycle_cache → teb_tls_get fallback
//   New: teb_tls_get (one MOV instruction, ~1 cycle on L1 hit)
//
// The old lifecycle_cache was a thread_local variable that required
// ThreadLocalStoragePointer (gs:0x58 → TLS array → module block → value),
// which is three pointer chases and crashes on SKIP_THREAD_ATTACH threads.
// The inline TEB slot is a single indexed load from a fixed segment register
// offset — no indirection, no null-pointer risk, no guard branch.

ThreadLifecycle *get_current_lifecycle() {
  if (lifecycle_tls_index == internal::TLS_OUT_OF_INDEXES)
    return nullptr;
  return static_cast<ThreadLifecycle *>(
      internal::teb_tls_get(lifecycle_tls_index));
}

void set_current_lifecycle(ThreadLifecycle *lc) {
  if (lifecycle_tls_index != internal::TLS_OUT_OF_INDEXES)
    internal::teb_tls_set(lifecycle_tls_index, lc);
}

} // namespace LIBC_NAMESPACE_DECL

// Phase 4: Thread lifecycle TLS must exist before signal_state hangs
// per-thread signal state off it.
int LIBC_NAMESPACE::internal::lifecycle_startup_init() {
  LIBC_NAMESPACE::lifecycle_init();
  return 0;
}


// Fork child reinit for the lifecycle pool.
//
// Thread trimming: the lifecycle pool is a SlabPool that holds per-thread
// ThreadLifecycle objects.  After fork, only the forking thread survives.
// The actual cleanup of stale (non-forking) ThreadLifecycle objects is
// performed by registry_fork_reinit() (called from signal_fork_reinit()),
// which walks all registry slots, closes inherited thread handles, marks
// robust mutexes OWNER_DIED, and frees dynamically-allocated lifecycles.
//
// This function runs BEFORE registry_fork_reinit in the reinit sequence.
// It is safe because:
//   - SlabPool::fork_reinit() only releases slabs where ALL slots have been
//     returned.  Since stale lifecycles are still "allocated" (not freed),
//     their slabs remain committed and accessible.
//   - After registry_fork_reinit calls free_lifecycle() on stale entries,
//     those slots are returned to the slab's cross-thread queue.  The next
//     alloc_slow() will adopt abandoned slabs and reclaim them.
//   - The forking thread's own lifecycle (via TEB TLS inline slot)
//     remains valid — its slab is preserved by the TID check in
//     SlabPool::fork_reinit().
//
// What we do here:
//   1. Verify the forking thread's lifecycle is still reachable via TLS
//      and clear stale per-thread state (active_syscall, pending cancel).
//   2. Reset the pool's internal locks and reclaim dead-thread slabs.
//
// Pool-managed TLS handles the slab cache: SlabPool::fork_reinit walks
// all_slabs_head_, re-claims the surviving thread's slab under the new
// TID, and demotes parent-only slabs into the abandoned chain (or
// retires them via Crystalline if empty). The TEB TLS slot is preserved
// across fork (CoW), and the child's first alloc on this pool routes
// through the existing slab — no explicit TLS clear needed here.
void LIBC_NAMESPACE::internal::lifecycle_fork_reinit() {

  // Verify the forking thread's lifecycle root is still accessible.
  // If TLS was corrupted during fork, catch it early rather than letting
  // signal_fork_reinit() dereference a bad pointer.
  auto *self = LIBC_NAMESPACE::get_current_lifecycle();
  if (self) {
    // Do NOT null thread_ring here — thread_ring_fork_reinit() runs AFTER
    // lifecycle_fork_reinit and needs lc->thread_ring to find and close
    // the stale IoRing handles + free the registered buffer VA.  It nulls
    // lc->thread_ring after cleanup.  No code path between these two
    // reinit functions dereferences the ring for I/O (the child is
    // single-threaded and not doing I/O during reinit).

    // Clear the active_syscall frame — no syscall is in flight in the child
    // (the child begins execution after fork, not mid-syscall).
    self->active_syscall.store(nullptr, LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);

    // Cancelation state and cleanup stack are NOT reset.  POSIX specifies
    // the child is a "replica of the calling thread" — it inherits the
    // forking thread's cancelability state/type and cleanup handler chain.
    // The cleanup handlers are stack-allocated __pthread_cleanup_t structs
    // whose pointers remain valid (the child inherits the stack contents).
    //
    // cancel_state (state/type bits) is preserved — POSIX "replica".
    // notify_word is cleared: stale parent notifications (pending cancel,
    // pending signals) must not carry into the child. fork_reinit()
    // updates owner_tid_ (child gets a new TID) and clears park_state_.
    self->notify_word.fork_reinit();
    self->notify_word.write(0);

    // The late-alert expectation is owner-local TLS state — any
    // parent-side unconsumed expectation is meaningless in the child,
    // whose own wait slots will be reset by wait_slot::fork_reinit.
    // Clearing here prevents the child's first Phase 4 wait from
    // misclassifying a real STATUS_ALERTED as stale.
    self->expect_late_alert.store(0, cpp::MemoryOrder::RELAXED);

    // The cached preferred NUMA node belongs to the parent's CPU at
    // the time of `pthread_create`; the child resumes on whatever CPU
    // NT picks for the post-fork process and the parent's cached
    // value is stale by definition. Reset to the unresolved sentinel
    // so the next user-facing allocation re-probes via
    // `NtGetCurrentProcessorNumberEx` against the child's actual CPU.
    self->preferred_node.store(LIBC_NAMESPACE::kPreferredNodeUnresolved,
                                cpp::MemoryOrder::RELAXED);
  }

  // Reset the pool: release dead-thread slabs, reset internal spinlocks.
  LIBC_NAMESPACE::lifecycle_pool.fork_reinit();
}

LIBC_REGISTER_FINI(3, lifecycle, &::LIBC_NAMESPACE::internal::lifecycle_fini)

LIBC_REGISTER_FORK_REINIT(lifecycle,
                          ::LIBC_NAMESPACE::internal::kForkPrioLifecycle,
                          &::LIBC_NAMESPACE::internal::lifecycle_fork_reinit)
