//===-- Stress tests for the Crystalline-W thread registry ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Concurrency / scale coverage of the thread registry. Each test
// targets a different class of failure that basic-correctness tests
// can't surface:
//
//   1. HashSplitUnderRegistration
//      Drive thread count past the 16-bucket round-base several
//      times (load factor 4 ⇒ split triggers at 64 live threads;
//      then 96, 128, ...). Every live task_id must remain findable
//      throughout — the split protocol's split_pending mid-state is
//      where transient "thread not found" against a known-live
//      target would surface.
//
//   2. HashLookupUnderConcurrentSplit
//      Multiple worker threads spam registry_find_by_task_id() on
//      a shared roster while another batch of threads register and
//      deregister, forcing splits. Lookups must NEVER misresolve
//      (return wrong lifecycle) and must never crash.
//
//   3. IterateAllUnderChurn
//      One thread iterates registry_for_each in a loop, gathering
//      a snapshot of task_ids; other threads spawn/join. Each
//      individual snapshot must contain no duplicates (Harris semantics)
//      and only valid task_ids; live counts must be self-consistent
//      after churn settles.
//
//   4. CreateExitChurnNoLeak
//      10K thread create+join cycles. live_count must return to its
//      baseline at the end (every register paired with a deregister).
//      Stresses Crystalline retire batching and the SlabPool reclaim
//      path through the .CRT$XLC abandon callback.
//
//   5. AlertAllWhileRegistering
//      registry_alert_all() running concurrently with thread create.
//      Validates the iter-list snapshot the alert path takes is
//      consistent against an evolving registry.
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
// Common worker shape: capture identity, park until release.
// =========================================================================

struct ParkSlot {
  Atomic<uint32_t> ready{0};
  Atomic<uint32_t> release{0};
  Atomic<uint32_t> task_id{0};
  Atomic<uint32_t> tid{0};
};

static void *park_worker(void *arg) {
  auto *slot = static_cast<ParkSlot *>(arg);
  ThreadLifecycle *self = get_current_lifecycle();
  if (self) {
    slot->task_id.store(self->task_id, MemoryOrder::RELAXED);
    slot->tid.store(self->tid, MemoryOrder::RELAXED);
  }
  slot->ready.store(1, MemoryOrder::RELEASE);
  while (slot->release.load(MemoryOrder::ACQUIRE) == 0)
    test_support::sleep_ms(0);
  return nullptr;
}

// Heap-allocate the slot/Thread arrays so we don't blow the test
// stack at high thread counts. Uses NtAllocateVirtualMemory directly
// — no dependence on operator new being routed through libc's heap,
// which we don't want to perturb during a registry-focused test.
template <typename T> static T *vm_alloc_array(size_t n) {
  size_t bytes = sizeof(T) * n;
  void *addr = nullptr;
  SIZE_T region = bytes;
  NTSTATUS s = ::NtAllocateVirtualMemory(NtCurrentProcess(), &addr, 0, &region,
                                         MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
  if (!NT_SUCCESS(s))
    return nullptr;
  // Placement-new every element with default initializer.
  for (size_t i = 0; i < n; ++i)
    new (static_cast<void *>(reinterpret_cast<T *>(addr) + i)) T();
  return reinterpret_cast<T *>(addr);
}

template <typename T> static void vm_free_array(T *p, size_t n) {
  if (!p)
    return;
  for (size_t i = 0; i < n; ++i)
    p[i].~T();
  void *addr = p;
  SIZE_T region = 0;
  (void)::NtFreeVirtualMemory(NtCurrentProcess(), &addr, &region, MEM_RELEASE);
}

// =========================================================================
// 1. HashSplitUnderRegistration
//
// The linear-hash splitter triggers when `live_count > 4 * bucket_count`.
// Initial L=4 ⇒ 16 buckets ⇒ first split at 65 live threads. Each
// successful split increments S; one full round (S → 2^L) doubles
// L. To exercise the protocol thoroughly, we want to drive multiple
// splits — i.e., push live_count well past 64.
//
// Constraint: we don't want to leave hundreds of threads alive across
// the whole test, so the test caps at N_PEAK and verifies findability
// at every stage.
// =========================================================================

TEST(LlvmLibcThreadRegistryStress, HashSplitUnderRegistration) {
  // 256 threads pushes through several split events: 65 (first split
  // at L=4), 96 (after second), 128 (round complete, L=5), 224 (L=6
  // round triggered). Caps at 256 to keep total handle count modest.
  constexpr uint32_t N_PEAK = 256;
  uint32_t baseline = registry_live_count();

  ParkSlot *slots = vm_alloc_array<ParkSlot>(N_PEAK);
  Thread *ths = vm_alloc_array<Thread>(N_PEAK);
  ASSERT_NE(slots, static_cast<ParkSlot *>(nullptr));
  ASSERT_NE(ths, static_cast<Thread *>(nullptr));

  for (uint32_t i = 0; i < N_PEAK; ++i) {
    int rc = ths[i].run(park_worker, &slots[i]);
    ASSERT_EQ(rc, 0);

    while (slots[i].ready.load(MemoryOrder::ACQUIRE) == 0)
      test_support::sleep_ms(0);

    // After every register, every PRIOR thread must still resolve.
    // This is the live-set invariant the split protocol must preserve.
    // We sample every 16th add (full check is O(N^2) = 65k ops, fine
    // but unnecessary) plus around every load-factor boundary.
    bool full_check = (i + 1) % 16 == 0 || (i + 1) == 65 || (i + 1) == 97 ||
                      (i + 1) == 129 || (i + 1) == 225;
    if (full_check) {
      for (uint32_t j = 0; j <= i; ++j) {
        ThreadLifecycle *lc =
            registry_find_by_task_id(slots[j].task_id.load(MemoryOrder::ACQUIRE));
        ASSERT_NE(lc, static_cast<ThreadLifecycle *>(nullptr));
        ASSERT_EQ(lc->task_id, slots[j].task_id.load(MemoryOrder::ACQUIRE));
        ASSERT_EQ(lc->tid, slots[j].tid.load(MemoryOrder::ACQUIRE));
      }
    }
  }

  // Final exhaustive sweep at peak. live_count includes our threads
  // plus any background threads.
  EXPECT_GE(registry_live_count(), baseline + N_PEAK);
  for (uint32_t j = 0; j < N_PEAK; ++j) {
    ThreadLifecycle *lc = registry_find_by_task_id(
        slots[j].task_id.load(MemoryOrder::ACQUIRE));
    ASSERT_NE(lc, static_cast<ThreadLifecycle *>(nullptr));
  }

  // Release & join in reverse order to also exercise unlink-from-tail.
  for (uint32_t i = N_PEAK; i-- > 0;) {
    slots[i].release.store(1, MemoryOrder::RELEASE);
  }
  for (uint32_t i = N_PEAK; i-- > 0;) {
    ASSERT_EQ(ths[i].join(static_cast<void **>(nullptr)), 0);
  }

  // Drift back to baseline. live_count is decremented in deregister
  // BEFORE Crystalline retire, so this should be tight.
  EXPECT_EQ(registry_live_count(), baseline);

  vm_free_array(ths, N_PEAK);
  vm_free_array(slots, N_PEAK);
}

// =========================================================================
// 2. HashLookupUnderConcurrentSplit
//
// While split-triggering churn is happening, a fleet of lookup workers
// keeps querying registry_find_by_task_id on a roster of known-live
// targets. Lookups must NEVER misresolve.
//
// "Misresolve" means: returns a non-null lifecycle whose task_id /= the
// queried one. This is the failure mode of a botched split protocol —
// e.g., a lookup that visits both halves of a splitting bucket but
// gets confused about which entry corresponds to its query.
// =========================================================================

struct LookupCtx {
  ParkSlot *roster;
  uint32_t roster_size;
  Atomic<bool> stop{false};
  Atomic<uint64_t> lookups{0};
  Atomic<uint64_t> misses{0};
  Atomic<uint64_t> misresolves{0};
};

static void *lookup_worker(void *arg) {
  auto *ctx = static_cast<LookupCtx *>(arg);
  uint64_t local_lookups = 0;
  uint64_t local_misses = 0;
  uint64_t local_misresolves = 0;
  // Cheap LCG for index churn — avoids any libc rand dependency
  // and keeps the inner loop tight.
  uint32_t state = static_cast<uint32_t>(NtCurrentThreadId() | 1u);
  while (!ctx->stop.load(MemoryOrder::ACQUIRE)) {
    state = state * 1664525u + 1013904223u;
    uint32_t i = state % ctx->roster_size;
    uint32_t want_task_id =
        ctx->roster[i].task_id.load(MemoryOrder::ACQUIRE);
    if (want_task_id == 0) {
      // Slot not populated yet.
      continue;
    }
    ThreadLifecycle *lc = registry_find_by_task_id(want_task_id);
    ++local_lookups;
    if (!lc) {
      ++local_misses;
    } else if (lc->task_id != want_task_id) {
      ++local_misresolves;
    }
  }
  ctx->lookups.fetch_add(local_lookups, MemoryOrder::RELAXED);
  ctx->misses.fetch_add(local_misses, MemoryOrder::RELAXED);
  ctx->misresolves.fetch_add(local_misresolves, MemoryOrder::RELAXED);
  return nullptr;
}

TEST(LlvmLibcThreadRegistryStress, HashLookupUnderConcurrentSplit) {
  constexpr uint32_t ROSTER = 96;       // pushes past first split (>64).
  constexpr uint32_t LOOKUPERS = 4;
  uint32_t baseline = registry_live_count();

  ParkSlot *roster = vm_alloc_array<ParkSlot>(ROSTER);
  Thread *roster_th = vm_alloc_array<Thread>(ROSTER);
  ASSERT_NE(roster, static_cast<ParkSlot *>(nullptr));
  ASSERT_NE(roster_th, static_cast<Thread *>(nullptr));

  // Bring up the roster.
  for (uint32_t i = 0; i < ROSTER; ++i) {
    ASSERT_EQ(roster_th[i].run(park_worker, &roster[i]), 0);
    while (roster[i].ready.load(MemoryOrder::ACQUIRE) == 0)
      test_support::sleep_ms(0);
  }

  // Spin up lookup workers. They run alongside churn below.
  LookupCtx lctx{roster, ROSTER, {}, {}, {}, {}};
  ParkSlot lookup_slots[LOOKUPERS]; // lookup workers self-register
                                     // through Thread::run; they're
                                     // registry-visible like any other
                                     // libc thread.
  Thread lookup_ths[LOOKUPERS];
  for (uint32_t i = 0; i < LOOKUPERS; ++i) {
    ASSERT_EQ(lookup_ths[i].run(lookup_worker, &lctx), 0);
  }

  // Drive churn: register and deregister a transient batch on top of
  // the roster, repeatedly. Each transient cycle adds and removes
  // entries, exercising delete-from-bucket + insert-into-bucket
  // racing with lookups.
  constexpr uint32_t CHURN_BATCHES = 8;
  constexpr uint32_t TRANSIENT = 32;
  for (uint32_t b = 0; b < CHURN_BATCHES; ++b) {
    ParkSlot transient[TRANSIENT];
    Thread tths[TRANSIENT];
    for (uint32_t i = 0; i < TRANSIENT; ++i) {
      ASSERT_EQ(tths[i].run(park_worker, &transient[i]), 0);
    }
    for (uint32_t i = 0; i < TRANSIENT; ++i) {
      while (transient[i].ready.load(MemoryOrder::ACQUIRE) == 0)
        test_support::sleep_ms(0);
    }
    for (uint32_t i = 0; i < TRANSIENT; ++i) {
      transient[i].release.store(1, MemoryOrder::RELEASE);
    }
    for (uint32_t i = 0; i < TRANSIENT; ++i) {
      ASSERT_EQ(tths[i].join(static_cast<void **>(nullptr)), 0);
    }
  }

  // Stop lookup workers.
  lctx.stop.store(true, MemoryOrder::RELEASE);
  for (uint32_t i = 0; i < LOOKUPERS; ++i)
    ASSERT_EQ(lookup_ths[i].join(static_cast<void **>(nullptr)), 0);

  // Lookups must have happened. Misses are acceptable (transient
  // dereg between query and lookup). Misresolves are NOT — the
  // structural claim is registry never returns the wrong lifecycle.
  EXPECT_GT(lctx.lookups.load(MemoryOrder::ACQUIRE), 0u);
  EXPECT_EQ(lctx.misresolves.load(MemoryOrder::ACQUIRE), 0u);

  // Roster is still alive — a roster query must succeed.
  for (uint32_t i = 0; i < ROSTER; ++i) {
    uint32_t want = roster[i].task_id.load(MemoryOrder::ACQUIRE);
    ThreadLifecycle *lc = registry_find_by_task_id(want);
    ASSERT_NE(lc, static_cast<ThreadLifecycle *>(nullptr));
    ASSERT_EQ(lc->task_id, want);
  }

  // Tear down roster.
  for (uint32_t i = 0; i < ROSTER; ++i)
    roster[i].release.store(1, MemoryOrder::RELEASE);
  for (uint32_t i = 0; i < ROSTER; ++i)
    ASSERT_EQ(roster_th[i].join(static_cast<void **>(nullptr)), 0);

  EXPECT_EQ(registry_live_count(), baseline);

  vm_free_array(roster_th, ROSTER);
  vm_free_array(roster, ROSTER);
}

// =========================================================================
// 3. IterateAllUnderChurn
//
// One iterator thread takes snapshots in a tight loop; other threads
// register/deregister continuously. Per-snapshot invariants:
//   * No duplicate task_ids (Harris-list semantics — each entry is
//     visited at most once, even if a concurrent split or unlink is
//     splicing it).
//   * Every visited lifecycle has a non-zero task_id.
//
// Cumulative invariant: after churn settles, live_count returns to
// baseline (no orphan iter-list entry).
// =========================================================================

struct IterCtx {
  Atomic<bool> stop{false};
  Atomic<uint64_t> snapshots{0};
  Atomic<uint64_t> visited_total{0};
  Atomic<uint64_t> dup_or_zero{0};
};

static void *iter_worker(void *arg) {
  auto *ctx = static_cast<IterCtx *>(arg);
  // Local task_id buffer for duplicate detection per-snapshot.
  // 256 is well above any reasonable peak in this test.
  constexpr uint32_t MAX_SNAPSHOT = 256;
  uint32_t seen[MAX_SNAPSHOT];
  while (!ctx->stop.load(MemoryOrder::ACQUIRE)) {
    uint32_t n = 0;
    bool bad = false;
    registry_for_each([&](ThreadLifecycle *lc) {
      if (lc->task_id == 0) {
        bad = true;
        return false;
      }
      if (n >= MAX_SNAPSHOT)
        return true; // bound — terminate walk.
      for (uint32_t k = 0; k < n; ++k) {
        if (seen[k] == lc->task_id) {
          bad = true;
          return false;
        }
      }
      seen[n++] = lc->task_id;
      return false;
    });
    ctx->snapshots.fetch_add(1, MemoryOrder::RELAXED);
    ctx->visited_total.fetch_add(n, MemoryOrder::RELAXED);
    if (bad)
      ctx->dup_or_zero.fetch_add(1, MemoryOrder::RELAXED);
  }
  return nullptr;
}

TEST(LlvmLibcThreadRegistryStress, IterateAllUnderChurn) {
  uint32_t baseline = registry_live_count();

  IterCtx ictx;
  Thread iter_th;
  ASSERT_EQ(iter_th.run(iter_worker, &ictx), 0);

  // Continuous churn: short bursts of create+join.
  constexpr uint32_t BATCHES = 16;
  constexpr uint32_t PER_BATCH = 16;
  for (uint32_t b = 0; b < BATCHES; ++b) {
    ParkSlot batch[PER_BATCH];
    Thread bths[PER_BATCH];
    for (uint32_t i = 0; i < PER_BATCH; ++i)
      ASSERT_EQ(bths[i].run(park_worker, &batch[i]), 0);
    for (uint32_t i = 0; i < PER_BATCH; ++i)
      while (batch[i].ready.load(MemoryOrder::ACQUIRE) == 0)
        test_support::sleep_ms(0);
    for (uint32_t i = 0; i < PER_BATCH; ++i)
      batch[i].release.store(1, MemoryOrder::RELEASE);
    for (uint32_t i = 0; i < PER_BATCH; ++i)
      ASSERT_EQ(bths[i].join(static_cast<void **>(nullptr)), 0);
  }

  ictx.stop.store(true, MemoryOrder::RELEASE);
  ASSERT_EQ(iter_th.join(static_cast<void **>(nullptr)), 0);

  // Iterator MUST have run at least once and never observed
  // duplicate or zero task_ids.
  EXPECT_GT(ictx.snapshots.load(MemoryOrder::ACQUIRE), 0u);
  EXPECT_EQ(ictx.dup_or_zero.load(MemoryOrder::ACQUIRE), 0u);

  // Settling: live_count returns to baseline.
  EXPECT_EQ(registry_live_count(), baseline);
}

// =========================================================================
// 4. CreateExitChurnNoLeak
//
// Long-running create+join in a tight loop. live_count must equal
// baseline both before and after, and no observable counting drift
// between iterations. This stresses Crystalline retire batching, the
// SlabPool .CRT$XLC abandon callback, and the per-thread bucket-entry
// pool's tls_alloc/free path.
// =========================================================================

TEST(LlvmLibcThreadRegistryStress, CreateExitChurnNoLeak) {
  constexpr uint32_t ITERS = 2048;
  uint32_t baseline = registry_live_count();

  uint32_t prev_task_id = 0;
  for (uint32_t i = 0; i < ITERS; ++i) {
    ParkSlot slot;
    Thread th;
    ASSERT_EQ(th.run(park_worker, &slot), 0);
    while (slot.ready.load(MemoryOrder::ACQUIRE) == 0)
      test_support::sleep_ms(0);

    uint32_t got = slot.task_id.load(MemoryOrder::ACQUIRE);
    ASSERT_GT(got, prev_task_id);
    prev_task_id = got;

    slot.release.store(1, MemoryOrder::RELEASE);
    ASSERT_EQ(th.join(static_cast<void **>(nullptr)), 0);

    // Every 64 iterations check baseline. We don't check every
    // iteration to keep the test fast; the goal is steady-state, not
    // strict pairing on each cycle (Crystalline batches retires).
    if ((i + 1) % 64 == 0) {
      ASSERT_EQ(registry_live_count(), baseline);
    }
  }
  EXPECT_EQ(registry_live_count(), baseline);
}

// =========================================================================
// 5. AlertAllWhileRegistering
//
// registry_alert_all() walks the iter list with a fixed-size stack
// buffer and flushes batches to NtAlertMultipleThreadByThreadId mid-
// walk. While threads are registering/deregistering,
// alert_all must never crash, never alert with a stale tid that maps
// to a since-recycled-and-unrelated thread (this is the primary
// motivation for the rewrite).
//
// We verify by counting: alert_all returns without trapping; the
// roster threads observe at least one wake (kernel alerts deliver
// via NtAlertMultipleThreadByThreadId / NtAlertThreadByThreadId).
// =========================================================================

struct AlertCtx {
  Atomic<uint32_t> ready{0};
  Atomic<uint32_t> woke_count{0};
  Atomic<bool> done{false};
};

static void *alertable_park(void *arg) {
  auto *ctx = static_cast<AlertCtx *>(arg);
  ctx->ready.store(1, MemoryOrder::RELEASE);
  while (!ctx->done.load(MemoryOrder::ACQUIRE)) {
    // Alertable wait: wakes on any kernel alert, including
    // NtAlertThreadByThreadId. We use a short relative sleep so the
    // loop polls `done` even without alerts.
    test_support::alertable_sleep_ms(5);
    ctx->woke_count.fetch_add(1, MemoryOrder::RELAXED);
  }
  return nullptr;
}

TEST(LlvmLibcThreadRegistryStress, AlertAllWhileRegistering) {
  constexpr uint32_t ALERTABLE = 8;
  AlertCtx ctxs[ALERTABLE];
  Thread alertable_ths[ALERTABLE];

  for (uint32_t i = 0; i < ALERTABLE; ++i) {
    ASSERT_EQ(alertable_ths[i].run(alertable_park, &ctxs[i]), 0);
  }
  for (uint32_t i = 0; i < ALERTABLE; ++i) {
    while (ctxs[i].ready.load(MemoryOrder::ACQUIRE) == 0)
      test_support::sleep_ms(0);
  }

  // Alert several times while a churn batch is going. Each alert
  // wakes every alertable_park worker (and may wake unrelated
  // background-libc threads, which is fine — the alert API is
  // best-effort and the test assertion is "no crash, some wakes").
  constexpr uint32_t ROUNDS = 16;
  constexpr uint32_t TRANSIENT = 8;
  for (uint32_t r = 0; r < ROUNDS; ++r) {
    ParkSlot transient[TRANSIENT];
    Thread tths[TRANSIENT];
    for (uint32_t i = 0; i < TRANSIENT; ++i)
      ASSERT_EQ(tths[i].run(park_worker, &transient[i]), 0);

    registry_alert_all(NtCurrentThreadId());

    for (uint32_t i = 0; i < TRANSIENT; ++i)
      while (transient[i].ready.load(MemoryOrder::ACQUIRE) == 0)
        test_support::sleep_ms(0);
    for (uint32_t i = 0; i < TRANSIENT; ++i)
      transient[i].release.store(1, MemoryOrder::RELEASE);
    for (uint32_t i = 0; i < TRANSIENT; ++i)
      ASSERT_EQ(tths[i].join(static_cast<void **>(nullptr)), 0);
  }

  // Finish alertable workers.
  for (uint32_t i = 0; i < ALERTABLE; ++i)
    ctxs[i].done.store(true, MemoryOrder::RELEASE);
  for (uint32_t i = 0; i < ALERTABLE; ++i)
    ASSERT_EQ(alertable_ths[i].join(static_cast<void **>(nullptr)), 0);

  // At least one wake observed across all alertable workers — the
  // 5ms periodic check would also satisfy this even without alerts,
  // so the assertion is loose. The strong check is "no crash" —
  // surviving the loop without an exception or trap is the primary
  // result.
  uint32_t total_wakes = 0;
  for (uint32_t i = 0; i < ALERTABLE; ++i)
    total_wakes += ctxs[i].woke_count.load(MemoryOrder::ACQUIRE);
  EXPECT_GT(total_wakes, 0u);
}

} // namespace
} // namespace LIBC_NAMESPACE_DECL
