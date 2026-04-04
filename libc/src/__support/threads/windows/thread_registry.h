//===-- Crystalline-W thread registry for Windows ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lock-free, wait-free-safe-reclaimed thread registry built atop
// Crystalline-W SMR (Nikolaev & Ravindran, PPoPP '24).
//
// Storage layout
// --------------
//   * Lifecycles are slab-allocated; their address is the canonical
//     pthread_t target via `&lc->attrib`.
//   * The global iter list is a single Harris linked list rooted at
//     `g_pcb.thread_registry.iter_head`.
//   * The task_id index is a linear-hash table whose buckets are
//     Harris chains of `BucketEntry` nodes (see `lockfree_hash.h`).
//
// Cross-thread reference discipline
// ---------------------------------
//   `ThreadHandle = {tid, task_id}` is the canonical identifier any
//   site captures when it needs to refer to another thread later.
//   Resolution goes through `task_id` (monotonic, never recycled);
//   `tid` is along for the ride and read off the freshly-resolved
//   lifecycle when a syscall needs it.
//
//   Sites with `pthread_t pt` in scope use the bypass:
//       ThreadLifecycle *lc = pt->platform_data;
//   This skips the registry entirely. POSIX-trust applies — a stale
//   `pt` after `pthread_join` / `pthread_detach` is undefined per
//   POSIX, same as glibc/musl.
//
// Reclamation
// -----------
//   `registry_deregister_and_retire(lc)` sequences:
//     1. Mark + unlink the BucketEntry referencing `lc`. Retire entry
//        via Crystalline.
//     2. Mark + unlink `lc` from the iter list. Retire `lc` via
//        Crystalline.
//   Crystalline frees both once no thread holds a reservation
//   pinning them. The lifecycle's slab slot returns to the pool;
//   the entry's page returns via `page_free`.
//
//   `registry_thread_quiesce()` calls Crystalline `clear_all()` on
//   the calling thread's reservations — used at API boundaries
//   where the caller has finished all dereferences.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/type_traits/remove_reference.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry_state.h"

namespace LIBC_NAMESPACE_DECL {

// =========================================================================
// Registration
// =========================================================================

// Register `lc` with the given thread handle and TID. If
// `duplicate_handle` is true, `thread_handle` is duplicated;
// otherwise it's adopted. Idempotent for the (lc, tid) pair —
// re-registering with the same identity returns the existing
// registration. Allocates a `task_id` if `lc->task_id == 0`.
//
// Returns true on success. Failure modes: OOM (no task_id slot, no
// bucket entry alloc, etc.) — failure propagates a false return; the
// lifecycle is left unregistered.
bool registry_register(ThreadLifecycle *lc, HANDLE thread_handle,
                       uint32_t tid, bool duplicate_handle);

// Self-register: `NtCurrentThread()` (duplicated) + `NtCurrentThreadId()`.
bool registry_register_self(ThreadLifecycle *lc);

// Register `lc` consuming a CALLER-PRE-ALLOCATED BucketEntry. The entry
// must have been produced by `registry_alloc_bucket_entry(lc->task_id,
// lc)`; on success the registry takes ownership and the caller MUST
// NOT touch it. On failure the registry returns it to the caller for
// release via `registry_release_unused_entry()` so a recoverable failure
// path (NtCreateThreadEx fail) does not leak the slab slot.
//
// The pre-allocation contract (H2) lets Thread::run reserve the entry
// BEFORE NtCreateThreadEx so the post-create registry_register cannot
// silently fail on slab exhaustion. If pre-alloc fails, pthread_create
// returns ENOMEM cleanly without ever launching the thread.
struct BucketEntry; // Defined in lockfree_hash.h.
bool registry_register_with_entry(ThreadLifecycle *lc, BucketEntry *entry,
                                   HANDLE thread_handle, uint32_t tid,
                                   bool duplicate_handle);

// Pre-allocate a BucketEntry for use with registry_register_with_entry.
// Allocates `lc->task_id` if not already set. Returns nullptr on OOM.
BucketEntry *registry_alloc_bucket_entry_for(ThreadLifecycle *lc);

// Release a pre-allocated entry that was never published via
// registry_register_with_entry (e.g., NtCreateThreadEx failed in
// Thread::run after pre-alloc). DOES NOT go through Crystalline retire
// because the entry was never reachable by any reader.
void registry_release_unused_entry(BucketEntry *entry);

// Remove `lc` from the registry. The lifecycle memory is NOT freed
// — `lc` stays valid for the caller. Use this when the caller still
// holds references; for the final exit path use
// `registry_deregister_and_retire` which hands `lc` to Crystalline.
void registry_deregister(ThreadLifecycle *lc);

// Remove `lc` and submit it to Crystalline for deferred reclaim.
// Returns immediately; Crystalline runs the FreeFn once no
// reservation pins `lc`. Caller MUST NOT touch `lc` after this.
void registry_deregister_and_retire(ThreadLifecycle *lc);

// =========================================================================
// Lookups (Crystalline-protected)
// =========================================================================
//
// All lookups consume one Crystalline reservation index for the
// duration of the call. The returned pointer is valid until the
// caller's next registry call at the same reservation index, OR
// until `registry_thread_quiesce()`. DO NOT cache across these
// boundaries.
//
// Crystalline transitively pins the chain of objects observed under
// one reservation: bucket head page → bucket entry → lifecycle. So a
// lookup that returns `lc` keeps `lc` alive for the duration of the
// reservation hold.

// O(1) average — task_id hash. Returns nullptr if no live thread has
// the given task_id.
ThreadLifecycle *registry_find_by_task_id(uint32_t task_id);

// Resolve a captured `ThreadHandle`. Returns nullptr if the task_id
// is no longer registered. The `tid` field is sanity-checked against
// the resolved lifecycle (defense in depth — task_id alone is the
// authoritative identity, since it never recycles).
ThreadLifecycle *registry_resolve(ThreadHandle h);

// Build a `ThreadHandle` for the calling thread. Returns
// `ThreadHandle::invalid()` if the current thread has no registered
// lifecycle (bootstrap, foreign thread).
ThreadHandle current_thread_handle();

// =========================================================================
// Cross-thread operations (caller has lifecycle pointer)
// =========================================================================
//
// These take `lc` directly — caller is responsible for ensuring `lc`
// is valid (typically by holding a Crystalline reservation, or by
// being the lifecycle's owning thread).

// Borrow the thread handle. Returns nullptr if the lifecycle has no
// handle.
LIBC_INLINE HANDLE registry_borrow_handle(ThreadLifecycle *lc) {
  if (!lc)
    return nullptr;
  return lc->thread_handle.load(cpp::MemoryOrder::ACQUIRE);
}

// NtCreateThreadStateChange + NtChangeThreadState. Returns true on
// success.
bool registry_suspend(ThreadLifecycle *lc);
bool registry_resume(ThreadLifecycle *lc);

// =========================================================================
// Iteration
// =========================================================================
//
// `registry_for_each` walks the global iter list under a Crystalline
// reservation, invoking `visitor(lc)` for every live (unmarked)
// lifecycle. Visitor returns true to terminate early. `skip_tid`
// (NT TID) skips a specific thread (typically the caller).
//
// The visitor MUST NOT mutate the registry (no register/deregister
// from inside) — it would invalidate the walk's chain pointers.

using RegistryVisitorFn = bool (*)(void *ctx, ThreadLifecycle *lc);

// Non-template walker. Single copy per binary.
bool registry_for_each_impl(RegistryVisitorFn visitor, void *ctx,
                            uint32_t skip_tid);

template <typename Fn>
LIBC_INLINE bool registry_for_each(Fn &&visitor, uint32_t skip_tid = 0) {
  using FnVal = cpp::remove_reference_t<Fn>;
  auto trampoline = [](void *ctx, ThreadLifecycle *lc) -> bool {
    return (*static_cast<FnVal *>(ctx))(lc);
  };
  return registry_for_each_impl(trampoline, &visitor, skip_tid);
}

// Find the first lifecycle matching a predicate. Returns nullptr if
// none. Walks the iter list once, terminating on first match.
template <typename Pred>
LIBC_INLINE ThreadLifecycle *registry_find_if(Pred &&predicate) {
  ThreadLifecycle *result = nullptr;
  auto wrapper = [&predicate, &result](ThreadLifecycle *lc) -> bool {
    if (predicate(lc)) {
      result = lc;
      return true;
    }
    return false;
  };
  registry_for_each(wrapper, 0);
  return result;
}

// =========================================================================
// Batch TID broadcast (NtAlertMultipleThreadByThreadId target)
// =========================================================================

// Alert every live thread (NtAlertMultipleThreadByThreadId, with
// per-thread fallback on syscall failure). Skips `skip_tid`. The
// implementation walks the iter list with a fixed-size stack buffer
// and flushes mid-walk when the buffer fills — no heap allocation, no
// pagination state, no live_count snapshot. Mirrors the futex_utils
// alert-batching pattern.
//
// Callers that want a TID list (tests, diagnostics, cross-thread
// synchronisation primitives) compose `registry_for_each` with a
// visitor that pushes onto a caller-owned container — there is no
// dedicated "collect_tids" API because every variant has fragile
// truncation semantics relative to caller-supplied capacity.
void registry_alert_all(uint32_t skip_tid = 0);

// =========================================================================
// Lifecycle / fork / TLS
// =========================================================================

// Process-wide live lifecycle count.
LIBC_INLINE uint32_t registry_live_count();

// Lazy-init the registry. Idempotent. Called by the first
// `registry_register*` (or any other API entry that needs the
// indexed views).
bool registry_ensure_initialized();

// Fork-child reinitialization. Single-threaded post-fork. Cleans
// stale lifecycles, frees their backing slabs, and rebuilds the
// registry with `self` as the sole live lifecycle. Crystalline's
// `fork_reinit_trampoline` runs separately (chained through the
// global Crystalline domain registry).
void registry_fork_reinit(ThreadLifecycle *self);

// Drop every Crystalline reservation the calling thread holds for
// the registry's domain. Wired into the TLS cleanup chain. After
// this returns, any lifecycle pointer the caller previously obtained
// from a registry lookup MUST be re-resolved before use.
void registry_thread_quiesce();

// =========================================================================
// Stats
// =========================================================================

LIBC_INLINE uint32_t registry_live_count() {
  extern uint32_t registry_live_count_impl();
  return registry_live_count_impl();
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_H
