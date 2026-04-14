//===-- Per-thread IoRing implementation -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <stddef.h> // size_t — Clang freestanding builtin, not UCRT

#include "src/__support/CPP/scope_guard.h"
#include "src/__support/OSUtil/windows/io/thread_ring.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_ioring_ops.h"
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace ioring {

// ---------------------------------------------------------------------------
// Process-wide thread ring pool
// ---------------------------------------------------------------------------

static internal::SlabPool ring_pool;
static internal::SlabPool::ThreadSlab ring_current_slab{nullptr};

// No longer owns a TEB slot — the ring pointer lives in
// ThreadLifecycle::thread_ring, accessed via get_current_lifecycle().

// ---------------------------------------------------------------------------
// Reactor waker bridge callback
// ---------------------------------------------------------------------------
//
// Fires on the reactor drain thread when the IoRing completion event signals
// (CQ transitions empty→non-empty). Wakes any I/O thread parked in
// futex_addr::wait_nt(&cq->Tail, ...) inside ioring_submit(), then re-arms
// the WCP for the next CQ transition.
// Reactor waker bridge — fires on the reactor drain thread when the IoRing
// completion event signals (CQ transitions empty→non-empty). Wakes the I/O
// thread via the mechanism indicated by needs_wake:
//   WAKE_NONE  (0) — no thread waiting, skip (fast-path ops consumed CQE sync)
//   WAKE_FUTEX (1) — thread parked on futex, use futex_addr::wake
//   WAKE_EVENT (2) — thread in alertable NtWaitForSingleObject on wait_event,
//                    signal wait_event (separate from the IoRing completion
//                    event, so no double-consumer race with the WCP)
static void ioring_cq_waker(void *context, NTSTATUS /*status*/,
                             ULONG_PTR /*information*/) {
  auto *tr = static_cast<ThreadRing *>(context);
  ULONG mode = __atomic_load_n(&tr->ring.needs_wake, __ATOMIC_ACQUIRE);
  if (mode == WAKE_FUTEX)
    futex_addr::wake(&tr->ring.cq->Tail, 1);
  else if (mode == WAKE_EVENT)
    ::NtSetEvent(tr->wait_event, nullptr);
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
  // One-shot reserve+commit — single NtAllocateVirtualMemoryEx call.
  void *base = windows::oneshot_alloc(nullptr, ThreadRing::REG_BUF_SIZE,
                                      PAGE_READWRITE);
  if (!base)
    return false;

  auto free_on_fail = cpp::make_scope_guard(
      [&] { windows::vm_release(base); });

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
    windows::vm_release(tr->reg_buf_base);
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
  if (tr->wait_event)
    NtClose(tr->wait_event);
  internal::SlabPool::free(tr);
}

// Allocate and initialize a ThreadRing for the calling thread.
// Called from the slow path of get_thread_ring().
ThreadRing *thread_ring_create() {
  auto *lc = get_current_lifecycle();
  if (!lc)
    return nullptr;

  void *slot = internal::SlabPool::alloc(ring_current_slab);
  if (!slot)
    slot = ring_pool.alloc_slow(&ring_current_slab);
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

  // Separate event for the alertable wait path (ioring_wait_cqe). This event
  // is NOT associated with the IoRing or the reactor WCP — it's signaled
  // explicitly by ioring_cq_waker when needs_wake == WAKE_EVENT. This avoids
  // the double-consumer race where both the WCP and NtWaitForSingleObject
  // compete for the same auto-reset IoRing completion event.
  auto oa2 = windows::internal_oa();
  s = NtCreateEvent(&tr->wait_event, EVENT_MODIFY_STATE | SYNCHRONIZE, &oa2,
                    SynchronizationEvent, FALSE);
  if (!NT_SUCCESS(s))
    return nullptr;
  auto close_wait_event = cpp::make_scope_guard([&] { NtClose(tr->wait_event); });

  // Register the completion event with the reactor for the waker bridge.
  // When the CQ transitions empty→non-empty, the kernel signals tr->event,
  // IOCP delivers it to the reactor drain thread, and ioring_cq_waker
  // wakes the I/O thread via futex (WAKE_FUTEX) or wait_event (WAKE_EVENT).
  tr->ring.owner_tid = ::NtCurrentThreadId();
  tr->reactor_token =
      internal::reactor::watch(tr->event, ioring_cq_waker, tr);

  // Registered buffer is lazy — allocated on first O_DIRECT I/O via
  // ensure_reg_buf(). Cached I/O threads never pay this cost.

  // All steps succeeded — dismiss guards and commit.
  close_wait_event.dismiss();
  close_event.dismiss();
  close_ring.dismiss();
  free_mem.dismiss();
  lc->thread_ring = tr;
  return tr;
}

void thread_ring_init() {
  ring_pool.init(sizeof(ThreadRing), alignof(ThreadRing));
}

void thread_ring_shutdown() {
  // No TEB slot to free. Per-thread cleanup happens in lifecycle_cleanup.
}

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
    tr->wait_event = nullptr;
    tr->reactor_token = LIBC_NAMESPACE::internal::reactor::INVALID_TOKEN;
    // Don't call SlabPool::free() — pool is about to be reset below.
    lc->thread_ring = nullptr;
  }

  LIBC_NAMESPACE::ioring::ring_current_slab = nullptr;
  LIBC_NAMESPACE::ioring::ring_pool.fork_reinit();
}
