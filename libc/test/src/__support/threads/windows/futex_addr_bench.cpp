//===-- futex_addr benchmark: parking lot vs WaitOnAddress ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Sibling benchmark to futex_bench. Same harness (common header), same
// test matrix where it applies — but the primitive under test is the
// address-keyed parking lot `futex_addr::wait/wake` rather than the
// embedded-Treiber-stack Futex.
//
// Why a separate bench: the two primitives serve different callers and
// different correctness contracts:
//
//   - Futex (futex_utils.h): state and waiter stack live inside one
//     cpp::Atomic<uint32_t> word. O(1) wake, no hashing, no bucket
//     lock. Used by RawMutex and internal thread primitives.
//
//   - futex_addr (futex_addr.h): state lives in a shared 256-bucket
//     parking lot keyed by address hash. Waiter SLL per bucket,
//     bucket-level spinlock for insert/scan. Used anywhere we need
//     to park on an arbitrary memory address (pthread condvars,
//     atomic_wait, user primitives).
//
// The point of this file is to measure what the parking lot costs
// relative to its upper and lower bounds:
//   - Upper bound: WaitOnAddress — Windows' own parking lot.
//   - Lower bound: Futex — no hashing, no bucket lock, no SLL scan.
//     Including it as a reference row makes visible the cost of the
//     extra indirection (hash + bucket acquire + DLL/SLL walk).
//
// Test matrix mirrors futex_bench (letters kept identical for direct
// cross-reference):
//   A. Ping-pong latency (1 pair, 4 pairs contended)
//   B. Uncontended wake (live_count fast path)
//   C. Timeout return (wait on value with 1ns timeout)
//   E. Fan-out wake (wake all N parked waiters)
//   F. Mutex-style acquire/release
//   G. High thread count (32, 64 waiters)
//   H. Race-exposure stress
//   K. Variable critical-section mutex
//   L. Oversubscription mutex
//   M. Independent futexes across addresses — the natural win case
//      for a hashed parking lot.
//
// Plus two futex_addr-specific tests that have no Futex analogue:
//   N. Bucket-collision stress. 8 addresses deliberately hashed to
//      the same 256-bucket slot. Serialises on one bucket lock.
//   O. Bucket-dispersion scaling. 8 addresses deliberately hashed to
//      distinct buckets. Bucket locks are independent — expected
//      near-linear speedup over N.
//
// N and O share the same per-pair workload; their ratio is the
// parking lot's hash-dispersion efficiency under contention.
//
// D is dropped: Futex exposes fetch_add on the embedded word, but
// futex_addr is a wait/wake primitive over an external atomic — the
// counter benchmark has nothing to say here.
//
// Hosted build — same link rules as futex_bench.
//
//===----------------------------------------------------------------------===//

#include "futex_bench_common.h"

#include "src/__support/threads/windows/futex_addr.h"

namespace LIBC_NAMESPACE_DECL {

// ============================================================
// Bench A.1: WaitOnAddress ping-pong (reference)
// ============================================================

struct WoaPP {
  cpp::Atomic<uint32_t> val{0};
  cpp::Atomic<bool> ready{false};
};

LIBC_INLINE void woa_pp_roundtrip(WoaPP &pp) {
  pp.val.store(1, cpp::MemoryOrder::RELEASE);
  ::RtlWakeAddressSingle(&pp.val.val);
  for (;;) {
    uint32_t v = pp.val.load(cpp::MemoryOrder::ACQUIRE);
    if (v == 0)
      break;
    uint32_t expected = 1;
    ::RtlWaitOnAddress(&pp.val.val, &expected, sizeof(expected), nullptr);
  }
}

static void woa_responder(void *arg) {
  auto *pp = static_cast<WoaPP *>(arg);
  pp->ready.store(true, cpp::MemoryOrder::RELEASE);
  for (int i = 0; i < PING_TOTAL; ++i) {
    for (;;) {
      uint32_t v = pp->val.load(cpp::MemoryOrder::ACQUIRE);
      if (v == 1)
        break;
      uint32_t expected = 0;
      ::RtlWaitOnAddress(&pp->val.val, &expected, sizeof(expected), nullptr);
    }
    pp->val.store(0, cpp::MemoryOrder::RELEASE);
    ::RtlWakeAddressSingle(&pp->val.val);
  }
}

static void bench_woa(int64_t *timings) {
  reset_lp();
  WoaPP pp;
  ThreadArg ta;
  pthread_t h = start_thread(woa_responder, &pp, ta);
  while (!pp.ready.load(cpp::MemoryOrder::ACQUIRE))
    ;

  for (int i = 0; i < PING_WARMUP_ITERS; ++i)
    woa_pp_roundtrip(pp);

  for (int s = 0; s < PING_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < PING_BATCH; ++k)
      woa_pp_roundtrip(pp);
    timings[s] = (now_ns() - t0) / PING_BATCH;
  }
  join_thread(h);
}

// ============================================================
// Bench A.2: futex_addr ping-pong
// ============================================================
//
// Semantically identical to the WoA variant — only the parking
// primitive differs. Caller-side sequence is the same two-step
// "write value, wake address" / "load-check, wait-on-mismatch"
// pattern since futex_addr, like RtlWaitOnAddress, keys on the
// external atomic's address.

struct FaPP {
  cpp::Atomic<uint32_t> val{0};
  cpp::Atomic<bool> ready{false};
};

LIBC_INLINE void fa_pp_roundtrip(FaPP &pp) {
  // Caller emits the RELEASE store; futex_addr::wake does its own
  // SEQ_CST fence before reading live_count, so the waker side is
  // already Dekker-safe. No need for store_and_notify fusion (that
  // exists on Futex because Futex's wake path loads waiter state
  // without an external fence).
  pp.val.store(1, cpp::MemoryOrder::RELEASE);
  futex_addr::wake(&pp.val, 1);
  while (pp.val.load(cpp::MemoryOrder::ACQUIRE) != 0)
    futex_addr::wait(&pp.val, 1u, nullptr);
}

static void fa_responder(void *arg) {
  auto *pp = static_cast<FaPP *>(arg);
  pp->ready.store(true, cpp::MemoryOrder::RELEASE);
  for (int i = 0; i < PING_TOTAL; ++i) {
    while (pp->val.load(cpp::MemoryOrder::ACQUIRE) != 1)
      futex_addr::wait(&pp->val, 0u, nullptr);
    pp->val.store(0, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&pp->val, 1);
  }
}

static void bench_fa(int64_t *timings) {
  reset_lp();
  FaPP pp;
  ThreadArg ta;
  pthread_t h = start_thread(fa_responder, &pp, ta);
  while (!pp.ready.load(cpp::MemoryOrder::ACQUIRE))
    ;

  for (int i = 0; i < PING_WARMUP_ITERS; ++i)
    fa_pp_roundtrip(pp);

  for (int s = 0; s < PING_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < PING_BATCH; ++k)
      fa_pp_roundtrip(pp);
    timings[s] = (now_ns() - t0) / PING_BATCH;
  }
  join_thread(h);
}

// ============================================================
// Bench A.3/4: 4-pair contended ping-pong (WoA / futex_addr)
// ============================================================

static constexpr int NUM_PAIRS = 4;

struct WoaContCtx {
  cpp::Atomic<uint32_t> val{0};
  cpp::Atomic<bool> ready{false};
};

static void woa_cont_responder(void *arg) {
  auto *ctx = static_cast<WoaContCtx *>(arg);
  ctx->ready.store(true, cpp::MemoryOrder::RELEASE);
  for (int i = 0; i < PING_TOTAL; ++i) {
    for (;;) {
      uint32_t v = ctx->val.load(cpp::MemoryOrder::ACQUIRE);
      if (v == 1)
        break;
      uint32_t expected = 0;
      ::RtlWaitOnAddress(&ctx->val.val, &expected, sizeof(expected), nullptr);
    }
    ctx->val.store(0, cpp::MemoryOrder::RELEASE);
    ::RtlWakeAddressSingle(&ctx->val.val);
  }
}

LIBC_INLINE void woa_cont_roundtrip(WoaContCtx *ctx) {
  for (int p = 0; p < NUM_PAIRS; ++p) {
    ctx[p].val.store(1, cpp::MemoryOrder::RELEASE);
    ::RtlWakeAddressSingle(&ctx[p].val.val);
  }
  for (int p = 0; p < NUM_PAIRS; ++p) {
    for (;;) {
      uint32_t v = ctx[p].val.load(cpp::MemoryOrder::ACQUIRE);
      if (v == 0)
        break;
      uint32_t expected = 1;
      ::RtlWaitOnAddress(&ctx[p].val.val, &expected, sizeof(expected), nullptr);
    }
  }
}

static void bench_woa_contended(int64_t *timings) {
  reset_lp();
  WoaContCtx ctx[NUM_PAIRS];
  ThreadArg tas[NUM_PAIRS];
  pthread_t handles[NUM_PAIRS];

  for (int p = 0; p < NUM_PAIRS; ++p)
    handles[p] = start_thread(woa_cont_responder, &ctx[p], tas[p]);
  for (int p = 0; p < NUM_PAIRS; ++p)
    while (!ctx[p].ready.load(cpp::MemoryOrder::ACQUIRE))
      ;

  for (int i = 0; i < PING_WARMUP_ITERS; ++i)
    woa_cont_roundtrip(ctx);
  for (int s = 0; s < PING_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < PING_BATCH; ++k)
      woa_cont_roundtrip(ctx);
    timings[s] = (now_ns() - t0) / PING_BATCH;
  }
  for (int p = 0; p < NUM_PAIRS; ++p)
    join_thread(handles[p]);
}

struct FaContCtx {
  cpp::Atomic<uint32_t> val{0};
  cpp::Atomic<bool> ready{false};
};

static void fa_cont_responder(void *arg) {
  auto *ctx = static_cast<FaContCtx *>(arg);
  ctx->ready.store(true, cpp::MemoryOrder::RELEASE);
  for (int i = 0; i < PING_TOTAL; ++i) {
    while (ctx->val.load(cpp::MemoryOrder::ACQUIRE) != 1)
      futex_addr::wait(&ctx->val, 0u, nullptr);
    ctx->val.store(0, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&ctx->val, 1);
  }
}

LIBC_INLINE void fa_cont_roundtrip(FaContCtx *ctx) {
  for (int p = 0; p < NUM_PAIRS; ++p) {
    ctx[p].val.store(1, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&ctx[p].val, 1);
  }
  for (int p = 0; p < NUM_PAIRS; ++p)
    while (ctx[p].val.load(cpp::MemoryOrder::ACQUIRE) != 0)
      futex_addr::wait(&ctx[p].val, 1u, nullptr);
}

static void bench_fa_contended(int64_t *timings) {
  reset_lp();
  FaContCtx ctx[NUM_PAIRS];
  ThreadArg tas[NUM_PAIRS];
  pthread_t handles[NUM_PAIRS];

  for (int p = 0; p < NUM_PAIRS; ++p)
    handles[p] = start_thread(fa_cont_responder, &ctx[p], tas[p]);
  for (int p = 0; p < NUM_PAIRS; ++p)
    while (!ctx[p].ready.load(cpp::MemoryOrder::ACQUIRE))
      ;

  for (int i = 0; i < PING_WARMUP_ITERS; ++i)
    fa_cont_roundtrip(ctx);
  for (int s = 0; s < PING_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < PING_BATCH; ++k)
      fa_cont_roundtrip(ctx);
    timings[s] = (now_ns() - t0) / PING_BATCH;
  }
  for (int p = 0; p < NUM_PAIRS; ++p)
    join_thread(handles[p]);
}

// ============================================================
// Bench B: Uncontended wake — no waiters (live_count / fast path)
// ============================================================
//
// futex_addr::wake's uncontended path is:
//   SEQ_CST fence → live_count load → early return on zero.
// Two atomic ops plus a fence; RtlWakeAddressSingle with no parked
// waiter is a kernel thunk that returns quickly but still crosses the
// user/kernel boundary. Batch 20k calls per sample so per-call cost
// stays above timer resolution.

static constexpr int EMPTY_WAKE_BATCH = 20000;

static void bench_wake_empty_fa(int64_t *timings) {
  cpp::Atomic<uint32_t> val{0};
  for (int i = 0; i < EMPTY_WAKE_BATCH; ++i)
    futex_addr::wake(&val, 1);
  for (int s = 0; s < PING_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < EMPTY_WAKE_BATCH; ++k)
      futex_addr::wake(&val, 1);
    timings[s] = (now_ns() - t0) * 1000 / EMPTY_WAKE_BATCH;
  }
}

static void bench_wake_empty_woa(int64_t *timings) {
  cpp::Atomic<uint32_t> val{0};
  for (int i = 0; i < EMPTY_WAKE_BATCH; ++i)
    ::RtlWakeAddressSingle(&val.val);
  for (int s = 0; s < PING_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < EMPTY_WAKE_BATCH; ++k)
      ::RtlWakeAddressSingle(&val.val);
    timings[s] = (now_ns() - t0) * 1000 / EMPTY_WAKE_BATCH;
  }
}

// ============================================================
// Bench C: Timeout return
// ============================================================
//
// futex_addr::wait with a 1ns timeout still performs the full setup:
// spin phase, slot allocation, bucket-lock insert + Dekker recheck,
// CAS WAITING→IN_KERNEL, NtWaitForAlertByThreadId (returns immediately
// with STATUS_TIMEOUT), lock-free CAS IN_KERNEL→TIMED_OUT, lazy SLL
// cleanup on next wait. This exercises the hot "short timeout" path
// — the same shape pthread_cond_timedwait hits on a very close deadline.
//
// The reference WoA variant shares futex_bench's observation: non-zero
// timeouts on RtlWaitOnAddress always round up to the ~15.6ms kernel
// timer tick, dominating the measurement. We keep the same 20-sample
// budget so reports are comparable, but the result is effectively the
// scheduler tick rather than a path cost.

static constexpr int TIMEOUT_BATCH = 20;
static constexpr int TIMEOUT_WOA_SAMPLES = 20;

static void bench_timeout_fa(int64_t *timings) {
  cpp::Atomic<uint32_t> val{42};
  struct timespec ts = {0, 100}; // 100ns — rounds to 1 hns tick, not zero
  // Warmup drains one-time costs: kuser spin threshold load, first-use
  // slot allocation, slab page-in, bucket zero-init.
  for (int i = 0; i < 200; ++i)
    futex_addr::wait(&val, 42u, &ts);
  for (int s = 0; s < HEAVY_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < TIMEOUT_BATCH; ++k)
      futex_addr::wait(&val, 42u, &ts);
    timings[s] = (now_ns() - t0) / TIMEOUT_BATCH;
  }
}

static void bench_timeout_woa(int64_t *timings) {
  cpp::Atomic<uint32_t> val{42};
  for (int i = -2; i < TIMEOUT_WOA_SAMPLES; ++i) {
    LARGE_INTEGER timeout;
    timeout.QuadPart = -1; // 100ns relative — effectively immediate
    uint32_t expected = 42;
    int64_t t0 = now_ns();
    ::RtlWaitOnAddress(&val.val, &expected, sizeof(expected), &timeout);
    int64_t t1 = now_ns();
    if (i >= 0)
      timings[i] = t1 - t0;
  }
}

// ============================================================
// Bench C.2: Early-return (EAGAIN) — value-mismatch fast path
// ============================================================
//
// Specific to wait-on-address primitives: the first thing wait_nt does
// is load the value and compare against expected. A mismatch returns
// -EAGAIN in a handful of instructions. This path is on the hot edge
// of every uncontended mutex acquire (atomic CAS succeeds → never
// actually calls wait), but it is also hit on the double-check *after*
// CAS loss — so the cost matters under contention. Batch 20k since
// the path is sub-100ns on modern CPUs.

static constexpr int EAGAIN_BATCH = 20000;

static void bench_eagain_fa(int64_t *timings) {
  cpp::Atomic<uint32_t> val{0};
  // Expected=1, value=0 — mismatch on first load, immediate -EAGAIN.
  for (int i = 0; i < EAGAIN_BATCH; ++i)
    futex_addr::wait(&val, 1u, nullptr);
  for (int s = 0; s < PING_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < EAGAIN_BATCH; ++k)
      futex_addr::wait(&val, 1u, nullptr);
    timings[s] = (now_ns() - t0) * 1000 / EAGAIN_BATCH;
  }
}

static void bench_eagain_woa(int64_t *timings) {
  cpp::Atomic<uint32_t> val{0};
  for (int i = 0; i < EAGAIN_BATCH; ++i) {
    uint32_t expected = 1;
    ::RtlWaitOnAddress(&val.val, &expected, sizeof(expected), nullptr);
  }
  for (int s = 0; s < PING_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < EAGAIN_BATCH; ++k) {
      uint32_t expected = 1;
      ::RtlWaitOnAddress(&val.val, &expected, sizeof(expected), nullptr);
    }
    timings[s] = (now_ns() - t0) * 1000 / EAGAIN_BATCH;
  }
}

// ============================================================
// Bench E: notify-all fan-out
// ============================================================
//
// Fan-out across N parked waiters. futex_addr::wake(count=UINT32_MAX)
// does a two-pass walk under bucket lock, CAS-claims each matching
// slot, then batch-alerts IN_KERNEL waiters outside the lock. Batch
// alert uses alert_multiple for count>1, single alert for count==1.
//
// Same fresh-thread-per-sample strategy as futex_bench: pool-based
// fan-out was unreliable at 16+ waiters. Thread creation happens
// pre-timing; only the wake→all-done window is measured.

struct FaFanOutCtx {
  cpp::Atomic<uint32_t> val{0};
  cpp::Atomic<int> arrived{0};
  cpp::Atomic<int> done{0};
};

static void fa_fanout_waiter(void *arg) {
  auto *ctx = static_cast<FaFanOutCtx *>(arg);
  ctx->arrived.fetch_add(1, cpp::MemoryOrder::RELEASE);
  while (ctx->val.load(cpp::MemoryOrder::ACQUIRE) == 0)
    futex_addr::wait(&ctx->val, 0u, nullptr);
  ctx->done.fetch_add(1, cpp::MemoryOrder::RELEASE);
}

static void bench_fanout_fa(int64_t *timings, int num_waiters,
                             int samples = HEAVY_SAMPLES) {
  for (int s = 0; s < samples; ++s) {
    reset_lp();
    FaFanOutCtx ctx;
    ctx.val.store(0, cpp::MemoryOrder::RELAXED);
    ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
    ctx.done.store(0, cpp::MemoryOrder::RELAXED);

    ThreadArg tas[64];
    pthread_t handles[64];
    for (int t = 0; t < num_waiters; ++t)
      handles[t] = start_thread(fa_fanout_waiter, &ctx, tas[t]);

    while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < num_waiters)
      ;
    quiet_sleep_ns(1'000'000); // 1ms settle so all waiters reach kernel

    int64_t t0 = now_ns();
    ctx.val.store(1, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&ctx.val, UINT32_MAX);
    while (ctx.done.load(cpp::MemoryOrder::ACQUIRE) < num_waiters)
      ;
    timings[s] = now_ns() - t0;

    for (int t = 0; t < num_waiters; ++t)
      join_thread(handles[t]);
  }
}

struct WoaFanOutCtx {
  cpp::Atomic<uint32_t> val{0};
  cpp::Atomic<int> arrived{0};
  cpp::Atomic<int> done{0};
};

// Fresh-thread-per-sample — mirrors bench_fanout_fa and matches
// futex_bench.cpp::bench_fanout_woa. A WorkerPool here still hangs at
// 16+ waiters: the pool's own round_seq is a Futex, so its per-round
// wait/notify_all oversubscribes the wait-slot pool the same way the
// Futex-side fanout does. The measured window is trigger-wake →
// all-done, which starts after arrival, so spawning fresh costs
// nothing measured.
static void woa_fanout_waiter(void *arg) {
  auto *ctx = static_cast<WoaFanOutCtx *>(arg);
  ctx->arrived.fetch_add(1, cpp::MemoryOrder::RELEASE);
  while (ctx->val.load(cpp::MemoryOrder::ACQUIRE) == 0) {
    uint32_t expected = 0;
    ::RtlWaitOnAddress(&ctx->val.val, &expected, sizeof(expected), nullptr);
  }
  ctx->done.fetch_add(1, cpp::MemoryOrder::RELEASE);
}

static void bench_fanout_woa(int64_t *timings, int num_waiters,
                              int samples = HEAVY_SAMPLES) {
  for (int s = 0; s < samples; ++s) {
    reset_lp();
    WoaFanOutCtx ctx;
    ctx.val.store(0, cpp::MemoryOrder::RELAXED);
    ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
    ctx.done.store(0, cpp::MemoryOrder::RELAXED);

    ThreadArg tas[64];
    pthread_t handles[64];
    for (int t = 0; t < num_waiters; ++t)
      handles[t] = start_thread(woa_fanout_waiter, &ctx, tas[t]);

    while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < num_waiters)
      ;
    quiet_sleep_ns(1'000'000); // 1ms settle so all waiters reach kernel

    int64_t t0 = now_ns();
    ctx.val.store(1, cpp::MemoryOrder::RELEASE);
    ::RtlWakeAddressAll(&ctx.val.val);
    while (ctx.done.load(cpp::MemoryOrder::ACQUIRE) < num_waiters)
      ;
    timings[s] = now_ns() - t0;

    for (int t = 0; t < num_waiters; ++t)
      join_thread(handles[t]);
  }
}

// ============================================================
// Bench F: Mutex-style acquire/release
// ============================================================
//
// Build a two-state mutex on top of futex_addr + an external atomic.
// This matches the "classic" futex mutex pattern used by glibc (minus
// the 3-state contention marker that RawMutex adds). The comparison
// we want is:
//   futex_addr mutex : parking lot serializes contenders on bucket lock
//   WoA mutex        : Windows parking lot
//   RawMutex         : Futex embedded-stack, no hashing (via RawMutex)

struct FaMutexShared {
  cpp::Atomic<uint32_t> lock{0}; // 0 = unlocked, 1 = locked
  volatile int64_t counter;
  int iters;
  int cs_ns = 0;
};

static void mutex_fa_round(void *arg, int /*idx*/) {
  auto *ctx = static_cast<FaMutexShared *>(arg);
  for (int i = 0; i < ctx->iters; ++i) {
    for (;;) {
      uint32_t expected = 0;
      if (ctx->lock.compare_exchange_weak(expected, 1u,
                                          cpp::MemoryOrder::ACQUIRE,
                                          cpp::MemoryOrder::RELAXED))
        break;
      futex_addr::wait(&ctx->lock, 1u, nullptr);
    }
    if (ctx->cs_ns > 0) {
      int64_t t0 = now_ns();
      while (now_ns() - t0 < ctx->cs_ns)
        ;
    }
    ctx->counter++;
    ctx->lock.store(0, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&ctx->lock, 1);
  }
}

static void bench_mutex_fa_params(int64_t *timings, int num_threads,
                                   int total_iters, int cs_ns, int samples) {
  int warmup = samples / 10;
  FaMutexShared ctx;
  ctx.iters = total_iters / num_threads;
  ctx.cs_ns = cs_ns;

  // fresh-thread-per-sample: the benched primitive (futex_addr::wait) shares
  // the wait-slot pool with any Futex::wait in the harness. pool_run_mutex_style
  // would race its own round_seq Futex against the bench and deadlock at 8T+.
  for (int s = 0; s < warmup + samples; ++s) {
    ctx.lock.store(0, cpp::MemoryOrder::RELAXED);
    ctx.counter = 0;
    int64_t t = fresh_run_mutex_style(num_threads, mutex_fa_round, &ctx);
    if (s >= warmup)
      timings[s - warmup] = t;
  }
}

static void bench_mutex_fa(int64_t *timings, int num_threads) {
  bench_mutex_fa_params(timings, num_threads, 100000, 0, HEAVY_SAMPLES);
}

struct WoaMutexShared {
  cpp::Atomic<uint32_t> lock{0};
  volatile int64_t counter;
  int iters;
  int cs_ns = 0;
};

static void mutex_woa_round(void *arg, int /*idx*/) {
  auto *ctx = static_cast<WoaMutexShared *>(arg);
  for (int i = 0; i < ctx->iters; ++i) {
    for (;;) {
      uint32_t expected = 0;
      if (ctx->lock.compare_exchange_weak(expected, 1u,
                                          cpp::MemoryOrder::ACQUIRE,
                                          cpp::MemoryOrder::RELAXED))
        break;
      uint32_t one = 1;
      ::RtlWaitOnAddress(&ctx->lock.val, &one, sizeof(one), nullptr);
    }
    if (ctx->cs_ns > 0) {
      int64_t t0 = now_ns();
      while (now_ns() - t0 < ctx->cs_ns)
        ;
    }
    ctx->counter++;
    ctx->lock.store(0, cpp::MemoryOrder::RELEASE);
    ::RtlWakeAddressSingle(&ctx->lock.val);
  }
}

static void bench_mutex_woa_params(int64_t *timings, int num_threads,
                                    int total_iters, int cs_ns, int samples) {
  int warmup = samples / 10;
  WoaMutexShared ctx;
  ctx.iters = total_iters / num_threads;
  ctx.cs_ns = cs_ns;

  // WoA's kernel parking is independent of our wait-slot pool, so pool-based
  // mutex harness would be technically safe. Using fresh here anyway keeps
  // every mutex row in this bench on identical timing scaffolding — makes
  // cross-variant comparisons apples-to-apples rather than pool-vs-fresh.
  for (int s = 0; s < warmup + samples; ++s) {
    ctx.lock.store(0, cpp::MemoryOrder::RELAXED);
    ctx.counter = 0;
    int64_t t = fresh_run_mutex_style(num_threads, mutex_woa_round, &ctx);
    if (s >= warmup)
      timings[s - warmup] = t;
  }
}

static void bench_mutex_woa(int64_t *timings, int num_threads) {
  bench_mutex_woa_params(timings, num_threads, 100000, 0, HEAVY_SAMPLES);
}

// RawMutex reference (Futex embedded stack under the hood). Timing
// shape must match the other two so the rows compare cleanly — i.e.
// same fresh-thread runner, same total_iters split, same cs_ns busy-wait.
struct RawMutexShared {
  RawMutex lock;
  volatile int64_t counter;
  int iters;
  int cs_ns = 0;
};

static void mutex_raw_round(void *arg, int /*idx*/) {
  auto *ctx = static_cast<RawMutexShared *>(arg);
  for (int i = 0; i < ctx->iters; ++i) {
    ctx->lock.lock();
    if (ctx->cs_ns > 0) {
      int64_t t0 = now_ns();
      while (now_ns() - t0 < ctx->cs_ns)
        ;
    }
    ctx->counter++;
    ctx->lock.unlock();
  }
}

static void bench_mutex_raw_params(int64_t *timings, int num_threads,
                                    int total_iters, int cs_ns, int samples) {
  int warmup = samples / 10;
  RawMutexShared ctx;
  ctx.iters = total_iters / num_threads;
  ctx.cs_ns = cs_ns;

  // RawMutex wraps Futex::wait, so pool_run_mutex_style would collide with
  // the bench on round_seq the same way futex_addr does.
  for (int s = 0; s < warmup + samples; ++s) {
    ctx.lock.reset();
    ctx.counter = 0;
    int64_t t = fresh_run_mutex_style(num_threads, mutex_raw_round, &ctx);
    if (s >= warmup)
      timings[s - warmup] = t;
  }
}

// ============================================================
// Bench H: race-exposure stress (8-pair contended ping-pong)
// ============================================================
//
// Mirrors futex_bench's stress path. More pairs + more iters exposes
// the WAITING ↔ IN_KERNEL state drift window where a late CAS might
// fall through (wake-misses would hang here). With the two-CAS retry
// in wake() (predict-then-fall-through), all legitimate drifts are
// claimed. Watchdog thread prints progress on stall.

static constexpr int STRESS_PAIRS = 8;
static constexpr int STRESS_ITERS = 25000;
static cpp::Atomic<int> stress_iter{0};
static FaContCtx *stress_ctx_ptr;

static void stress_responder(void *arg) {
  auto *ctx = static_cast<FaContCtx *>(arg);
  ctx->ready.store(true, cpp::MemoryOrder::RELEASE);
  for (int i = 0; i < STRESS_ITERS; ++i) {
    while (ctx->val.load(cpp::MemoryOrder::ACQUIRE) != 1)
      futex_addr::wait(&ctx->val, 0u, nullptr);
    ctx->val.store(0, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&ctx->val, 1);
  }
}

static void stress_watchdog(void *) {
  for (;;) {
    test_support::sleep_ms(3000);
    int iter = stress_iter.load(cpp::MemoryOrder::ACQUIRE);
    if (iter >= STRESS_ITERS)
      return;
    write_str("    WATCHDOG: stall at iter=");
    write_i64(iter);
    write_str("/");
    write_i64(STRESS_ITERS);
    write_str(" vals=[");
    for (int p = 0; p < STRESS_PAIRS; ++p) {
      if (p)
        write_str(",");
      write_i64(stress_ctx_ptr[p].val.load(cpp::MemoryOrder::RELAXED));
    }
    write_str("]\n");
  }
}

static bool stress_contended() {
  reset_lp();
  FaContCtx ctx[STRESS_PAIRS];
  ThreadArg tas[STRESS_PAIRS];
  pthread_t handles[STRESS_PAIRS];

  stress_ctx_ptr = ctx;
  stress_iter.store(0, cpp::MemoryOrder::RELAXED);

  ThreadArg wdta;
  pthread_t watchdog = start_thread(stress_watchdog, nullptr, wdta);

  for (int p = 0; p < STRESS_PAIRS; ++p)
    handles[p] = start_thread(stress_responder, &ctx[p], tas[p]);
  for (int p = 0; p < STRESS_PAIRS; ++p)
    while (!ctx[p].ready.load(cpp::MemoryOrder::ACQUIRE))
      ;

  int64_t t0 = now_ns();
  for (int i = 0; i < STRESS_ITERS; ++i) {
    stress_iter.store(i, cpp::MemoryOrder::RELEASE);
    for (int p = 0; p < STRESS_PAIRS; ++p) {
      ctx[p].val.store(1, cpp::MemoryOrder::RELEASE);
      futex_addr::wake(&ctx[p].val, 1);
    }
    for (int p = 0; p < STRESS_PAIRS; ++p)
      while (ctx[p].val.load(cpp::MemoryOrder::ACQUIRE) != 0)
        futex_addr::wait(&ctx[p].val, 1u, nullptr);
  }
  int64_t elapsed = now_ns() - t0;

  stress_iter.store(STRESS_ITERS, cpp::MemoryOrder::RELEASE);
  for (int p = 0; p < STRESS_PAIRS; ++p)
    join_thread(handles[p]);
  join_thread(watchdog);

  write_str("  ");
  write_i64(STRESS_PAIRS);
  write_str("-pair x ");
  write_i64(STRESS_ITERS);
  write_str(" iters: ");
  write_i64(elapsed / 1000000);
  write_str(" ms (");
  write_i64(elapsed / STRESS_ITERS);
  write_str(" ns/iter)\n");
  return true;
}

// ============================================================
// Bench M: Independent mutexes (distinct addresses)
// ============================================================
//
// N independent mutexes each with 2 threads. futex_addr-level
// expectation: since addresses hash-disperse into distinct buckets
// (N=8, expected same-bucket probability = 1 - Π(1 - k/256) ≈ 11%),
// bucket locks are almost always independent → near-linear scaling
// vs single-mutex 2T baseline.
//
// Compare to bench N below, which forces the opposite layout.

struct IndependentMutexCtx {
  FaMutexShared mutexes[16];
  int num_mutexes;
};

static void independent_mutex_round(void *arg, int idx) {
  auto *ctx = static_cast<IndependentMutexCtx *>(arg);
  mutex_fa_round(&ctx->mutexes[idx / 2], idx);
}

static void bench_independent_mutex_fa(int64_t *timings, int num_mutexes,
                                        int iters_per_mutex, int samples) {
  int warmup = samples / 10;
  IndependentMutexCtx ctx;
  ctx.num_mutexes = num_mutexes;
  for (int m = 0; m < num_mutexes; ++m) {
    ctx.mutexes[m].iters = iters_per_mutex / 2;
    ctx.mutexes[m].cs_ns = 0;
  }

  for (int s = 0; s < warmup + samples; ++s) {
    for (int m = 0; m < num_mutexes; ++m) {
      ctx.mutexes[m].lock.store(0, cpp::MemoryOrder::RELAXED);
      ctx.mutexes[m].counter = 0;
    }
    int64_t t = fresh_run_mutex_style(num_mutexes * 2, independent_mutex_round,
                                      &ctx);
    if (s >= warmup)
      timings[s - warmup] = t;
  }
}

// ============================================================
// Bench N/O: Bucket collision vs bucket dispersion
// ============================================================
//
// futex_addr partitions waiters across 256 hash buckets (hash_address()
// in futex_addr.h — shift/XOR of the address). Adjacent mutexes on a
// stack or in a struct almost always hash-disperse; but a pathological
// caller could end up with many mutexes landing on the same bucket,
// and we want a bench that measures exactly that.
//
// N.  Single-bucket hot contention: 8 addresses picked at runtime so
//     their hash() outputs collide. Each address runs a 2-thread mutex.
//     All 8 buckets share one spinlock → wake-side serialises.
//
// O.  Cross-bucket dispersion: 8 addresses picked at runtime so their
//     hash() outputs are all distinct. Same 2-thread-per-mutex workload.
//     Bucket locks are independent → near-linear to [F] 2T × 8.
//
// The ratio N/O is the parking lot's hash-dispersion penalty under
// contention — the cost a caller pays when unlucky with addresses.
// Bench M (above) measures the typical-caller case where dispersion is
// random; N and O bracket the best and worst cases for the same
// workload shape.

// Probe a pool of atomics for a set that all hash to the same bucket,
// and a set that all hash to distinct buckets.
struct BucketSets {
  static constexpr int NEEDED = 8;
  // Pool backing — size picked so both searches succeed with high
  // probability. 4096 atomics gives ~16 expected per bucket for
  // collision search, and 16× the 8 needed for dispersion search.
  // Aligned to 64B so no two atomics share a cache line (false
  // sharing would conflate this with the hash-dispersion signal).
  struct alignas(64) Atom {
    cpp::Atomic<uint32_t> val{0};
    uint64_t _pad[7]; // fill out to 64B
  };
  static constexpr int POOL = 4096;
  Atom pool_mem[POOL];

  // Output: pointers to the chosen atomics.
  cpp::Atomic<uint32_t> *colliding[NEEDED] = {}; // all hash to same bucket
  cpp::Atomic<uint32_t> *distinct[NEEDED] = {};  // all hash to distinct buckets

  bool colliding_found = false;
  bool distinct_found = false;

  void probe() {
    // Group atomics by bucket.
    static constexpr int BUCKETS = futex_addr::BUCKET_COUNT; // 256
    int head[BUCKETS];
    int next[POOL];
    int count[BUCKETS];
    for (int i = 0; i < BUCKETS; ++i) {
      head[i] = -1;
      count[i] = 0;
    }
    for (int i = 0; i < POOL; ++i) {
      unsigned bi = futex_addr::hash_address(
          reinterpret_cast<uintptr_t>(&pool_mem[i].val));
      next[i] = head[bi];
      head[bi] = i;
      count[bi]++;
    }

    // Colliding set: pick any bucket with >=NEEDED members.
    for (int bi = 0; bi < BUCKETS; ++bi) {
      if (count[bi] >= NEEDED) {
        int idx = head[bi];
        for (int k = 0; k < NEEDED; ++k) {
          colliding[k] = &pool_mem[idx].val;
          idx = next[idx];
        }
        colliding_found = true;
        break;
      }
    }

    // Distinct set: pick the first member from each of NEEDED distinct
    // non-empty buckets.
    int picked = 0;
    for (int bi = 0; bi < BUCKETS && picked < NEEDED; ++bi) {
      if (count[bi] == 0)
        continue;
      distinct[picked++] = &pool_mem[head[bi]].val;
    }
    distinct_found = (picked == NEEDED);
  }
};

struct BucketMutexWorker {
  cpp::Atomic<uint32_t> *lock;
  int iters;
  volatile int64_t counter;
};

struct BucketBenchCtx {
  BucketMutexWorker workers[BucketSets::NEEDED];
  int num_mutexes;
};

// Pair mapping (matches IndependentMutexCtx): workers (2k, 2k+1)
// share workers[k].lock. Runs the same two-state CAS-wait-store-wake
// loop as mutex_fa_round, but against a caller-supplied external
// atomic rather than an embedded mutex struct. No cs_ns — we want
// this bench to stress the wait/wake path, not the critical section.
static void bucket_mutex_round(void *arg, int idx) {
  auto *ctx = static_cast<BucketBenchCtx *>(arg);
  auto &w = ctx->workers[idx / 2];
  for (int i = 0; i < w.iters; ++i) {
    for (;;) {
      uint32_t expected = 0;
      if (w.lock->compare_exchange_weak(expected, 1u,
                                        cpp::MemoryOrder::ACQUIRE,
                                        cpp::MemoryOrder::RELAXED))
        break;
      futex_addr::wait(w.lock, 1u, nullptr);
    }
    w.counter++;
    w.lock->store(0, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(w.lock, 1);
  }
}

static void bench_bucket_layout(int64_t *timings, cpp::Atomic<uint32_t> **locks,
                                 int num_mutexes, int iters_per_mutex,
                                 int samples) {
  int warmup = samples / 10;
  BucketBenchCtx ctx;
  ctx.num_mutexes = num_mutexes;
  for (int m = 0; m < num_mutexes; ++m) {
    ctx.workers[m].lock = locks[m];
    ctx.workers[m].iters = iters_per_mutex / 2;
    ctx.workers[m].counter = 0;
  }

  for (int s = 0; s < warmup + samples; ++s) {
    for (int m = 0; m < num_mutexes; ++m) {
      locks[m]->store(0, cpp::MemoryOrder::RELAXED);
      ctx.workers[m].counter = 0;
    }
    int64_t t = fresh_run_mutex_style(num_mutexes * 2, bucket_mutex_round,
                                      &ctx);
    if (s >= warmup)
      timings[s - warmup] = t;
  }
}

// ============================================================
// Entry
// ============================================================

} // namespace LIBC_NAMESPACE_DECL

static int bench_main() {
  using namespace LIBC_NAMESPACE;

  bench_prelude();

  write_str("=== futex_addr comprehensive benchmark ===\n");
  write_str("    Ping-pong: ");
  write_i64(PING_SAMPLES);
  write_str(" samples x ");
  write_i64(PING_BATCH);
  write_str(" iter/sample.  Heavy: ");
  write_i64(HEAVY_SAMPLES);
  write_str(" samples.\n");
  write_str("    Main LP 2, workers stepped across physical cores first"
            " (LP schedule len=");
  write_i64(g_lp_schedule_len);
  write_str("), HIGH priority.\n");
  write_str("    Timer: RDTSC calibrated vs QPC.\n");
  write_str("    kuser_spin_threshold=");
  write_i64(spin_wait::kuser_spin_threshold());
  write_str("  backend=");
  write_i64(static_cast<int>(spin_wait::get_backend()));
  write_str(" (0=RELAX 1=UMWAIT 2=MWAITX)\n");
  write_str("    Bucket count=");
  write_i64(futex_addr::BUCKET_COUNT);
  write_str("\n\n");

  static int64_t timings[HEAVY_SAMPLES > PING_SAMPLES ? HEAVY_SAMPLES
                                                      : PING_SAMPLES];

  // --- A: Ping-pong latency ---
  write_str("[A] Single-pair ping-pong round-trip (ns/iter):\n");

  bench_woa(timings);
  report("WaitOnAddress/WakeByAddressSingle", timings, PING_SAMPLES);

  bench_fa(timings);
  report("futex_addr::wait/wake", timings, PING_SAMPLES);

  write_str("\n[A] 4-pair contended ping-pong (ns/iter):\n");

  bench_woa_contended(timings);
  report("WaitOnAddress (4-pair)", timings, PING_SAMPLES);

  bench_fa_contended(timings);
  report("futex_addr (4-pair)", timings, PING_SAMPLES);

  // --- B: Uncontended wake ---
  write_str("\n[B] Uncontended wake — no waiters (ns per 1000 calls):\n");

  bench_wake_empty_woa(timings);
  report("RtlWakeAddressSingle (empty)", timings, PING_SAMPLES);

  bench_wake_empty_fa(timings);
  report("futex_addr::wake (empty)", timings, PING_SAMPLES);

  // --- C: Timeout return ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[C] Timeout return latency (ns/call):\n");

  bench_timeout_woa(timings);
  report("RtlWaitOnAddress (timeout)", timings, TIMEOUT_WOA_SAMPLES);

  bench_timeout_fa(timings);
  report("futex_addr::wait (100ns to)", timings, HEAVY_SAMPLES);

  // --- C.2: Value-mismatch EAGAIN fast path ---
  write_str("\n[C.2] Value-mismatch EAGAIN (ns per 1000 calls):\n");

  bench_eagain_woa(timings);
  report("RtlWaitOnAddress (EAGAIN)", timings, PING_SAMPLES);

  bench_eagain_fa(timings);
  report("futex_addr::wait (EAGAIN)", timings, PING_SAMPLES);

  // --- E: notify_all fan-out ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[E] notify_all fan-out — wake-to-all-running (ns):\n");

  bench_fanout_woa(timings, 4);
  report("RtlWakeAddressAll (4 waiters)", timings, HEAVY_SAMPLES);

  bench_fanout_fa(timings, 4);
  report("futex_addr::wake (4 waiters)", timings, HEAVY_SAMPLES);

  bench_fanout_woa(timings, 16);
  report("RtlWakeAddressAll (16 waiters)", timings, HEAVY_SAMPLES);

  bench_fanout_fa(timings, 16);
  report("futex_addr::wake (16 waiters)", timings, HEAVY_SAMPLES);

  // --- F: Mutex-style contended lock ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[F] Mutex 100k acquire/release total time (ns):\n");

  bench_mutex_woa(timings, 2);
  report("WoA mutex        (2 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_fa(timings, 2);
  report("futex_addr mutex (2 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_raw_params(timings, 2, 100000, 0, HEAVY_SAMPLES);
  report("RawMutex         (2 threads)", timings, HEAVY_SAMPLES);

  // --- G: High thread count ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[G] High thread count:\n");

  write_str("  notify_all fan-out:\n");

  bench_fanout_woa(timings, 32);
  report("RtlWakeAddressAll (32 waiters)", timings, HEAVY_SAMPLES);

  bench_fanout_fa(timings, 32);
  report("futex_addr::wake (32 waiters)", timings, HEAVY_SAMPLES);

  constexpr int FANOUT64_SAMPLES = 50;
  bench_fanout_woa(timings, 64, FANOUT64_SAMPLES);
  report("RtlWakeAddressAll (64 waiters)", timings, FANOUT64_SAMPLES);

  bench_fanout_fa(timings, 64, FANOUT64_SAMPLES);
  report("futex_addr::wake (64 waiters)", timings, FANOUT64_SAMPLES);

  write_str("  Mutex contention:\n");

  bench_mutex_woa(timings, 8);
  report("WoA mutex        (8 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_fa(timings, 8);
  report("futex_addr mutex (8 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_woa(timings, 16);
  report("WoA mutex        (16 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_fa(timings, 16);
  report("futex_addr mutex (16 threads)", timings, HEAVY_SAMPLES);

  // --- K: Variable critical section length ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[K] Variable-CS mutex — 50k ops total (ns, lower=better):\n");
  write_str("    (cs_ns = busy-wait inside critical section)\n");
  {
    static constexpr int VCS_ITERS = 50000;
    static constexpr int VCS_CS_NS[] = {100, 1000};
    static constexpr int VCS_THREADS[] = {4, 8, 16};

    for (int ci = 0; ci < 2; ++ci) {
      int cs_ns = VCS_CS_NS[ci];
      write_str("  cs=");
      write_i64(cs_ns);
      write_str("ns:\n");

      for (int ti = 0; ti < 3; ++ti) {
        int nt = VCS_THREADS[ti];

        auto make_name = [nt](const char *pre, char *out) {
          char *p = out;
          while (*pre)
            *p++ = *pre++;
          if (nt >= 10)
            *p++ = '0' + (nt / 10);
          *p++ = '0' + (nt % 10);
          *p++ = 'T';
          *p++ = ')';
          *p = '\0';
        };

        {
          bench_mutex_fa_params(timings, nt, VCS_ITERS, cs_ns, HEAVY_SAMPLES);
          char name[64];
          make_name("futex_addr (", name);
          report(name, timings, HEAVY_SAMPLES);
          quiet_sleep_ns(50'000'000);
        }
        {
          bench_mutex_raw_params(timings, nt, VCS_ITERS, cs_ns, HEAVY_SAMPLES);
          char name[64];
          make_name("RawMutex   (", name);
          report(name, timings, HEAVY_SAMPLES);
          quiet_sleep_ns(50'000'000);
        }
        {
          bench_mutex_woa_params(timings, nt, VCS_ITERS, cs_ns, HEAVY_SAMPLES);
          char name[64];
          make_name("WoA        (", name);
          report(name, timings, HEAVY_SAMPLES);
          quiet_sleep_ns(50'000'000);
        }
      }
    }
  }

  // --- L: Oversubscription (32T, 1ns CS) ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[L] Oversubscription — 32T mutex 50k ops:\n");
  {
    static constexpr int OS_ITERS = 50000;
    int64_t times[HEAVY_SAMPLES];

    bench_mutex_fa_params(times, 32, OS_ITERS, 0, HEAVY_SAMPLES);
    report("futex_addr (32T)", times, HEAVY_SAMPLES);
    quiet_sleep_ns(50'000'000);

    bench_mutex_woa_params(times, 32, OS_ITERS, 0, HEAVY_SAMPLES);
    report("WoA        (32T)", times, HEAVY_SAMPLES);
    quiet_sleep_ns(50'000'000);

    bench_mutex_raw_params(times, 32, OS_ITERS, 0, HEAVY_SAMPLES);
    report("RawMutex   (32T)", times, HEAVY_SAMPLES);
    quiet_sleep_ns(50'000'000);
  }

  // --- M: Independent futexes (typical-caller dispersion) ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[M] Independent futexes — 8 mutexes x 2T each"
            " (50k ops/mutex):\n");
  {
    static constexpr int MIF_MUTEXES = 8;
    static constexpr int MIF_ITERS = 50000;
    static constexpr int MIF_SAMPLES = 50;
    int64_t times[MIF_SAMPLES];

    bench_independent_mutex_fa(times, MIF_MUTEXES, MIF_ITERS, MIF_SAMPLES);
    report("8 x futex_addr 2T (typical)", times, MIF_SAMPLES);
    quiet_sleep_ns(50'000'000);

    bench_mutex_fa_params(times, 2, MIF_ITERS, 0, MIF_SAMPLES);
    report("1 x futex_addr 2T (baseline)", times, MIF_SAMPLES);
    quiet_sleep_ns(50'000'000);
  }

  // --- N, O: Bucket-collision vs bucket-dispersion ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[N/O] Hash-dispersion — 8 mutexes x 2T (50k ops/mutex):\n");
  {
    static constexpr int BKT_MUTEXES = BucketSets::NEEDED;
    static constexpr int BKT_ITERS = 50000;
    static constexpr int BKT_SAMPLES = 50;

    // Heap-sized to avoid stack-pressure warnings at 512KB+.
    static BucketSets sets;
    sets.probe();

    if (!sets.distinct_found || !sets.colliding_found) {
      write_str("  (bucket probe failed: distinct_ok=");
      write_i64(sets.distinct_found ? 1 : 0);
      write_str(" colliding_ok=");
      write_i64(sets.colliding_found ? 1 : 0);
      write_str(")\n");
    } else {
      int64_t times[BKT_SAMPLES];

      // [O] Dispersion: 8 mutexes on 8 distinct buckets — expected
      // to track bench [M] closely (M uses natural hash dispersion,
      // O forces it). Gap between M and O tells us how close the
      // natural case is to the lucky case.
      bench_bucket_layout(times, sets.distinct, BKT_MUTEXES, BKT_ITERS,
                          BKT_SAMPLES);
      report("[O] 8 mutexes, 8 buckets", times, BKT_SAMPLES);
      quiet_sleep_ns(50'000'000);

      // [N] Collision: 8 mutexes all on one bucket — wake/wait must
      // serialise on the single bucket lock. Ratio [N]/[O] is the
      // parking lot's worst-case hash penalty.
      bench_bucket_layout(times, sets.colliding, BKT_MUTEXES, BKT_ITERS,
                          BKT_SAMPLES);
      report("[N] 8 mutexes, 1 bucket", times, BKT_SAMPLES);
      quiet_sleep_ns(50'000'000);
    }
  }

  // --- H: Stress test ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[H] Contended stress test (8-pair, 25k iters):\n");
  stress_contended();

  // --- J: DLL cancel-race stress ---
  // Same shape as futex_bench's [J]: many iters of a contended
  // futex_addr-mutex, verifying the counter hits the expected total.
  // A DLL-cancel / wake-claim race would corrupt the SLL and hang
  // here, so a clean counter at end is the correctness signal.
  quiet_sleep_ns(50'000'000);
  write_str("\n[J] SLL cancel-race stress test:\n");
  {
    static constexpr int DLL_STRESS_THREADS[] = {2, 4, 8, 16};
    static constexpr int DLL_STRESS_ITERS = 200000;

    for (int ti = 0; ti < 4; ++ti) {
      int nt = DLL_STRESS_THREADS[ti];
      write_str("  ");
      write_i64(nt);
      write_str("T mutex (");
      write_i64(DLL_STRESS_ITERS);
      write_str(" iters): ");

      FaMutexShared ctx;
      ctx.lock.store(0, cpp::MemoryOrder::RELAXED);
      ctx.counter = 0;
      ctx.iters = DLL_STRESS_ITERS / nt;
      ctx.cs_ns = 0;
      int64_t elapsed = fresh_run_mutex_style(nt, mutex_fa_round, &ctx);

      write_i64(elapsed / 1000000);
      write_str(" ms  counter=");
      write_i64(ctx.counter);
      write_str(ctx.counter == DLL_STRESS_ITERS ? "  OK\n" : "  MISMATCH!\n");
    }
  }

  write_str("\n=== Done ===\n");
  return 0;
}

// Hosted entry — libc startup (crt1.obj) handles wait_slot::init() via
// .CRT$XIBD and all other subsystem initialization before calling main().
extern "C" int main() { return bench_main(); }
