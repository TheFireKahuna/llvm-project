//===-- TID-recycling regression test for the thread registry -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The structural claim of the Crystalline-W registry rewrite is that
// libc's cross-thread references — `ThreadHandle = {tid, task_id}` —
// are immune to NT-TID recycling. The previous registry stored TIDs
// alone in places like wait_slot's owner field; the kernel reuses TID
// values once the original thread exits, so a captured TID could later
// alert/resolve a different thread that happens to inherit the same
// numeric ID. This is "Flavor B staleness" and was the motivation for
// the rewrite.
//
// The new design pairs every captured TID with a 30-bit monotonic
// task_id that NEVER recycles within a process. registry_resolve uses
// task_id as the authoritative identity; the tid sanity check is
// belt-and-suspenders. So even if NT recycles a thread's numeric TID,
// the captured handle either:
//   * Resolves to nullptr (because task_id was retired), OR
//   * Resolves to the same lifecycle (because the thread is still
//     alive — but in that case the kernel hasn't recycled the TID).
//
// What this test exercises
// ------------------------
//
//   1. CapturedHandleNeverCrossResolves
//      Capture a handle for thread A. Join A. Spawn many short-lived
//      thread waves; if NT recycles A's tid for any of them, the test
//      catches it but verifies registry_resolve(captured) STILL returns
//      nullptr — task_id mismatch is correctly rejected.
//
//   2. RecycledTidGetsFreshTaskId
//      Same scenario as above. When a recycled tid is observed in the
//      live thread's handle, that live thread MUST report a different
//      task_id than the captured one. (Otherwise the rewrite's
//      monotonic-task_id invariant is broken.)
//
//   3. RegistryFindByTidVestigial
//      Document that registry_find_by_task_id (the canonical path)
//      doesn't accept a TID — only a task_id. Forging by tid alone
//      isn't even an API we expose. This test pins the API contract.
//
// The test cannot deterministically force NT to recycle a TID — that's
// a kernel-internal allocation policy. We drive enough churn that
// recycling is overwhelmingly likely on a busy CI machine, but the
// test passes equally if no recycling is observed (the captured handle
// just stays unresolved). The test FAILS only if the registry returns
// the *wrong* lifecycle — i.e., the structural claim is violated.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/threads/sleep.h"
#include "src/__support/threads/thread.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

namespace LIBC_NAMESPACE_DECL {
namespace {

using cpp::Atomic;
using cpp::MemoryOrder;

// Worker that captures its identity and exits immediately. Used to
// generate many short-lived threads quickly — high churn is what
// drives NT into reusing recently-freed TID slots.
struct CaptureCtx {
  Atomic<uint32_t> task_id{0};
  Atomic<uint32_t> tid{0};
};

static void *capture_then_exit(void *arg) {
  auto *ctx = static_cast<CaptureCtx *>(arg);
  ThreadLifecycle *self = get_current_lifecycle();
  if (self) {
    ctx->task_id.store(self->task_id, MemoryOrder::RELEASE);
    ctx->tid.store(self->tid, MemoryOrder::RELEASE);
  }
  return nullptr;
}

// =========================================================================
// 1. CapturedHandleNeverCrossResolves
//
// The hard claim: a stale captured handle MUST NOT resolve to any
// thread, regardless of whether NT later recycled the captured TID.
// =========================================================================

TEST(LlvmLibcThreadRegistryRecycling, CapturedHandleNeverCrossResolves) {
  // Step 1: spawn anchor thread, capture handle, join.
  CaptureCtx anchor;
  Thread anchor_th;
  ASSERT_EQ(anchor_th.run(capture_then_exit, &anchor), 0);
  ASSERT_EQ(anchor_th.join(static_cast<void **>(nullptr)), 0);

  uint32_t anchor_task_id = anchor.task_id.load(MemoryOrder::ACQUIRE);
  uint32_t anchor_tid = anchor.tid.load(MemoryOrder::ACQUIRE);
  ASSERT_NE(anchor_task_id, 0u);
  ASSERT_NE(anchor_tid, 0u);

  ThreadHandle captured{anchor_tid, anchor_task_id};

  // Step 2: drive churn. On every iteration, verify the captured
  // handle still resolves to nullptr — even if NT just reused
  // anchor_tid for the latest thread, the task_id mismatch must
  // suppress the resolution.
  //
  // The loop count balances cost vs. likelihood of observing a
  // recycle. 1024 short-lived threads is enough for NT to reuse
  // recently-freed TID slots on essentially any kernel build.
  constexpr int CHURN = 1024;
  bool ever_observed_recycle = false;
  for (int i = 0; i < CHURN; ++i) {
    CaptureCtx churn;
    Thread churn_th;
    ASSERT_EQ(churn_th.run(capture_then_exit, &churn), 0);
    ASSERT_EQ(churn_th.join(static_cast<void **>(nullptr)), 0);

    uint32_t churn_tid = churn.tid.load(MemoryOrder::ACQUIRE);
    if (churn_tid == anchor_tid)
      ever_observed_recycle = true;

    // The crucial invariant: the captured handle MUST resolve to
    // nullptr, period. If a churn thread happened to recycle the
    // anchor's tid, that thread's task_id is necessarily different
    // (monotonic counter) and registry_resolve must reject the
    // captured handle on either find-by-task_id miss or tid sanity.
    ThreadLifecycle *resolved = registry_resolve(captured);
    EXPECT_EQ(resolved, static_cast<ThreadLifecycle *>(nullptr));
    EXPECT_EQ(registry_find_by_task_id(captured.task_id),
              static_cast<ThreadLifecycle *>(nullptr));
  }

  // Print the recycling outcome so a human reader can see whether
  // the test exercised the recycle path. The test passes whether
  // recycling was observed or not — the assertion is about
  // resolution, not occurrence.
  (void)ever_observed_recycle;
}

// =========================================================================
// 2. RecycledTidGetsFreshTaskId
//
// Concurrent variant: keep a slow-thread alive that holds task_id
// `T_old`, exit it, then aggressively churn while watching for a
// thread to pick up T_old's tid. When that happens, that thread's
// task_id MUST be strictly greater than T_old's (monotonicity is
// the load-bearing invariant; if it fails, every captured-handle
// site in the rewrite breaks).
// =========================================================================

struct ParkCtx {
  Atomic<uint32_t> ready{0};
  Atomic<uint32_t> release{0};
  Atomic<uint32_t> task_id{0};
  Atomic<uint32_t> tid{0};
};

static void *park_until_release(void *arg) {
  auto *ctx = static_cast<ParkCtx *>(arg);
  ThreadLifecycle *self = get_current_lifecycle();
  if (self) {
    ctx->task_id.store(self->task_id, MemoryOrder::RELAXED);
    ctx->tid.store(self->tid, MemoryOrder::RELAXED);
  }
  ctx->ready.store(1, MemoryOrder::RELEASE);
  while (ctx->release.load(MemoryOrder::ACQUIRE) == 0)
    test_support::sleep_ms(0);
  return nullptr;
}

TEST(LlvmLibcThreadRegistryRecycling, RecycledTidGetsFreshTaskId) {
  // Spawn anchor, capture, release, join.
  ParkCtx anchor;
  Thread anchor_th;
  ASSERT_EQ(anchor_th.run(park_until_release, &anchor), 0);
  while (anchor.ready.load(MemoryOrder::ACQUIRE) == 0)
    test_support::sleep_ms(0);
  uint32_t anchor_task_id = anchor.task_id.load(MemoryOrder::ACQUIRE);
  uint32_t anchor_tid = anchor.tid.load(MemoryOrder::ACQUIRE);
  ASSERT_NE(anchor_task_id, 0u);
  anchor.release.store(1, MemoryOrder::RELEASE);
  ASSERT_EQ(anchor_th.join(static_cast<void **>(nullptr)), 0);

  // Churn-and-watch. Stop early if we observe the recycle.
  constexpr int CHURN = 2048;
  bool observed_recycle = false;
  uint32_t recycled_task_id = 0;
  for (int i = 0; i < CHURN; ++i) {
    CaptureCtx ctx;
    Thread th;
    ASSERT_EQ(th.run(capture_then_exit, &ctx), 0);
    ASSERT_EQ(th.join(static_cast<void **>(nullptr)), 0);
    uint32_t got_tid = ctx.tid.load(MemoryOrder::ACQUIRE);
    if (got_tid == anchor_tid) {
      observed_recycle = true;
      recycled_task_id = ctx.task_id.load(MemoryOrder::ACQUIRE);
      break;
    }
  }

  if (observed_recycle) {
    // The structural claim: monotonic task_id, never recycled.
    EXPECT_GT(recycled_task_id, anchor_task_id);
  }
  // If we did not observe recycling, the test still passes — kernels
  // vary on TID reuse policy, and the "no false positive" assertion
  // in test #1 already covers correctness regardless.
}

// =========================================================================
// 3. RegistryFindByTaskIdRejectsTaskIdZero
//
// task_id 0 is reserved (the robust-mutex protocol uses TID 0 for
// "no owner" and the rewrite preserves that). registry_find_by_task_id
// MUST reject 0 unconditionally — handing back a lifecycle for "no
// owner" would be a category error.
// =========================================================================

TEST(LlvmLibcThreadRegistryRecycling, RegistryFindByTaskIdRejectsZero) {
  EXPECT_EQ(registry_find_by_task_id(0),
            static_cast<ThreadLifecycle *>(nullptr));
  EXPECT_EQ(registry_resolve(ThreadHandle::invalid()),
            static_cast<ThreadLifecycle *>(nullptr));

  // Even with a non-zero tid, task_id == 0 must reject.
  ThreadHandle bad_only_tid{NtCurrentThreadId(), 0};
  EXPECT_FALSE(bad_only_tid.is_valid());
  EXPECT_EQ(registry_resolve(bad_only_tid),
            static_cast<ThreadLifecycle *>(nullptr));
}

// =========================================================================
// 4. CapturedHandleSurvivesIntervening — the alternate scenario.
//
// While a captured handle is held, intervening thread create+joins MUST
// NOT cause the original captured thread (which is still parked) to be
// inadvertently retired or reassigned. Verifies cross-thread isolation
// of captured references — the captured lifecycle stays resolvable as
// long as the underlying thread is alive, regardless of churn around
// it.
// =========================================================================

TEST(LlvmLibcThreadRegistryRecycling, CapturedHandleSurvivesIntervening) {
  ParkCtx host;
  Thread host_th;
  ASSERT_EQ(host_th.run(park_until_release, &host), 0);
  while (host.ready.load(MemoryOrder::ACQUIRE) == 0)
    test_support::sleep_ms(0);

  ThreadHandle host_handle{host.tid.load(MemoryOrder::ACQUIRE),
                           host.task_id.load(MemoryOrder::ACQUIRE)};
  ASSERT_TRUE(host_handle.is_valid());

  // Spam short-lived threads while the host is parked.
  constexpr int CHURN = 256;
  for (int i = 0; i < CHURN; ++i) {
    CaptureCtx ctx;
    Thread th;
    ASSERT_EQ(th.run(capture_then_exit, &ctx), 0);
    ASSERT_EQ(th.join(static_cast<void **>(nullptr)), 0);

    // After every churn iteration, the host must still resolve.
    ThreadLifecycle *resolved = registry_resolve(host_handle);
    ASSERT_NE(resolved, static_cast<ThreadLifecycle *>(nullptr));
    EXPECT_EQ(resolved->task_id, host_handle.task_id);
    EXPECT_EQ(resolved->tid, host_handle.tid);
  }

  host.release.store(1, MemoryOrder::RELEASE);
  ASSERT_EQ(host_th.join(static_cast<void **>(nullptr)), 0);

  // After join, host stops resolving.
  EXPECT_EQ(registry_resolve(host_handle),
            static_cast<ThreadLifecycle *>(nullptr));
}

} // namespace
} // namespace LIBC_NAMESPACE_DECL
