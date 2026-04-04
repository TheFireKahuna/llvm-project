//===-- Tests for the Crystalline-W thread registry -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Basic-correctness coverage of the thread_registry / ThreadLifecycle
// design that replaced the EpochGuard-pinned page-slot registry. Each
// test exercises ONE invariant of the new design:
//
//   * Lookup by task_id / handle resolves a freshly-registered thread.
//   * task_id is monotonic and never recycled (even across pthread_join).
//   * A {tid, task_id} handle captured before join resolves to nullptr
//     after join completes (Crystalline retire drained).
//   * A handle whose task_id matches but whose tid doesn't is rejected
//     (belt-and-suspenders — task_id alone is authoritative since it
//     never recycles, but the tid sanity check guards against captured
//     handles for a thread that was later reaped + replaced).
//   * pt->platform_data bypass returns the same lifecycle pointer that
//     registry_find_by_task_id returns — proves Pattern A is wired
//     correctly through Thread::run().
//   * registry_for_each visits every live registered thread (Harris
//     filter handles deregistered entries that haven't been physically
//     unlinked yet). Verified twice: once for visit-count + de-dup,
//     once composed into a TID buffer for membership.
//   * current_thread_handle from inside a libc-managed thread returns
//     a valid handle; from a raw NtCreateThreadEx thread it returns
//     ThreadHandle::invalid().
//   * registry_live_count tracks register/deregister exactly.
//
// Concurrency stress lives in thread_registry_stress_test.cpp; this
// file deliberately keeps thread counts low so a regression in basic
// correctness is unambiguous.
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

// =========================================================================
// Worker primitives
//
// Two-phase rendezvous: the worker stores its captured {task_id, tid} into
// the context and signals `ready`; the test then drives observations and
// finally signals `release` so the worker exits and lets the test join.
// The handshake is deterministic — the worker is guaranteed to be
// registered before the test inspects any registry state, and the test
// is guaranteed to finish its inspection before the worker exits.
// =========================================================================

struct WorkerCtx {
  Atomic<uint32_t> ready{0};
  Atomic<uint32_t> release{0};
  Atomic<uint32_t> captured_task_id{0};
  Atomic<uint32_t> captured_tid{0};
  Atomic<bool> current_handle_valid{false};
  Atomic<uint32_t> current_handle_task_id{0};
  Atomic<uint32_t> current_handle_tid{0};
  // Lifecycle pointer the worker observed for itself — used to compare
  // against what the parent sees through pt->platform_data and through
  // registry_find_by_task_id.
  Atomic<void *> self_lifecycle{nullptr};
};

static void *worker_capture_then_park(void *arg) {
  auto *ctx = static_cast<WorkerCtx *>(arg);
  ThreadLifecycle *self = get_current_lifecycle();
  if (self) {
    ctx->self_lifecycle.store(self, MemoryOrder::RELAXED);
    ctx->captured_task_id.store(self->task_id, MemoryOrder::RELAXED);
    ctx->captured_tid.store(self->tid, MemoryOrder::RELAXED);
  }
  ThreadHandle h = current_thread_handle();
  ctx->current_handle_valid.store(h.is_valid(), MemoryOrder::RELAXED);
  ctx->current_handle_task_id.store(h.task_id, MemoryOrder::RELAXED);
  ctx->current_handle_tid.store(h.tid, MemoryOrder::RELAXED);
  ctx->ready.store(1, MemoryOrder::RELEASE);
  while (ctx->release.load(MemoryOrder::ACQUIRE) == 0)
    test_support::sleep_ms(0);
  return nullptr;
}

// Spin until the worker has registered and published its identity.
static void wait_ready(WorkerCtx &ctx) {
  while (ctx.ready.load(MemoryOrder::ACQUIRE) == 0)
    test_support::sleep_ms(0);
}

// =========================================================================
// 1. Basic register/find/resolve.
// =========================================================================

TEST(LlvmLibcThreadRegistry, FindByTaskIdReturnsRegisteredLifecycle) {
  WorkerCtx ctx;
  Thread th;
  ASSERT_EQ(th.run(worker_capture_then_park, &ctx), 0);
  wait_ready(ctx);

  uint32_t task_id = ctx.captured_task_id.load(MemoryOrder::ACQUIRE);
  ASSERT_NE(task_id, 0u);

  ThreadLifecycle *lc = registry_find_by_task_id(task_id);
  ASSERT_NE(lc, static_cast<ThreadLifecycle *>(nullptr));
  EXPECT_EQ(lc->task_id, task_id);
  EXPECT_EQ(lc->tid, ctx.captured_tid.load(MemoryOrder::ACQUIRE));

  // The bypass pointer the parent has access to MUST match the registry's
  // resolved pointer — proves Pattern A and the registry are coherent.
  auto *bypass = static_cast<ThreadLifecycle *>(th.attrib->platform_data);
  EXPECT_EQ(bypass, lc);
  EXPECT_EQ(bypass, ctx.self_lifecycle.load(MemoryOrder::ACQUIRE));

  ctx.release.store(1, MemoryOrder::RELEASE);
  ASSERT_EQ(th.join(static_cast<void **>(nullptr)), 0);
}

TEST(LlvmLibcThreadRegistry, ResolveByHandleSucceedsForLiveThread) {
  WorkerCtx ctx;
  Thread th;
  ASSERT_EQ(th.run(worker_capture_then_park, &ctx), 0);
  wait_ready(ctx);

  ThreadHandle h{ctx.captured_tid.load(MemoryOrder::ACQUIRE),
                 ctx.captured_task_id.load(MemoryOrder::ACQUIRE)};
  ASSERT_TRUE(h.is_valid());

  ThreadLifecycle *lc = registry_resolve(h);
  ASSERT_NE(lc, static_cast<ThreadLifecycle *>(nullptr));
  EXPECT_EQ(lc->task_id, h.task_id);
  EXPECT_EQ(lc->tid, h.tid);

  ctx.release.store(1, MemoryOrder::RELEASE);
  ASSERT_EQ(th.join(static_cast<void **>(nullptr)), 0);
}

TEST(LlvmLibcThreadRegistry, CurrentThreadHandleValidInsideLibcThread) {
  WorkerCtx ctx;
  Thread th;
  ASSERT_EQ(th.run(worker_capture_then_park, &ctx), 0);
  wait_ready(ctx);

  EXPECT_TRUE(ctx.current_handle_valid.load(MemoryOrder::ACQUIRE));
  EXPECT_EQ(ctx.current_handle_task_id.load(MemoryOrder::ACQUIRE),
            ctx.captured_task_id.load(MemoryOrder::ACQUIRE));
  EXPECT_EQ(ctx.current_handle_tid.load(MemoryOrder::ACQUIRE),
            ctx.captured_tid.load(MemoryOrder::ACQUIRE));

  ctx.release.store(1, MemoryOrder::RELEASE);
  ASSERT_EQ(th.join(static_cast<void **>(nullptr)), 0);
}

// =========================================================================
// 2. task_id never recycles, even across join+free.
//
// Crystalline retires the joined lifecycle's slab slot and the slot may
// be re-handed to a future thread — but the new thread MUST get a
// strictly larger task_id. This is the structural defense the rewrite
// delivers; if task_id ever repeated, the captured-handle protocol falls
// apart.
// =========================================================================

TEST(LlvmLibcThreadRegistry, TaskIdMonotonicAcrossJoin) {
  WorkerCtx first_ctx;
  Thread first_th;
  ASSERT_EQ(first_th.run(worker_capture_then_park, &first_ctx), 0);
  wait_ready(first_ctx);
  uint32_t first_task_id = first_ctx.captured_task_id.load(MemoryOrder::ACQUIRE);
  uint32_t first_tid = first_ctx.captured_tid.load(MemoryOrder::ACQUIRE);

  first_ctx.release.store(1, MemoryOrder::RELEASE);
  ASSERT_EQ(first_th.join(static_cast<void **>(nullptr)), 0);

  // Second thread — even if NT recycles the TID and the slab pool reuses
  // the lifecycle slot, task_id must be strictly larger.
  WorkerCtx second_ctx;
  Thread second_th;
  ASSERT_EQ(second_th.run(worker_capture_then_park, &second_ctx), 0);
  wait_ready(second_ctx);
  uint32_t second_task_id =
      second_ctx.captured_task_id.load(MemoryOrder::ACQUIRE);

  EXPECT_GT(second_task_id, first_task_id);

  // Capturing a handle pinned to the OLD task_id must NOT resolve to
  // the new thread, even if the new thread happens to have the same NT
  // tid. We can't force NT recycling deterministically, but resolving
  // {first_tid, first_task_id} after first_th.join() must yield nullptr
  // regardless: registry_find_by_task_id returns nullptr (entry retired)
  // OR the tid sanity check rejects a recycled-slot match.
  ThreadHandle stale{first_tid, first_task_id};
  EXPECT_EQ(registry_resolve(stale), static_cast<ThreadLifecycle *>(nullptr));

  second_ctx.release.store(1, MemoryOrder::RELEASE);
  ASSERT_EQ(second_th.join(static_cast<void **>(nullptr)), 0);
}

// =========================================================================
// 3. Resolve rejects a forged tid mismatch.
//
// Defense-in-depth: registry_resolve must check the resolved lifecycle's
// tid against the captured handle's tid. A stale handle with the right
// task_id but wrong tid would only happen via memory corruption or test
// forgery, but the resolve path must still reject it — the same code
// path is what catches the rare case of a captured handle pointing at
// a since-retired-and-recycled task_id slot.
// =========================================================================

TEST(LlvmLibcThreadRegistry, ResolveRejectsTidMismatch) {
  WorkerCtx ctx;
  Thread th;
  ASSERT_EQ(th.run(worker_capture_then_park, &ctx), 0);
  wait_ready(ctx);

  uint32_t task_id = ctx.captured_task_id.load(MemoryOrder::ACQUIRE);
  uint32_t tid = ctx.captured_tid.load(MemoryOrder::ACQUIRE);

  // Sanity: correct handle resolves.
  ThreadHandle good{tid, task_id};
  EXPECT_NE(registry_resolve(good), static_cast<ThreadLifecycle *>(nullptr));

  // Forged: same task_id but a tid that's guaranteed not to match (XOR
  // toggles a bit; pick one that also avoids the main thread's tid).
  uint32_t forged_tid = tid ^ 0x55AA55AA;
  if (forged_tid == 0)
    forged_tid = 0xDEADBEEF;
  ThreadHandle bad{forged_tid, task_id};
  EXPECT_EQ(registry_resolve(bad), static_cast<ThreadLifecycle *>(nullptr));

  ctx.release.store(1, MemoryOrder::RELEASE);
  ASSERT_EQ(th.join(static_cast<void **>(nullptr)), 0);
}

TEST(LlvmLibcThreadRegistry, FindByTaskIdRejectsZero) {
  EXPECT_EQ(registry_find_by_task_id(0),
            static_cast<ThreadLifecycle *>(nullptr));
  EXPECT_EQ(registry_resolve(ThreadHandle::invalid()),
            static_cast<ThreadLifecycle *>(nullptr));
}

// =========================================================================
// 4. Iteration: registry_for_each visits the expected live set,
//    composes into a TID buffer, no duplicates, no misses.
//
// We launch N workers and rendezvous each via its WorkerCtx. While they
// are parked at `release==0`, the registry MUST report exactly the
// expected delta over the test's baseline (other system threads may
// exist; we use baseline subtraction to make the test robust against
// background activity).
// =========================================================================

TEST(LlvmLibcThreadRegistry, ForEachAndCollectTidsAgree) {
  constexpr int N = 8;
  WorkerCtx ctxs[N];
  Thread ths[N];

  uint32_t baseline = registry_live_count();

  for (int i = 0; i < N; ++i)
    ASSERT_EQ(ths[i].run(worker_capture_then_park, &ctxs[i]), 0);
  for (int i = 0; i < N; ++i)
    wait_ready(ctxs[i]);

  // live_count must reflect the new threads.
  EXPECT_EQ(registry_live_count(), baseline + N);

  // for_each must see every captured task_id in our worker set.
  // Background threads are also visible — we filter to "ours" by task_id
  // membership. A correct registry surfaces each "ours" thread exactly
  // once (Harris-list semantics; no duplicate visits).
  bool seen[N] = {};
  uint32_t for_each_count = 0;
  uint32_t our_tid_count = 0;
  registry_for_each([&](ThreadLifecycle *lc) {
    ++for_each_count;
    for (int i = 0; i < N; ++i) {
      if (lc->task_id == ctxs[i].captured_task_id.load(MemoryOrder::ACQUIRE)) {
        EXPECT_FALSE(seen[i]); // No duplicate visits.
        seen[i] = true;
        ++our_tid_count;
      }
    }
    return false;
  });
  EXPECT_EQ(our_tid_count, static_cast<uint32_t>(N));
  for (int i = 0; i < N; ++i)
    EXPECT_TRUE(seen[i]);

  // Compose registry_for_each into a TID buffer to verify membership.
  // Buffer is sized for baseline+N; the registry's API doesn't itself
  // expose a "collect into buffer" entry point because every variant
  // would have fragile truncation semantics — the walker is the
  // primitive, callers compose it for their needs.
  constexpr uint32_t kCap = 256;
  uint32_t observed_tids[kCap];
  uint32_t collected = 0;
  registry_for_each([&](ThreadLifecycle *lc) -> bool {
    if (collected < kCap)
      observed_tids[collected++] = lc->tid;
    return false;
  });
  // Buffer must have been large enough to hold every visited thread —
  // otherwise the membership check below is unsound.
  ASSERT_LT(collected, kCap);
  EXPECT_EQ(collected, for_each_count);

  for (int i = 0; i < N; ++i) {
    uint32_t want_tid = ctxs[i].captured_tid.load(MemoryOrder::ACQUIRE);
    bool found = false;
    for (uint32_t j = 0; j < collected; ++j) {
      if (observed_tids[j] == want_tid) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found);
  }

  for (int i = 0; i < N; ++i)
    ctxs[i].release.store(1, MemoryOrder::RELEASE);
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(ths[i].join(static_cast<void **>(nullptr)), 0);

  // Wait for live_count to drift back down. Reclamation is asynchronous
  // (Crystalline retire); poll briefly with a generous bound.
  for (int i = 0; i < 200; ++i) {
    if (registry_live_count() == baseline)
      break;
    test_support::sleep_ms(5);
  }
  EXPECT_EQ(registry_live_count(), baseline);
}

// =========================================================================
// 5. live_count tracks register/deregister exactly across many cycles.
//
// Every thread create increments it by 1; every join (or detach + exit)
// decrements it by 1. The test runs multiple waves to stress the
// add/sub atomic without hitting the iter list's mark/unlink races
// directly (those are covered in the stress test).
// =========================================================================

TEST(LlvmLibcThreadRegistry, LiveCountTracksLifecycle) {
  uint32_t baseline = registry_live_count();
  constexpr int WAVES = 4;
  constexpr int N = 4;

  for (int w = 0; w < WAVES; ++w) {
    WorkerCtx ctxs[N];
    Thread ths[N];
    for (int i = 0; i < N; ++i)
      ASSERT_EQ(ths[i].run(worker_capture_then_park, &ctxs[i]), 0);
    for (int i = 0; i < N; ++i)
      wait_ready(ctxs[i]);

    EXPECT_EQ(registry_live_count(), baseline + N);

    for (int i = 0; i < N; ++i)
      ctxs[i].release.store(1, MemoryOrder::RELEASE);
    for (int i = 0; i < N; ++i)
      ASSERT_EQ(ths[i].join(static_cast<void **>(nullptr)), 0);

    // Drift back to baseline. Joins synchronize the deregister, but
    // Crystalline retire of the lifecycle node may lag — the live_count
    // decrement happens in registry_deregister BEFORE the retire, so
    // the count should match baseline immediately after join. If it
    // doesn't, there's a counting bug.
    EXPECT_EQ(registry_live_count(), baseline);
  }
}

// =========================================================================
// 6. Foreign thread (raw NtCreateThreadEx, never registered): observable
//    invariants.
//
// A raw thread created with test_support::create_thread bypasses
// Thread::run and never calls registry_register. From inside that thread:
//   * get_current_lifecycle() may return nullptr (no TLS root set) OR
//     return a lifecycle that some other foreign-thread integration
//     (robust mutex / signal init) has lazily attached. Both are valid;
//     the test only requires that current_thread_handle is well-defined.
//   * If current_thread_handle returns valid, the resolution must
//     round-trip: the tid in the handle equals NtCurrentThreadId.
// =========================================================================

struct ForeignCtx {
  Atomic<bool> done{false};
  Atomic<bool> handle_valid{false};
  Atomic<uint32_t> handle_tid{0};
  Atomic<uint32_t> nt_tid{0};
};

static DWORD foreign_capture(void *arg) {
  auto *ctx = static_cast<ForeignCtx *>(arg);
  ctx->nt_tid.store(NtCurrentThreadId(), MemoryOrder::RELAXED);
  ThreadHandle h = current_thread_handle();
  ctx->handle_valid.store(h.is_valid(), MemoryOrder::RELAXED);
  ctx->handle_tid.store(h.tid, MemoryOrder::RELAXED);
  ctx->done.store(true, MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcThreadRegistry, ForeignThreadCurrentHandleSelfConsistent) {
  ForeignCtx ctx;
  HANDLE t = test_support::create_thread(foreign_capture, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));
  ASSERT_EQ(test_support::wait_for_single_object(t, 5000),
            static_cast<DWORD>(test_support::WAIT_OBJECT_0));
  ::NtClose(t);

  // The foreign thread either had no lifecycle (handle invalid — the
  // baseline post-rewrite expectation) OR got a lifecycle attached by
  // some other lazy-init path. In the latter case, the tid must
  // match the kernel's view.
  if (ctx.handle_valid.load(MemoryOrder::ACQUIRE)) {
    EXPECT_EQ(ctx.handle_tid.load(MemoryOrder::ACQUIRE),
              ctx.nt_tid.load(MemoryOrder::ACQUIRE));
  } else {
    EXPECT_EQ(ctx.handle_tid.load(MemoryOrder::ACQUIRE), 0u);
  }
}

// =========================================================================
// 7. registry_for_each early-termination contract: visitor returning
//    true short-circuits the walk.
// =========================================================================

TEST(LlvmLibcThreadRegistry, ForEachEarlyTerminate) {
  constexpr int N = 4;
  WorkerCtx ctxs[N];
  Thread ths[N];
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(ths[i].run(worker_capture_then_park, &ctxs[i]), 0);
  for (int i = 0; i < N; ++i)
    wait_ready(ctxs[i]);

  // First-match semantics: visitor returning true must stop the walk.
  uint32_t target =
      ctxs[N - 1].captured_task_id.load(MemoryOrder::ACQUIRE);
  uint32_t visit_count = 0;
  ThreadLifecycle *found = nullptr;
  bool early_terminated = registry_for_each([&](ThreadLifecycle *lc) {
    ++visit_count;
    if (lc->task_id == target) {
      found = lc;
      return true;
    }
    return false;
  });
  EXPECT_TRUE(early_terminated);
  ASSERT_NE(found, static_cast<ThreadLifecycle *>(nullptr));
  EXPECT_EQ(found->task_id, target);
  // visit_count <= live_count: if the visitor returned true on the first
  // matching entry, walk stopped immediately. Lower bound is 1.
  EXPECT_GE(visit_count, 1u);
  EXPECT_LE(visit_count, registry_live_count() + 1);

  for (int i = 0; i < N; ++i)
    ctxs[i].release.store(1, MemoryOrder::RELEASE);
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(ths[i].join(static_cast<void **>(nullptr)), 0);
}

// =========================================================================
// 8. After join, the captured handle must NOT resolve to the same
//    lifecycle. The lifecycle's slab slot may be reused by a future
//    thread — but task_id won't recycle, so registry_find_by_task_id
//    returns nullptr (or, defensively, a different thread whose tid
//    sanity check fails).
// =========================================================================

TEST(LlvmLibcThreadRegistry, CapturedHandleAfterJoinResolvesNull) {
  WorkerCtx ctx;
  Thread th;
  ASSERT_EQ(th.run(worker_capture_then_park, &ctx), 0);
  wait_ready(ctx);

  ThreadHandle captured{ctx.captured_tid.load(MemoryOrder::ACQUIRE),
                        ctx.captured_task_id.load(MemoryOrder::ACQUIRE)};

  // While the worker is parked, the handle resolves.
  EXPECT_NE(registry_resolve(captured),
            static_cast<ThreadLifecycle *>(nullptr));

  ctx.release.store(1, MemoryOrder::RELEASE);
  ASSERT_EQ(th.join(static_cast<void **>(nullptr)), 0);

  // After join, deregister has run and live_count was decremented.
  // registry_resolve MUST NOT return a live lifecycle for this handle.
  // Two valid outcomes:
  //   * find_by_task_id(captured.task_id) returns nullptr (the bucket
  //     entry was unlinked), OR
  //   * a different thread is now at the same slab slot — but its
  //     task_id is strictly newer (monotonic counter), so the lookup
  //     by the captured task_id misses.
  EXPECT_EQ(registry_resolve(captured),
            static_cast<ThreadLifecycle *>(nullptr));
  EXPECT_EQ(registry_find_by_task_id(captured.task_id),
            static_cast<ThreadLifecycle *>(nullptr));
}

} // namespace
} // namespace LIBC_NAMESPACE_DECL
