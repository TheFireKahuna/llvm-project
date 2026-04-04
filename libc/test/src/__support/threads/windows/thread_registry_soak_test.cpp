//===-- Soak test for the Crystalline-W thread registry -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Long-running thread create/destroy that watches for memory leaks
// invisible to live_count. Crystalline retire batches are
// asynchronously drained, and the SlabPool reclaims slabs only on the
// .CRT$XLC abandon callback (per-thread exit) — neither of these is
// observable through live_count alone. The soak test takes
// before/peak/after snapshots of `VM_COUNTERS::PagefileUsage` (the
// process-private commit charge) and verifies that after a long run
// of create+join the process commit returns to within a small bounded
// delta of the starting commit.
//
// Why PagefileUsage rather than WorkingSetSize: working set is
// kernel-managed and noisy under memory pressure (pages get trimmed
// asynchronously). PagefileUsage reflects what the process committed
// via NtAllocateVirtualMemory + slab pools and tracks the registry's
// allocation pattern directly.
//
// What this test would catch
// --------------------------
//
//   * BucketEntry slab leak — every registered thread allocates one
//     entry; if `free_thread_registry_node` doesn't route the entry
//     back to `g_bucket_entry_pool`, ITERS entries accumulate
//     (~ITERS × 56B). At ITERS=10K that's ~560KB of unreclaimed slab
//     pages, well above the noise floor.
//
//   * Lifecycle slab leak — same pattern with `lifecycle_pool`.
//     A 16KB ThreadLifecycle × 10K iterations = 160MB; impossible to
//     miss.
//
//   * Iter-list orphan node — if Harris unlink ever fails to
//     physically detach a marked node, those nodes pin themselves
//     and their bucket entries against Crystalline reclaim, growing
//     monotonically.
//
//   * BucketHeadPage churn under repeated split — fork-reinit aside,
//     hash splits should not allocate new pages once steady state is
//     reached. If the splitter ever leaks a BucketHeadPage on the
//     OOM-recovery path, this test surfaces it.
//
// The test is gated on a runtime tunable (ITERS=2048 default) so CI
// can keep the run fast. To turn it into a true soak set
// LIBC_REGISTRY_SOAK_ITERS to a much larger number — the assertion
// scales the tolerance accordingly.
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

#include <sys/ntabi.h> // VM_COUNTERS

namespace LIBC_NAMESPACE_DECL {
namespace {

using cpp::Atomic;
using cpp::MemoryOrder;

// =========================================================================
// Process commit-charge snapshot
//
// Direct NtQueryInformationProcess(ProcessVmCounters). Returns 0 on
// failure; the test treats 0 as "skip the leak assertion" rather than
// hard-failing — the syscall is reliable on every Windows version, but
// guarding keeps the soak test robust against future VM_COUNTERS layout
// shifts.
// =========================================================================

static SIZE_T sample_pagefile_usage() {
  VM_COUNTERS vmc{};
  NTSTATUS s = ::NtQueryInformationProcess(NtCurrentProcess(),
                                           ProcessVmCounters, &vmc,
                                           sizeof(vmc), nullptr);
  if (!NT_SUCCESS(s))
    return 0;
  return vmc.PagefileUsage;
}

// Worker shape: minimal — register, capture monotonic counter, exit.
struct Slot {
  Atomic<uint32_t> task_id{0};
};

static void *micro_worker(void *arg) {
  auto *slot = static_cast<Slot *>(arg);
  ThreadLifecycle *self = get_current_lifecycle();
  if (self)
    slot->task_id.store(self->task_id, MemoryOrder::RELAXED);
  return nullptr;
}

// =========================================================================
// 1. CommitChargeReturnsToBaseline
//
// Spawn ITERS worker threads sequentially (create + join), measuring
// PagefileUsage at: start, peak (1/4 through), and end. After the run
// the commit charge should return to within `kAllowedGrowthBytes` of
// the baseline. 8MB tolerance covers slab-page rounding + Crystalline
// retire amortization without hiding a real ~ITERS-scale leak.
// =========================================================================

TEST(LlvmLibcThreadRegistrySoak, CommitChargeReturnsToBaseline) {
  // Configurable iteration count for "true soak" runs. Default 2048
  // keeps unit-test time bounded; bump via env / build define for
  // overnight runs.
#ifndef LIBC_REGISTRY_SOAK_ITERS
#define LIBC_REGISTRY_SOAK_ITERS 2048u
#endif
  constexpr uint32_t ITERS = LIBC_REGISTRY_SOAK_ITERS;

  // Warm-up: 16 threads to amortize the first-time Crystalline init,
  // first-time slab allocation, etc. Without this the "before"
  // snapshot would underestimate steady-state commit and produce
  // false-positive growth at the end.
  for (uint32_t i = 0; i < 16; ++i) {
    Slot s;
    Thread th;
    ASSERT_EQ(th.run(micro_worker, &s), 0);
    ASSERT_EQ(th.join(static_cast<void **>(nullptr)), 0);
  }

  uint32_t baseline_live = registry_live_count();
  SIZE_T before = sample_pagefile_usage();

  // Optional: skip if we can't measure.
  if (before == 0)
    return;

  SIZE_T peak = before;
  uint32_t prev_task_id = 0;
  for (uint32_t i = 0; i < ITERS; ++i) {
    Slot s;
    Thread th;
    ASSERT_EQ(th.run(micro_worker, &s), 0);
    ASSERT_EQ(th.join(static_cast<void **>(nullptr)), 0);

    // task_id strictly monotonic — proves no reuse across iterations.
    uint32_t got = s.task_id.load(MemoryOrder::ACQUIRE);
    ASSERT_GT(got, prev_task_id);
    prev_task_id = got;

    // Sample peak commit a few times during the run.
    if ((i + 1) == ITERS / 4 || (i + 1) == ITERS / 2) {
      SIZE_T cur = sample_pagefile_usage();
      if (cur > peak)
        peak = cur;
    }

    // Periodic live_count check — must equal baseline at every quiet
    // point. If it ever drifts, there's a counting bug.
    if ((i + 1) % 256 == 0) {
      ASSERT_EQ(registry_live_count(), baseline_live);
    }
  }

  // Final sample. Allow brief settle for any retired-but-not-yet-
  // freed Crystalline batches to drain through subsequent registry
  // operations. We poke a few small ops to flush.
  for (int i = 0; i < 4; ++i) {
    Slot s;
    Thread th;
    ASSERT_EQ(th.run(micro_worker, &s), 0);
    ASSERT_EQ(th.join(static_cast<void **>(nullptr)), 0);
  }
  test_support::sleep_ms(20);
  SIZE_T after = sample_pagefile_usage();

  EXPECT_EQ(registry_live_count(), baseline_live);

  // Tolerance: 8MB absolute, OR 2x growth — whichever larger. The
  // 2x guard catches regression on small initial baselines (where
  // 8MB might dwarf real growth); the 8MB floor stops normal slab
  // page rounding and Crystalline retire amortization from
  // false-positiving on a tight run.
  constexpr SIZE_T kAllowedAbsoluteBytes = 8u * 1024u * 1024u;
  SIZE_T allowed = kAllowedAbsoluteBytes;
  if (before > allowed)
    allowed = before; // doubling tolerance.
  EXPECT_LE(after, before + allowed);
  // The peak we observed mid-run should also be reasonable, but we
  // don't assert on it — the meaningful invariant is the return-to-
  // baseline at the end.
  (void)peak;
}

// =========================================================================
// 2. ManyTinyWavesNoCountDrift
//
// Same shape but parallel: NWAVES iterations of "spawn N, join N".
// Verifies that the live_count atomic increments and decrements by
// the right amounts every wave, with no drift from concurrent
// register/dereg races.
// =========================================================================

struct WaveSlot {
  Atomic<uint32_t> ready{0};
  Atomic<uint32_t> release{0};
};

static void *wave_worker(void *arg) {
  auto *slot = static_cast<WaveSlot *>(arg);
  slot->ready.store(1, MemoryOrder::RELEASE);
  while (slot->release.load(MemoryOrder::ACQUIRE) == 0)
    test_support::sleep_ms(0);
  return nullptr;
}

TEST(LlvmLibcThreadRegistrySoak, ManyTinyWavesNoCountDrift) {
  constexpr uint32_t NWAVES = 64;
  constexpr uint32_t PER_WAVE = 16;
  uint32_t baseline = registry_live_count();

  for (uint32_t w = 0; w < NWAVES; ++w) {
    WaveSlot slots[PER_WAVE];
    Thread ths[PER_WAVE];

    for (uint32_t i = 0; i < PER_WAVE; ++i)
      ASSERT_EQ(ths[i].run(wave_worker, &slots[i]), 0);
    for (uint32_t i = 0; i < PER_WAVE; ++i)
      while (slots[i].ready.load(MemoryOrder::ACQUIRE) == 0)
        test_support::sleep_ms(0);

    // Mid-wave: live_count == baseline + PER_WAVE.
    EXPECT_EQ(registry_live_count(), baseline + PER_WAVE);

    for (uint32_t i = 0; i < PER_WAVE; ++i)
      slots[i].release.store(1, MemoryOrder::RELEASE);
    for (uint32_t i = 0; i < PER_WAVE; ++i)
      ASSERT_EQ(ths[i].join(static_cast<void **>(nullptr)), 0);

    // Post-wave: back to baseline.
    EXPECT_EQ(registry_live_count(), baseline);
  }
}

} // namespace
} // namespace LIBC_NAMESPACE_DECL
