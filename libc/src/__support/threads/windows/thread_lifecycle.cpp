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
//   1. Robust mutex list — mark held mutexes OWNER_DIED (must run before
//      signal deregistration so waiters can probe the handle for liveness).
//   2. Signal state — registry_remove hides the node and zeros owner_tid.
//   3. Thread ring — close IoRing, NtClose event, free allocation.
//   4. Lifecycle free — for foreign threads (pool_allocated).
//
// Init: lifecycle_startup_init() — Phase 4, after pcb_startup_init(), before signal.
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/OSUtil/windows/io.h"
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/threads/windows/robust_list_cleanup.h"
#include "src/__support/threads/windows/thread_registry.h"

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
} // namespace signal_state

// ---------------------------------------------------------------------------
// Process-wide lifecycle pool
// ---------------------------------------------------------------------------

static internal::SlabPool lifecycle_pool;
static thread_local internal::SlabPool::ThreadSlab lifecycle_current_slab{nullptr};

ThreadLifecycle *alloc_lifecycle() {
  void *slot = internal::SlabPool::alloc(lifecycle_current_slab);
  if (!slot)
    slot = lifecycle_pool.alloc_slow(&lifecycle_current_slab);
  if (!slot)
    return nullptr;
  auto *lc = static_cast<ThreadLifecycle *>(slot);
  zero_lifecycle(lc);
  lc->dynamically_allocated = true;
  return lc;
}

void free_lifecycle(ThreadLifecycle *lc) {
  if (lc)
    internal::SlabPool::free(lc);
}

// The single TEB TLS slot index for the lifecycle root.
static DWORD lifecycle_tls_index = internal::TLS_OUT_OF_INDEXES;

// Thread-local cache — eliminates the TEB read on hot paths.
static thread_local ThreadLifecycle *lifecycle_cache;

// TLS cleanup callback — the single cleanup entry point for all per-thread
// state. Runs on thread exit via .CRT$XLC. Ordering is explicit and
// deterministic, not dependent on registration order.
static void NTAPI lifecycle_cleanup(void *data) {
  lifecycle_cache = nullptr;
  if (!data)
    return;
  auto *lc = static_cast<ThreadLifecycle *>(data);

  // 0. __cxa_thread_finalize — run thread_local destructors and POSIX TSS
  //    destructors. Must run first: C++ destructors may call cancellation
  //    points, close fds, etc. Called unconditionally — it's a no-op if
  //    nothing was registered, and this avoids cross-layer registration hooks.
  __cxa_thread_finalize(nullptr);

  // 1. Robust mutex cleanup — mark held mutexes OWNER_DIED.
  //    Must run before signal deregistration so waiters can still probe
  //    our thread_handle via the registry to confirm death.
  robust_mutex::robust_list_cleanup(lc);

  // 2. Signal state — zero owner_tid and leave reclamation to the registry.
  if (lc->signal) {
    signal_state::deregister_thread_state(lc->signal);
  } else {
    registry_deregister(lc);
  }

  // 3. Thread ring — close IoRing handle + event, free allocation.
  if (lc->thread_ring)
    ioring::thread_ring_destroy(lc->thread_ring);

  // 4. Slab-allocated lifecycles: for DETACHED threads, this is the only
  //    cleanup agent — deregister and retire for deferred free. For JOINABLE
  //    threads (detach_state == EXITING), the joiner owns retirement via
  //    free_thread_lifecycle(). Calling registry_deregister_and_free from
  //    BOTH the exiting thread and the joiner would push the lifecycle onto
  //    the retired list twice, creating a cycle that hangs the reclaimer.
  if (lc->dynamically_allocated) {
    uint32_t ds = lc->detach_state.load(cpp::MemoryOrder::ACQUIRE);
    if (ds != 0x22 /* DetachState::EXITING */)
      registry_deregister_and_free(lc);
  }
}

void lifecycle_init() {
  lifecycle_pool.init(sizeof(ThreadLifecycle), alignof(ThreadLifecycle));
  lifecycle_tls_index = internal::tls_alloc();
  if (lifecycle_tls_index == internal::TLS_OUT_OF_INDEXES) {
    write_to_stderr("libc fatal: tls_alloc failed for lifecycle root\n");
    __builtin_trap();
  }
  internal::tls_cleanup_register(lifecycle_tls_index, lifecycle_cleanup);
}

DWORD get_lifecycle_tls_index() { return lifecycle_tls_index; }

// Check if the calling thread has ThreadLocalStoragePointer initialized.
// Threads created with THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH (e.g., the
// reactor drain thread) skip LdrpAllocateTls, leaving gs:0x58 NULL. Any
// access to a thread_local variable on such a thread would dereference NULL.
LIBC_INLINE bool has_tls_array() {
  void *tls_array;
#if defined(__x86_64__)
  __asm__ volatile("movq %%gs:0x58, %0" : "=r"(tls_array));
#elif defined(__aarch64__)
  __asm__ volatile("ldr %0, [x18, #0x58]" : "=r"(tls_array));
#else
  tls_array = nullptr;
#endif
  return tls_array != nullptr;
}

ThreadLifecycle *get_current_lifecycle() {
  // Guard against threads without TLS (SKIP_THREAD_ATTACH). Accessing
  // thread_local lifecycle_cache would crash if ThreadLocalStoragePointer
  // is NULL.
  if (!has_tls_array())
    return nullptr;
  auto *cached = lifecycle_cache;
  if (cached)
    return cached;
  if (lifecycle_tls_index == internal::TLS_OUT_OF_INDEXES)
    return nullptr;
  cached = static_cast<ThreadLifecycle *>(
      internal::teb_tls_get(lifecycle_tls_index));
  lifecycle_cache = cached;
  return cached;
}

void set_current_lifecycle(ThreadLifecycle *lc) {
  if (lifecycle_tls_index != internal::TLS_OUT_OF_INDEXES)
    internal::teb_tls_set(lifecycle_tls_index, lc);
  if (has_tls_array())
    lifecycle_cache = lc;
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
//   - The forking thread's own lifecycle (via lifecycle_cache / TEB TLS)
//     remains valid — its slab is preserved by the TID check in
//     SlabPool::fork_reinit().
//
// What we do here:
//   1. Clear the thread_local slab allocator cache (forces slow-path on
//      next allocation, which will find or adopt a slab).
//   2. Verify the forking thread's lifecycle is still reachable via TLS
//      and clear stale per-thread state (active_syscall, pending cancel).
//   3. Reset the pool's internal locks and reclaim dead-thread slabs.
void LIBC_NAMESPACE::internal::lifecycle_fork_reinit() {
  // Drop the TLS slab cache.  After fork, the cached slab pointer is
  // inherited from the parent but may reference a slab whose owner TID
  // no longer matches (the child gets a new TID).  Clearing forces the
  // slow path, which will re-establish a valid slab for the child thread.
  LIBC_NAMESPACE::lifecycle_current_slab = nullptr;

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
    // Only clear the pending-cancellation bit: a cancellation requested
    // against the parent thread should not carry into the child.
    uint8_t cw = self->cancel_word.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);
    cw &= ~uint8_t(0x04); // clear bit 2 (pending)
    self->cancel_word.store(cw, LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);
  }

  // Reset the pool: release dead-thread slabs, reset internal spinlocks.
  LIBC_NAMESPACE::lifecycle_pool.fork_reinit();
}
