//===-- Per-thread IoRing implementation -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <stddef.h> // size_t — Clang freestanding builtin, not UCRT

#include "src/__support/CPP/scope_guard.h"
#include "src/__support/OSUtil/windows/io/batch_engine.h"
#include "src/__support/OSUtil/windows/io/thread_ring.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_ioring_ops.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/legacy/slab_pool.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace ioring {

// ---------------------------------------------------------------------------
// Process-wide thread ring pool
// ---------------------------------------------------------------------------

static internal::SlabPool ring_pool;

// Migrated to pool-managed TLS: the previous file-local static
// `ring_current_slab` was a process-wide cache (not thread_local) — every
// thread shared one slab pointer, racing on alloc_slow updates. The
// per-thread slab now lives in the pool's TEB TLS slot, abandoning via
// FLS at thread exit so dead-thread slabs reach the Crystalline retire
// path (sealed-with-live-slots → last-freer self-retire).
//
// The thread ring POINTER (the ring's owner-visible address) still lives
// in ThreadLifecycle::thread_ring as before — pool-managed TLS only
// caches the *slab*, not the ring.

// ---------------------------------------------------------------------------
// Reactor waker bridge callback
// ---------------------------------------------------------------------------
//
// Fires on the reactor drain thread when the IoRing completion event signals
// (CQ transitions empty→non-empty). Checks if the owning I/O thread is
// kernel-parked and fires NtAlertThreadByThreadId if so.
//
// Zero-bounce design: alert_if_parked() only READS park_state_ — it never
// writes to value_ or any field on the owner's cache line. The three cases:
//
//   Owner in Phase 0 (UMWAIT on cq->Tail): The kernel already wrote
//     cq->Tail (that's what triggered this event). The cache-line write
//     woke the owner's hardware monitor directly. park_state_ == NOT_PARKED.
//     alert_if_parked skips the NtAlert. No cross-core traffic to the
//     owner's cq_word line at all — the reactor only reads park_state_
//     (one snoop to Shared, owner line stays valid).
//
//   Owner in Phase 1 (NtWaitForAlertByThreadId): park_state_ == KERNEL_PARKED.
//     alert_if_parked fires NtAlert → owner wakes → re-checks cq->Tail →
//     finds CQE (kernel wrote it before signaling the event).
//
//   Owner running (fast-path pop): park_state_ == NOT_PARKED. Skip.
//
static void ioring_cq_waker(void *context, NTSTATUS /*status*/,
                             ULONG_PTR /*information*/) {
  auto *tr = static_cast<ThreadRing *>(context);
  ThreadLocalWord::alert_if_parked(&tr->cq_word);
  internal::reactor::rearm(tr->reactor_token);
}

// ---------------------------------------------------------------------------
// Registered buffer management (lazy-init, simple committed VA)
// ---------------------------------------------------------------------------
//
// Simple NtAllocateVirtualMemory committed region (no section, no double-map).
// Registered as a single IORING_BUFFER_INFO — one MDL built at registration
// time, reused for every I/O op (67% speedup for unbuffered 64K reads —
// research D4). Saves 256KB VA + section object vs the old double-mapped
// ring buffer approach.
//
// Lazy: not created at ThreadRing init. Only allocated on first O_DIRECT I/O
// via ensure_reg_buf(). Cached I/O threads never pay this cost.
//
// Best-effort: failure leaves reg_buf_ready = false and callers fall back
// to unregistered I/O transparently.

static bool reg_buf_init(ThreadRing *tr) {
  // Internal libc backing buffer — raw page_alloc (MEM_RESERVE|MEM_COMMIT).
  // Not a POSIX-surface allocation; does not go through the placeholder path.
  void *base = internal::page_alloc(ThreadRing::REG_BUF_SIZE);
  if (!base)
    return false;

  auto free_on_fail = cpp::make_scope_guard(
      [&] { nt_pal::free_va(base); });

  // Register as a single buffer entry for IORING_OP_FLAG_REGISTERED_BUFFER.
  IORING_BUFFER_INFO buf_info;
  buf_info.Address = base;
  buf_info.Length = ThreadRing::REG_BUF_SIZE;

  auto *sqe =
      push_register_buffers(&tr->ring, &buf_info, 1, /*user_data=*/0);
  if (!sqe)
    return false;

  NTSTATUS s = submit(&tr->ring, 1); // submit_wait(1)
  if (!NT_SUCCESS(s))
    return false;

  // Consume the registration CQE.
  NT_IORING_CQE cqe;
  if (!pop_cqe(&tr->ring, &cqe) || cqe.ResultCode < 0)
    return false;

  // Registration succeeded — commit state.
  free_on_fail.dismiss();
  tr->reg_buf_base = base;
  tr->reg_buf_head = 0;
  tr->reg_buf_tail = 0;
  tr->reg_buf_ready = true;
  return true;
}

static void reg_buf_destroy(ThreadRing *tr) {
  if (tr->reg_buf_base) {
    nt_pal::free_va(tr->reg_buf_base);
    tr->reg_buf_base = nullptr;
    tr->reg_buf_ready = false;
  }
}

// Out-of-line lazy init — called from ThreadRing::ensure_reg_buf().
bool ThreadRing::ensure_reg_buf() {
  if (reg_buf_ready)
    return true;
  return reg_buf_init(this);
}

// Close and free a ThreadRing. Called from lifecycle_cleanup on thread exit
// and from thread_ring_create on partial-init failure paths.
void thread_ring_destroy(void *ptr) {
  if (!ptr)
    return;
  auto *tr = static_cast<ThreadRing *>(ptr);
  // Deregister the reactor watch before closing handles. unwatch() guarantees
  // no callback is executing and none will fire after it returns.
  if (tr->reactor_token.valid())
    internal::reactor::unwatch(tr->reactor_token);
  // Free registered buffer ring before closing the ring (the kernel holds
  // MDL references to these pages while the ring is open).
  reg_buf_destroy(tr);
  close(&tr->ring);
  if (tr->event)
    NtClose(tr->event);
  internal::SlabPool::free(tr);
}

// Allocate and initialize a ThreadRing for the calling thread.
// Called from the slow path of get_thread_ring().
ThreadRing *thread_ring_create() {
  auto *lc = get_current_lifecycle();
  if (!lc)
    return nullptr;

  void *slot = ring_pool.tls_alloc();
  if (!slot)
    return nullptr;
  auto *tr = static_cast<ThreadRing *>(slot);
  auto free_mem = cpp::make_scope_guard([&] { internal::SlabPool::free(tr); });

  NTSTATUS s = create(&tr->ring, IORING_VERSION_3, THREAD_RING_SQ_SIZE,
                      THREAD_RING_CQ_SIZE);
  if (!NT_SUCCESS(s))
    return nullptr;
  auto close_ring = cpp::make_scope_guard([&] { close(&tr->ring); });

  auto oa = windows::internal_oa();
  s = NtCreateEvent(&tr->event, EVENT_MODIFY_STATE | SYNCHRONIZE, &oa,
                    SynchronizationEvent, FALSE);
  if (!NT_SUCCESS(s))
    return nullptr;
  auto close_event = cpp::make_scope_guard([&] { NtClose(tr->event); });

  s = set_completion_event(&tr->ring, tr->event);
  if (!NT_SUCCESS(s))
    return nullptr;

  // Initialize the CQ notification word. Sets owner_tid_ from the calling
  // thread's TEB (for NtAlertThreadByThreadId), clears value/generation/
  // park_state. Replaces the old needs_wake tri-state + wait_event handle.
  tr->cq_word.init();

  // Register the completion event with the reactor for the waker bridge.
  // When the CQ transitions empty→non-empty, the kernel signals tr->event,
  // IOCP delivers it to the reactor drain thread, and ioring_cq_waker
  // checks cq_word via ThreadLocalWord::alert_if_parked(&cq_word).
  tr->reactor_token =
      internal::reactor::watch(tr->event, ioring_cq_waker, tr);

  // Registered buffer is lazy — allocated on first O_DIRECT I/O via
  // ensure_reg_buf(). Cached I/O threads never pay this cost.

  // All steps succeeded — dismiss guards and commit.
  close_event.dismiss();
  close_ring.dismiss();
  free_mem.dismiss();
  lc->thread_ring = tr;
  return tr;
}

void thread_ring_init() {
  ring_pool.init(sizeof(ThreadRing), alignof(ThreadRing));
  // Pool-managed TLS so the slab abandons via FLS at thread exit and
  // sealed-with-live-slots Crystalline retire can run.
  ring_pool.init_tls(internal::kTlsCleanupPhaseAllocator);
}

// Explicit instantiation of the production BatchEngine type. All member
// functions are LIBC_INLINE (always_inline), so this does not emit
// addressable copies at call sites — the explicit instantiation exists to
// document the canonical instantiation and to cover the rare debug build
// where inlining is suppressed and a linkable body is required.
template class BatchEngineT<DefaultBatchCfg>;

} // namespace ioring
} // namespace LIBC_NAMESPACE_DECL

int LIBC_NAMESPACE::internal::thread_ring_startup_init() {
  LIBC_NAMESPACE::ioring::thread_ring_init();
  return 0;
}

void LIBC_NAMESPACE::internal::thread_ring_fork_reinit() {
  // Invalidate the forking thread's stale ring. All IoRing, event, and
  // wait_event handles were created with internal_oa() / NtCreateIoRing
  // (non-inheritable), so they don't exist in the child's handle table.
  // Just null pointers and free VA — no NtClose.
  //
  // This also frees the 256KB reg_buf VA that would otherwise leak in the
  // child for every thread that had done O_DIRECT I/O in the parent.
  // Dead (non-forking) threads' rings are abandoned — their slab memory is
  // reclaimed by ring_pool.fork_reinit() below, but any reg_buf VA they
  // held is leaked.  This is a known limitation; fixing it would require a
  // SlabPool live-object iterator which doesn't exist today.
  auto *lc = LIBC_NAMESPACE::get_current_lifecycle();
  if (lc && lc->thread_ring) {
    auto *tr = static_cast<LIBC_NAMESPACE::ioring::ThreadRing *>(
        lc->thread_ring);
    // Free registered buffer VA — MDL registration died with the ring.
    LIBC_NAMESPACE::ioring::reg_buf_destroy(tr);
    // Null stale non-inherited handle pointers — no NtClose.
    tr->ring.nt_handle = nullptr;
    tr->ring.sq = nullptr;
    tr->ring.cq = nullptr;
    tr->event = nullptr;
    tr->reactor_token = LIBC_NAMESPACE::internal::reactor::INVALID_TOKEN;
    // Don't call SlabPool::free() — pool is about to be reset below.
    lc->thread_ring = nullptr;
  }

  // Pool-managed TLS: SlabPool::fork_reinit handles slab re-claim under
  // the new TID. The TEB TLS slot survives fork (CoW); no explicit clear.
  LIBC_NAMESPACE::ioring::ring_pool.fork_reinit();
}

LIBC_REGISTER_FORK_REINIT(thread_ring,
                          ::LIBC_NAMESPACE::internal::kForkPrioThreadRing,
                          &::LIBC_NAMESPACE::internal::thread_ring_fork_reinit)
