//===-- Futex benchmark: Treiber futex vs WaitOnAddress -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Comprehensive futex benchmark comparing the parking-lot Futex against
// Windows native RtlWaitOnAddress / RtlWakeAddressSingle / RtlWakeAddressAll.
//
// Test matrix:
//   A. Ping-pong latency (1 pair, 4 pairs contended)
//   B. Uncontended wake (notify with no waiters — live_count fast path)
//   C. Timeout return (lock-free timeout vs RtlWaitOnAddress timeout)
//   D. Atomic RMW throughput (fetch_add: single lock xadd vs CAS loop)
//   E. notify_all fan-out (wake N sleeping threads simultaneously)
//   F. Mutex-style acquire/release (realistic contended lock pattern)
//
// Hosted build — links against llvm-libc (crt1.obj + c.dll). The libc
// startup walker runs .CRT$XIBD which calls wait_slot::init() before
// main(). Output uses direct NtWriteFile to avoid stdio buffering.
//
// Build (via CMake integration test or standalone):
//   clang++ -std=c++17 -O2 -target x86_64-pc-windows-ntposix \
//     -DLIBC_NAMESPACE=__llvm_libc -DFUTEX_BENCH_HOSTED \
//     -I<llvm-project>/libc -I<llvm-project> \
//     futex_bench.cpp -lc -lntdll -o futex_bench.exe
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/futex_instrument.h"
#include "test/src/__support/threads/windows/futex_bench_common.h"

namespace LIBC_NAMESPACE_DECL {

// Per-worker enter/exit hooks for fresh_run_mutex_style — bind the
// futex_instrument TLS phase slot so set_phase / bump_heartbeat / the
// tripwire dumper all land in g_phases[idx] instead of no-op. Every
// Futex-touching bench passes these so a late-sample hang produces a
// per-worker phase table instead of a dead thread.
static void instrument_enter(int idx) {
  if (idx >= 0 &&
      static_cast<uint32_t>(idx) < futex_instrument::kMaxInstrWorkers) {
    auto &wp = futex_instrument::g_phases[idx];
    wp.worker_id.store(static_cast<uint32_t>(idx), cpp::MemoryOrder::RELAXED);
    wp.phase.store(futex_instrument::PHASE_INIT, cpp::MemoryOrder::RELAXED);
    wp.iter.store(0, cpp::MemoryOrder::RELAXED);
    wp.last_ret.store(0, cpp::MemoryOrder::RELAXED);
    wp.heartbeat.store(0, cpp::MemoryOrder::RELAXED);
    futex_instrument::bind_self(&wp);
  }
}

static void instrument_exit(int /*idx*/) {
  futex_instrument::set_phase(futex_instrument::PHASE_WORKER_DONE);
}

// ============================================================
// Bench 1: WaitOnAddress / WakeByAddressSingle
// ============================================================

struct WoaPP {
  cpp::Atomic<uint32_t> val{0};
  cpp::Atomic<bool> ready{false};
};

// Helper: drive one WoA ping-pong round-trip. Inlined in the hot loop
// to keep the timing window tight.
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
// Bench 2: Futex::wait / Futex::notify_one
// ============================================================

struct FutexPP {
  Futex val{0};
  cpp::Atomic<bool> ready{false};
};

LIBC_INLINE void futex_pp_roundtrip(FutexPP &pp) {
  // store_and_notify (SEQ_CST), NOT split store(RELEASE)+notify_one.
  // On x86 the split pattern reorders: the notify's ACQUIRE load of
  // stack_ can retire before the RELEASE store to value_ drains from the
  // store buffer, stranding a waiter that pushed in between.
  pp.val.store_and_notify(1);
  while (pp.val.load(cpp::MemoryOrder::ACQUIRE) != 0)
    pp.val.wait(1);
}

static void futex_responder(void *arg) {
  auto *pp = static_cast<FutexPP *>(arg);
  pp->ready.store(true, cpp::MemoryOrder::RELEASE);
  for (int i = 0; i < PING_TOTAL; ++i) {
    while (pp->val.load(cpp::MemoryOrder::ACQUIRE) != 1)
      pp->val.wait(0);
    pp->val.store_and_notify(0);
  }
}

static void bench_futex(int64_t *timings) {
  reset_lp();
  FutexPP pp;
  ThreadArg ta;
  pthread_t h = start_thread(futex_responder, &pp, ta);
  while (!pp.ready.load(cpp::MemoryOrder::ACQUIRE))
    ;

  for (int i = 0; i < PING_WARMUP_ITERS; ++i)
    futex_pp_roundtrip(pp);

  for (int s = 0; s < PING_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < PING_BATCH; ++k)
      futex_pp_roundtrip(pp);
    timings[s] = (now_ns() - t0) / PING_BATCH;
  }
  join_thread(h);
}

// ============================================================
// Bench 4: 4-pair contended — Futex
// ============================================================

static constexpr int NUM_PAIRS = 4;

struct ContendedCtx {
  Futex val{0};
  cpp::Atomic<bool> ready{false};
};

static void contended_responder(void *arg) {
  auto *ctx = static_cast<ContendedCtx *>(arg);
  ctx->ready.store(true, cpp::MemoryOrder::RELEASE);
  for (int i = 0; i < PING_TOTAL; ++i) {
    while (ctx->val.load(cpp::MemoryOrder::ACQUIRE) != 1)
      ctx->val.wait(0);
    ctx->val.store_and_notify(0);
  }
}

LIBC_INLINE void contended_roundtrip(ContendedCtx *ctx) {
  for (int p = 0; p < NUM_PAIRS; ++p)
    ctx[p].val.store_and_notify(1);
  for (int p = 0; p < NUM_PAIRS; ++p)
    while (ctx[p].val.load(cpp::MemoryOrder::ACQUIRE) != 0)
      ctx[p].val.wait(1);
}

static void bench_contended(int64_t *timings) {
  reset_lp();
  ContendedCtx ctx[NUM_PAIRS];
  ThreadArg tas[NUM_PAIRS];
  pthread_t handles[NUM_PAIRS];

  for (int p = 0; p < NUM_PAIRS; ++p)
    handles[p] = start_thread(contended_responder, &ctx[p], tas[p]);
  for (int p = 0; p < NUM_PAIRS; ++p)
    while (!ctx[p].ready.load(cpp::MemoryOrder::ACQUIRE))
      ;

  for (int i = 0; i < PING_WARMUP_ITERS; ++i)
    contended_roundtrip(ctx);

  for (int s = 0; s < PING_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < PING_BATCH; ++k)
      contended_roundtrip(ctx);
    timings[s] = (now_ns() - t0) / PING_BATCH;
  }
  for (int p = 0; p < NUM_PAIRS; ++p)
    join_thread(handles[p]);
}

// ============================================================
// Stress: N-pair contended ping-pong (race exposure)
// ============================================================
//
// Maximizes the WAITING→IN_KERNEL / SIGNALED race window by:
//   - More pairs (8) = more concurrent parking lot activity
//   - store_and_notify on all pairs before waiting = burst of wakes
//   - 50k iterations = enough to trigger intermittent races
//   - No timing overhead in the hot loop

static constexpr int STRESS_PAIRS = 8;
// Threads are spread across distinct physical cores, so each ping-pong
// iter is cross-core (~2-3μs) rather than intra-SMT-pair (<1μs). 25k
// iters is ~200ms of wall time and surfaces the same race windows.
static constexpr int STRESS_ITERS = 25000;

static cpp::Atomic<int> stress_iter{0};
static ContendedCtx *stress_ctx_ptr;

static void stress_responder(void *arg) {
  auto *ctx = static_cast<ContendedCtx *>(arg);
  ctx->ready.store(true, cpp::MemoryOrder::RELEASE);
  for (int i = 0; i < STRESS_ITERS; ++i) {
    while (ctx->val.load(cpp::MemoryOrder::ACQUIRE) != 1)
      ctx->val.wait(0);
    ctx->val.store_and_notify(0);
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
      if (p) write_str(",");
      write_i64(stress_ctx_ptr[p].val.load(cpp::MemoryOrder::RELAXED));
    }
    write_str("]\n");
  }
}

static bool stress_contended() {
  reset_lp();
  ContendedCtx ctx[STRESS_PAIRS];
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
    for (int p = 0; p < STRESS_PAIRS; ++p)
      ctx[p].val.store_and_notify(1);
    for (int p = 0; p < STRESS_PAIRS; ++p)
      while (ctx[p].val.load(cpp::MemoryOrder::ACQUIRE) != 0)
        ctx[p].val.wait(1);
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
// Bench 5: 4-pair contended — WaitOnAddress
// ============================================================

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

// ============================================================
// Bench 6: Uncontended wake — Futex (live_count fast path)
// ============================================================

// Uncontended wake: the fast path is ~1 atomic load + branch, under
// 1ns on modern CPUs. Batch aggressively so integer-ns/iter resolution
// survives division.
static constexpr int EMPTY_WAKE_BATCH = 20000;

// Result is reported in ns per 1000 calls so sub-ns per-call operations
// stay readable at integer resolution.
static void bench_wake_empty_futex(int64_t *timings) {
  Futex f{0};
  for (int i = 0; i < EMPTY_WAKE_BATCH; ++i)
    f.notify_one();
  for (int s = 0; s < PING_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < EMPTY_WAKE_BATCH; ++k)
      f.notify_one();
    timings[s] = (now_ns() - t0) * 1000 / EMPTY_WAKE_BATCH;
  }
}

// ============================================================
// Bench 7: Uncontended wake — WaitOnAddress
// ============================================================

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
// Bench 8: Timeout return — Futex (lock-free timeout)
// ============================================================

static Futex::Timeout make_immediate_timeout() {
  struct timespec ts = {0, 1}; // 1 ns — effectively immediate
  return *internal::AbsTimeout::from_timespec(ts, /*is_realtime=*/false);
}

// Futex lock-free timeout returns in a few microseconds without touching
// the kernel — small enough that QPC/RDTSC overhead and branch prediction
// warmup matter. Batch samples to amortize.
static constexpr int TIMEOUT_BATCH = 20;

static void bench_timeout_futex(int64_t *timings) {
  Futex f{42};
  // Warmup
  for (int i = 0; i < 200; ++i) {
    auto to = make_immediate_timeout();
    f.wait(42, {to});
  }
  for (int s = 0; s < HEAVY_SAMPLES; ++s) {
    int64_t t0 = now_ns();
    for (int k = 0; k < TIMEOUT_BATCH; ++k) {
      auto to = make_immediate_timeout();
      f.wait(42, {to});
    }
    timings[s] = (now_ns() - t0) / TIMEOUT_BATCH;
  }
}

// ============================================================
// Bench 9: Timeout return — WaitOnAddress
// ============================================================

// RtlWaitOnAddress with a non-zero timeout always hits the kernel timer
// tick (~15.6ms). 100 samples would cost 1.5s for no extra signal, so
// use a small fixed sample count. No batching (each sample already
// takes ~10^7ns — QPC/RDTSC overhead is invisible).
static constexpr int TIMEOUT_WOA_SAMPLES = 20;

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
// Bench 10: fetch_add throughput — Futex (single lock xadd)
// ============================================================

struct FetchAddPoolCtx {
  Futex *target;
  int iters;
};

static void fetch_add_round(void *arg, int /*idx*/) {
  auto *ctx = static_cast<FetchAddPoolCtx *>(arg);
  for (int i = 0; i < ctx->iters; ++i)
    ctx->target->fetch_add(1, cpp::MemoryOrder::RELAXED);
}

static void bench_fetch_add(int64_t *timings, int num_threads) {
  static constexpr int FETCH_ITERS = 1000000;

  Futex counter{0};
  FetchAddPoolCtx ctx{&counter, FETCH_ITERS / num_threads};

  for (int s = 0; s < HEAVY_WARMUP + HEAVY_SAMPLES; ++s) {
    if (s > 0)
      quiet_sleep_ns(1'000'000); // 1ms inter-sample drain
    counter.store(0);
    int64_t t = fresh_run_mutex_style(num_threads, fetch_add_round, &ctx,
                                      instrument_enter, instrument_exit);
    if (s >= HEAVY_WARMUP)
      timings[s - HEAVY_WARMUP] = t;
  }
}

// ============================================================
// Bench 11: fetch_add throughput — raw InterlockedAdd64
// ============================================================

struct RawFetchAddPoolCtx {
  volatile int64_t *target;
  int iters;
};

static void raw_fetch_add_round(void *arg, int /*idx*/) {
  auto *ctx = static_cast<RawFetchAddPoolCtx *>(arg);
  for (int i = 0; i < ctx->iters; ++i)
    __atomic_fetch_add(ctx->target, 1, __ATOMIC_RELAXED);
}

static void bench_raw_fetch_add(int64_t *timings, int num_threads) {
  static constexpr int FETCH_ITERS = 1000000;

  volatile int64_t counter = 0;
  RawFetchAddPoolCtx ctx{&counter, FETCH_ITERS / num_threads};

  for (int s = 0; s < HEAVY_WARMUP + HEAVY_SAMPLES; ++s) {
    if (s > 0)
      quiet_sleep_ns(1'000'000); // 1ms inter-sample drain
    counter = 0;
    int64_t t = fresh_run_mutex_style(num_threads, raw_fetch_add_round, &ctx,
                                      instrument_enter, instrument_exit);
    if (s >= HEAVY_WARMUP)
      timings[s - HEAVY_WARMUP] = t;
  }
}

// ============================================================
// Bench 12: notify_all fan-out — Futex
// ============================================================

struct FanOutCtx {
  Futex *futex;
  cpp::Atomic<int> arrived{0};
  cpp::Atomic<int> done{0};
};

// Fan-out uses fresh-thread-per-sample. Pool-based fan-out hit
// reproducible hangs at 16 waiters on 13 LPs: concurrent Futex::wait
// on the pool's round_seq while 16 waiters also use Futex::wait on
// a per-round futex would occasionally miss a notify (suspect: wait
// slot bucket collision under heavy churn). Fan-out's timing window
// is trigger-wake → all-done; thread creation happens pre-t0, so
// there's no measurement cost to spawning fresh.
static void fanout_waiter(void *arg) {
  auto *ctx = static_cast<FanOutCtx *>(arg);
  ctx->arrived.fetch_add(1, cpp::MemoryOrder::RELEASE);
  while (ctx->futex->load(cpp::MemoryOrder::ACQUIRE) == 0)
    ctx->futex->wait(0);
  ctx->done.fetch_add(1, cpp::MemoryOrder::RELEASE);
}

static void bench_fanout_futex(int64_t *timings, int num_waiters,
                                int samples = HEAVY_SAMPLES) {
  for (int s = 0; s < samples; ++s) {
    if (s > 0)
      quiet_sleep_ns(1'000'000); // 1ms drain (longer cools fresh-spawn path)
    reset_lp();
    Futex f{0};
    FanOutCtx ctx;
    ctx.futex = &f;
    ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
    ctx.done.store(0, cpp::MemoryOrder::RELAXED);

    ThreadArg tas[128];
    pthread_t handles[128];
    for (int t = 0; t < num_waiters; ++t)
      handles[t] = start_thread(fanout_waiter, &ctx, tas[t]);

    while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < num_waiters)
      ;
    quiet_sleep_ns(1'000'000); // 1ms settle

    int64_t t0 = now_ns();
    f.store_and_notify_all(1);
    while (ctx.done.load(cpp::MemoryOrder::ACQUIRE) < num_waiters)
      ;
    timings[s] = now_ns() - t0;

    for (int t = 0; t < num_waiters; ++t)
      join_thread(handles[t]);
  }
}

static void bench_fanout_futex_chunked(int64_t *timings, int num_waiters,
                                        uint32_t chunk_size,
                                        bool do_yield = true,
                                        int samples = HEAVY_SAMPLES) {
  for (int s = 0; s < samples; ++s) {
    if (s > 0)
      quiet_sleep_ns(1'000'000); // 1ms drain (longer cools fresh-spawn path)
    reset_lp();
    Futex f{0};
    FanOutCtx ctx;
    ctx.futex = &f;
    ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
    ctx.done.store(0, cpp::MemoryOrder::RELAXED);

    ThreadArg tas[128];
    pthread_t handles[128];
    for (int t = 0; t < num_waiters; ++t)
      handles[t] = start_thread(fanout_waiter, &ctx, tas[t]);

    while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < num_waiters)
      ;
    quiet_sleep_ns(1'000'000); // 1ms settle

    int64_t t0 = now_ns();
    f.store_and_notify_all_chunked(1, chunk_size, do_yield);
    while (ctx.done.load(cpp::MemoryOrder::ACQUIRE) < num_waiters)
      ;
    timings[s] = now_ns() - t0;

    for (int t = 0; t < num_waiters; ++t)
      join_thread(handles[t]);
  }
}

// ============================================================
// Bench 13: notify_all fan-out — WaitOnAddress
// ============================================================

struct WoaFanOutCtx {
  cpp::Atomic<uint32_t> val{0};
  cpp::Atomic<int> arrived{0};
  cpp::Atomic<int> done{0};
};

// Fresh-thread-per-sample — mirrors bench_fanout_futex for the same
// reason: pool-based fan-out hits reproducible hangs at 16+ waiters on
// oversubscribed LPs due to the pool's own Futex::wait on round_seq
// racing with the per-round wait. Fan-out's timing window is
// trigger-wake → all-done, which starts after all threads are up, so
// spawning fresh costs nothing measured.
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
    if (s > 0)
      quiet_sleep_ns(1'000'000); // 1ms drain (longer cools fresh-spawn path)
    reset_lp();
    WoaFanOutCtx ctx;
    ctx.val.store(0, cpp::MemoryOrder::RELAXED);
    ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
    ctx.done.store(0, cpp::MemoryOrder::RELAXED);

    ThreadArg tas[128];
    pthread_t handles[128];
    for (int t = 0; t < num_waiters; ++t)
      handles[t] = start_thread(woa_fanout_waiter, &ctx, tas[t]);

    while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < num_waiters)
      ;
    quiet_sleep_ns(1'000'000); // 1ms settle

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
// Bench 14: Mutex-style acquire/release — Futex
// ============================================================

// ============================================================
// Bench 14: Mutex-style acquire/release — RawMutex
// Measures the real wrapper that users actually hit via
// pthread_mutex_*. Open-coding a second "raw Futex" mutex would
// just duplicate the same tri-state handoff protocol RawMutex
// already implements — zero measurement value, two copies of a
// subtle protocol to keep in sync. Futex-primitive cost signal
// lives in the ping-pong / notify / RMW benches above (A, B, C,
// D, E) which test producer-consumer wake patterns rather than
// mutual exclusion.
// ============================================================

struct RawMutexBenchShared {
  RawMutex lock;
  volatile int64_t counter;
  int iters;
  int cs_ns = 0;
};

static void mutex_rawmutex_round(void *arg, int /*idx*/) {
  auto *ctx = static_cast<RawMutexBenchShared *>(arg);
  for (int i = 0; i < ctx->iters; ++i) {
    futex_instrument::set_phase(futex_instrument::PHASE_IN_CAS);
    if (auto *wp = futex_instrument::tls_my_phase)
      wp->iter.store(static_cast<uint32_t>(i), cpp::MemoryOrder::RELAXED);
    ctx->lock.lock();
    futex_instrument::set_phase(futex_instrument::PHASE_IN_CS);
    futex_instrument::bump_heartbeat();
    if (ctx->cs_ns > 0) {
      int64_t t0 = now_ns();
      while (now_ns() - t0 < ctx->cs_ns)
        ;
    }
    ctx->counter++;
    futex_instrument::set_phase(futex_instrument::PHASE_IN_UNLOCK_NOTIFY);
    ctx->lock.unlock();
  }
  futex_instrument::set_phase(futex_instrument::PHASE_POST_WAIT_OK);
}

// Tripwire watchdog context — populated when the bench is running the
// hang-prone 16T RawMutex case. Read by the watchdog thread on timeout
// to dump counters + the RawMutex's Futex stack for forensics.
static cpp::Atomic<void *> g_tripwire_lock_futex{nullptr};
static cpp::Atomic<int> g_tripwire_num_workers{0};
static cpp::Atomic<int> g_tripwire_bench_done{0};
static cpp::Atomic<int> g_tripwire_sample_idx{0};

static void tripwire_watchdog(void *) {
  // 60-second timeout — plenty of time for a healthy 16T RawMutex bench
  // to finish (normally <1s), but short enough that a hang is reported
  // before the user's outer timeout expires.
  for (int i = 0; i < 60; ++i) {
    test_support::sleep_ms(1000);
    if (g_tripwire_bench_done.load(cpp::MemoryOrder::ACQUIRE))
      return;
  }
  write_str("\n\n");
  write_str("================================================================\n");
  write_str("TRIPWIRE WATCHDOG: 60s elapsed, bench not done (sample=");
  write_i64(g_tripwire_sample_idx.load(cpp::MemoryOrder::ACQUIRE));
  write_str(")\n");
  write_str("================================================================\n");
  void *futex_ptr = g_tripwire_lock_futex.load(cpp::MemoryOrder::ACQUIRE);
  int nworkers = g_tripwire_num_workers.load(cpp::MemoryOrder::ACQUIRE);
  futex_instrument::dump("Lost-alert tripwire dump", futex_ptr,
                         static_cast<uint32_t>(nworkers));
  write_str("\n(Watchdog exits; bench continues hanging. Ctrl-C to quit.)\n");
}

static void bench_mutex_rawmutex_params(int64_t *timings, int num_threads,
                                         int total_iters, int cs_ns,
                                         int samples,
                                         int drain_ns = 1'000'000) {
  int warmup = samples / 10;
  RawMutexBenchShared ctx;
  ctx.iters = total_iters / num_threads;
  ctx.cs_ns = cs_ns;

  // Arm the tripwire watchdog for any sufficiently-contended call
  // (num_threads >= 8). Covers Phase [G] 16T basic contention AND
  // Phase [K] cs>0 variable-CS — the latter moves waiters into the
  // Phase 3/4 kernel path (longer CS ⇒ less Phase 2.5 spin coverage),
  // which is a different stress profile for the alert-skip gate.
  bool arm_watchdog = (num_threads >= 8);
  pthread_t wd_handle{};
  ThreadArg wd_arg;
  if (arm_watchdog) {
    futex_instrument::set_writers(write_str);
    futex_instrument::reset();
    g_tripwire_lock_futex.store(&ctx.lock, cpp::MemoryOrder::RELEASE);
    g_tripwire_num_workers.store(num_threads, cpp::MemoryOrder::RELEASE);
    g_tripwire_bench_done.store(0, cpp::MemoryOrder::RELEASE);
    g_tripwire_sample_idx.store(0, cpp::MemoryOrder::RELEASE);
    wd_handle = start_thread(tripwire_watchdog, nullptr, wd_arg);
  }

  for (int s = 0; s < warmup + samples; ++s) {
    if (s > 0)
      quiet_sleep_ns(drain_ns); // inter-sample drain
    if (arm_watchdog)
      g_tripwire_sample_idx.store(s, cpp::MemoryOrder::RELEASE);
    ctx.lock.reset();
    ctx.counter = 0;
    int64_t t = fresh_run_mutex_style(num_threads, mutex_rawmutex_round, &ctx,
                                      instrument_enter, instrument_exit);
    if (s >= warmup)
      timings[s - warmup] = t;
  }

  if (arm_watchdog) {
    g_tripwire_bench_done.store(1, cpp::MemoryOrder::RELEASE);
    join_thread(wd_handle);
  }
}

static void bench_mutex_rawmutex(int64_t *timings, int num_threads) {
  // 2T sample window is ~4ms; a 1ms inter-sample drain forces cold-cache
  // re-establishment that dominates measurement. >2T runs are 6ms+ and
  // benefit from full DPC drain.
  int drain = (num_threads <= 2) ? 200'000 : 1'000'000;
  bench_mutex_rawmutex_params(timings, num_threads, 150000, 0, HEAVY_SAMPLES,
                              drain);
}

// ============================================================
// Bench 15: Mutex-style acquire/release — WaitOnAddress
// ============================================================

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
                                    int total_iters, int cs_ns,
                                    int samples,
                                    int drain_ns = 1'000'000) {
  int warmup = samples / 10;
  WoaMutexShared ctx;
  ctx.iters = total_iters / num_threads;
  ctx.cs_ns = cs_ns;

  for (int s = 0; s < warmup + samples; ++s) {
    if (s > 0)
      quiet_sleep_ns(drain_ns); // inter-sample drain
    ctx.lock.store(0, cpp::MemoryOrder::RELAXED);
    ctx.counter = 0;
    int64_t t = fresh_run_mutex_style(num_threads, mutex_woa_round, &ctx,
                                      instrument_enter, instrument_exit);
    if (s >= warmup)
      timings[s - warmup] = t;
  }
}

static void bench_mutex_woa(int64_t *timings, int num_threads) {
  int drain = (num_threads <= 2) ? 200'000 : 1'000'000;
  bench_mutex_woa_params(timings, num_threads, 150000, 0, HEAVY_SAMPLES, drain);
}

// [M]-style: N independent mutexes, each with 2 contending threads. All
// mutexes run in parallel. Tests wait-slot pool / bucket contention
// across multiple simultaneously-contended RawMutex instances.
struct IndependentMutexCtx {
  RawMutexBenchShared mutexes[16]; // up to 16 mutexes
  int num_mutexes;
};

static void independent_mutex_round(void *arg, int idx) {
  auto *ctx = static_cast<IndependentMutexCtx *>(arg);
  // Workers are paired: (idx/2) selects the mutex.
  mutex_rawmutex_round(&ctx->mutexes[idx / 2], idx);
}

static void bench_independent_mutex_rawmutex(int64_t *timings, int num_mutexes,
                                               int iters_per_mutex,
                                               int samples,
                                               int drain_ns = 1'000'000) {
  int warmup = samples / 10;

  IndependentMutexCtx ctx;
  ctx.num_mutexes = num_mutexes;
  for (int m = 0; m < num_mutexes; ++m) {
    ctx.mutexes[m].iters = iters_per_mutex / 2;
    ctx.mutexes[m].cs_ns = 0;
  }

  for (int s = 0; s < warmup + samples; ++s) {
    if (s > 0)
      quiet_sleep_ns(drain_ns); // inter-sample drain
    for (int m = 0; m < num_mutexes; ++m) {
      ctx.mutexes[m].lock.reset();
      ctx.mutexes[m].counter = 0;
    }
    int64_t t = fresh_run_mutex_style(num_mutexes * 2, independent_mutex_round,
                                      &ctx, instrument_enter, instrument_exit);
    if (s >= warmup)
      timings[s - warmup] = t;
  }
}

} // namespace LIBC_NAMESPACE_DECL

// ============================================================
// Entry
// ============================================================

static int bench_main() {
  using namespace LIBC_NAMESPACE;

  bench_prelude();

  write_str("=== Futex comprehensive benchmark ===\n");
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
  write_str("), ABOVE_NORMAL priority.\n");
  write_str("    Timer: RDTSC calibrated vs QPC.\n");
  write_str("    kuser_spin_threshold=");
  write_i64(spin_wait::kuser_spin_threshold());
  write_str("  backend=");
  write_i64(static_cast<int>(spin_wait::get_backend()));
  write_str(" (0=RELAX 1=UMWAIT 2=MWAITX)\n\n");

  // Single shared buffer sized for the largest sample count.
  static int64_t timings[HEAVY_SAMPLES > PING_SAMPLES ? HEAVY_SAMPLES
                                                      : PING_SAMPLES];

  // --- A: Ping-pong latency ---
  // bench_futex and bench_futex_saw are now identical (both use
  // store_and_notify); dropped the redundant line. Single pair + 4-pair
  // contended is enough to separate hot-path from parking-lot behavior.
  write_str("[A] Single-pair ping-pong round-trip (ns/iter):\n");

  bench_woa(timings);
  report("WaitOnAddress/WakeByAddressSingle", timings, PING_SAMPLES);

  bench_futex(timings);
  report("Futex::store_and_notify", timings, PING_SAMPLES);

  write_str("\n[A] 4-pair contended ping-pong (ns/iter):\n");

  bench_woa_contended(timings);
  report("WaitOnAddress (4-pair)", timings, PING_SAMPLES);

  bench_contended(timings);
  report("Futex::store_and_notify (4-pair)", timings, PING_SAMPLES);

  // --- B: Uncontended wake ---
  // Fast-path hits are sub-nanosecond, so report per 1000 calls.
  write_str("\n[B] Uncontended wake — no waiters (ns per 1000 calls):\n");

  bench_wake_empty_woa(timings);
  report("RtlWakeAddressSingle (empty)", timings, PING_SAMPLES);

  bench_wake_empty_futex(timings);
  report("Futex::notify_one (empty)", timings, PING_SAMPLES);

  // --- C: Timeout return ---
  // WoA timeout always hits the ~15.6ms kernel timer tick — small
  // sample count is fine (the signal is the tick itself, not jitter).
  // Futex timeout is lock-free (~few μs), so it's batched.
  quiet_sleep_ns(50'000'000); // decorrelate from prior section
  write_str("\n[C] Timeout return latency (ns/call):\n");

  bench_timeout_woa(timings);
  report("RtlWaitOnAddress (timeout)", timings, TIMEOUT_WOA_SAMPLES);

  bench_timeout_futex(timings);
  report("Futex::wait (lock-free timeout)", timings, HEAVY_SAMPLES);

  // --- D: Atomic RMW throughput (contended) ---
  // 1-thread case dropped: both variants reduce to the same lock-prefixed
  // xadd with ~1% delta — no signal. Contended case is where the
  // Futex::fetch_add CAS loop diverges from raw lock-xadd.
  quiet_sleep_ns(50'000'000);
  write_str("\n[D] fetch_add 1M ops total time (ns, lower=better):\n");

  bench_raw_fetch_add(timings, 4);
  report("raw lock xadd (4 threads)", timings, HEAVY_SAMPLES);

  bench_fetch_add(timings, 4);
  report("Futex::fetch_add (4 threads)", timings, HEAVY_SAMPLES);

  // --- E: notify_all fan-out ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[E] notify_all fan-out — wake-to-all-running (ns):\n");

  bench_fanout_woa(timings, 4);
  report("RtlWakeAddressAll (4 waiters)", timings, HEAVY_SAMPLES);

  bench_fanout_futex(timings, 4);
  report("Futex::store_and_notify_all (4)", timings, HEAVY_SAMPLES);

  bench_fanout_woa(timings, 16);
  report("RtlWakeAddressAll (16 waiters)", timings, HEAVY_SAMPLES);

  bench_fanout_futex(timings, 16);
  report("Futex::store_and_notify_all (16)", timings, HEAVY_SAMPLES);

  // --- F: Mutex-style contended lock ---
  // 4-thread case covered in [K] cs=100ns below; no need to duplicate.
  quiet_sleep_ns(50'000'000);
  write_str("\n[F] Mutex 100k acquire/release total time (ns):\n");

  bench_mutex_woa(timings, 2);
  report("WaitOnAddress mutex (2 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_rawmutex(timings, 2);
  report("RawMutex (2 threads)", timings, HEAVY_SAMPLES);

  // --- G: High thread count ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[G] High thread count:\n");

  write_str("  notify_all fan-out:\n");

  bench_fanout_woa(timings, 32);
  report("RtlWakeAddressAll (32 waiters)", timings, HEAVY_SAMPLES);

  bench_fanout_futex(timings, 32);
  report("Futex::store_and_notify_all (32)", timings, HEAVY_SAMPLES);

  // 32T fan-out is the upper bound: at 4× oversubscription on a typical
  // 16-LP desktop the result is dominated by NT dispatcher-lock contention
  // and the box becomes nearly inert (Task Manager / kill-switch
  // unreachable even at ABOVE_NORMAL priority). The 32T pair below uses
  // a reduced sample count to keep total spawn budget bounded.
  constexpr int FANOUT_HIGH_SAMPLES = 50;
  bench_fanout_woa(timings, 32, FANOUT_HIGH_SAMPLES);
  report("RtlWakeAddressAll (32 waiters, hi-N)", timings, FANOUT_HIGH_SAMPLES);

  bench_fanout_futex(timings, 32, FANOUT_HIGH_SAMPLES);
  report("Futex::store_and_notify_all (32, hi-N)", timings, FANOUT_HIGH_SAMPLES);

  // --- Chunk size reference (32 waiters) ---
  // A/B reference only; production notify_all uses single-pass. 30
  // samples is plenty for a qualitative comparison.
  write_str("\n  Chunk reference (32 waiters):\n");
  {
    constexpr int CHUNKREF_SAMPLES = 30;
    uint32_t lp = *reinterpret_cast<const volatile uint32_t *>(0x7FFE03C0ull);
    write_str("    LP count=");
    write_i64(lp);
    write_str("\n");

    // Production: single-pass all-at-once (matches notify_all)
    bench_fanout_futex_chunked(timings, 32, 32, false, CHUNKREF_SAMPLES);
    report("single-pass (production)", timings, CHUNKREF_SAMPLES);

    // Reference: LP-chunked + yield (previous approach)
    bench_fanout_futex_chunked(timings, 32, lp, true, CHUNKREF_SAMPLES);
    { char n[64]; char *p = n;
      const char *s = "LP-chunked+yield ("; while(*s)*p++=*s++;
      if(lp>=10)*p++='0'+(lp/10); *p++='0'+(lp%10);
      *p++=')'; *p='\0';
      report(n, timings, CHUNKREF_SAMPLES); }
  }

  // --- Diagnostic: 32-waiter wake latency breakdown ---
  write_str("\n  32-waiter wake breakdown (single run):\n");
  {
    static constexpr int DIAG_N = 32;
    // Per-thread wake timestamps. Written by each waiter, read by main.
    static cpp::Atomic<int64_t> wake_ts[DIAG_N];
    struct DiagCtx {
      Futex *futex;
      cpp::Atomic<int> arrived{0};
      cpp::Atomic<int> done{0};
      int thread_index_base; // starting index for wake_ts
    };

    // LP assignment tracking for correlation.
    static cpp::Atomic<int> thread_lp[DIAG_N];

    // Futex diagnostic
    {
      auto diag_waiter = +[](void *arg) {
        auto *ctx = static_cast<DiagCtx *>(arg);
        int my_idx = ctx->arrived.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
        // Record assigned LP (4 + arrival index, set by start_thread).
        // This is the PINNED LP, not the actual running LP — on a 16-LP
        // system, threads pinned to LP >= 16 get folded by the scheduler.
        thread_lp[my_idx].store(4 + my_idx, cpp::MemoryOrder::RELAXED);
        while (ctx->futex->load(cpp::MemoryOrder::ACQUIRE) == 0)
          ctx->futex->wait(0);
        wake_ts[my_idx].store(now_ns(), cpp::MemoryOrder::RELAXED);
        ctx->done.fetch_add(1, cpp::MemoryOrder::RELEASE);
      };

      reset_lp();
      Futex f{0};
      DiagCtx ctx;
      ctx.futex = &f;
      ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
      ctx.done.store(0, cpp::MemoryOrder::RELAXED);
      for (int i = 0; i < DIAG_N; ++i) {
        wake_ts[i].store(0, cpp::MemoryOrder::RELAXED);
        thread_lp[i].store(-1, cpp::MemoryOrder::RELAXED);
      }

      ThreadArg tas[DIAG_N];
      pthread_t handles[DIAG_N];
      for (int t = 0; t < DIAG_N; ++t)
        handles[t] = start_thread(diag_waiter, &ctx, tas[t]);
      while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < DIAG_N)
        ;
      // Let all threads reach kernel sleep.
      { LARGE_INTEGER d; d.QuadPart = -20000; ::NtDelayExecution(FALSE, &d); }

      int64_t t_pre = now_ns();
      f.store_and_notify_all(1);
      int64_t t_post = now_ns();
      while (ctx.done.load(cpp::MemoryOrder::ACQUIRE) < DIAG_N)
        ;
      int64_t t_all = now_ns();

      write_str("    Futex notify_all() call:    ");
      write_i64(t_post - t_pre); write_str(" ns\n");
      write_str("    Futex all-done latency:     ");
      write_i64(t_all - t_pre); write_str(" ns\n");

      // Collect and sort per-thread wake times relative to t_pre.
      int64_t deltas[DIAG_N];
      for (int i = 0; i < DIAG_N; ++i)
        deltas[i] = wake_ts[i].load(cpp::MemoryOrder::RELAXED) - t_pre;
      insertion_sort(deltas, DIAG_N);

      write_str("    Futex per-thread wake (ns): first=");
      write_i64(deltas[0]);
      write_str("  p25="); write_i64(deltas[DIAG_N / 4]);
      write_str("  med="); write_i64(deltas[DIAG_N / 2]);
      write_str("  p75="); write_i64(deltas[DIAG_N * 3 / 4]);
      write_str("  last="); write_i64(deltas[DIAG_N - 1]);
      write_str("\n");

      // LP correlation: sort by wake time, show LP and latency together.
      // Build index pairs for sorted output.
      struct LPWake { int64_t delta; int lp; int idx; };
      LPWake lp_wakes[DIAG_N];
      for (int i = 0; i < DIAG_N; ++i) {
        lp_wakes[i].delta = wake_ts[i].load(cpp::MemoryOrder::RELAXED) - t_pre;
        lp_wakes[i].lp = thread_lp[i].load(cpp::MemoryOrder::RELAXED);
        lp_wakes[i].idx = i;
      }
      // Simple sort by delta.
      for (int i = 1; i < DIAG_N; ++i) {
        LPWake key = lp_wakes[i];
        int j = i - 1;
        while (j >= 0 && lp_wakes[j].delta > key.delta) {
          lp_wakes[j + 1] = lp_wakes[j];
          --j;
        }
        lp_wakes[j + 1] = key;
      }

      // Count unique LPs actually used.
      uint32_t avail_lp =
          *reinterpret_cast<const volatile uint32_t *>(0x7FFE03C0ull);
      int unique_lps = 0;
      bool lp_seen[256] = {};
      for (int i = 0; i < DIAG_N; ++i) {
        int lp_v = lp_wakes[i].lp;
        if (lp_v >= 0 && lp_v < 256 && !lp_seen[lp_v]) {
          lp_seen[lp_v] = true;
          ++unique_lps;
        }
      }
      write_str("    LP distribution: ");
      write_i64(unique_lps);
      write_str(" unique LPs used (");
      write_i64(avail_lp);
      write_str(" available)\n");

      // Show first 8 and last 8 (fastest/slowest).
      write_str("    Fastest 8:  ");
      for (int i = 0; i < 8 && i < DIAG_N; ++i) {
        if (i) write_str(" ");
        write_str("LP");
        write_i64(lp_wakes[i].lp);
        write_str(":");
        write_i64(lp_wakes[i].delta);
      }
      write_str("\n    Slowest 8:  ");
      for (int i = DIAG_N - 8; i < DIAG_N; ++i) {
        if (i > DIAG_N - 8) write_str(" ");
        write_str("LP");
        write_i64(lp_wakes[i].lp);
        write_str(":");
        write_i64(lp_wakes[i].delta);
      }
      write_str("\n");

      for (int t = 0; t < DIAG_N; ++t)
        join_thread(handles[t]);
    }

    // WoA diagnostic
    {
      struct WoaDiagCtx {
        cpp::Atomic<uint32_t> val{0};
        cpp::Atomic<int> arrived{0};
        cpp::Atomic<int> done{0};
      };
      auto woa_diag_waiter = +[](void *arg) {
        auto *ctx = static_cast<WoaDiagCtx *>(arg);
        int my_idx = ctx->arrived.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
        for (;;) {
          uint32_t v = ctx->val.load(cpp::MemoryOrder::ACQUIRE);
          if (v == 1) break;
          uint32_t expected = 0;
          ::RtlWaitOnAddress(&ctx->val.val, &expected, sizeof(expected), nullptr);
        }
        wake_ts[my_idx].store(now_ns(), cpp::MemoryOrder::RELAXED);
        ctx->done.fetch_add(1, cpp::MemoryOrder::RELEASE);
      };

      reset_lp();
      WoaDiagCtx ctx;
      ctx.val.store(0, cpp::MemoryOrder::RELAXED);
      ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
      ctx.done.store(0, cpp::MemoryOrder::RELAXED);
      for (int i = 0; i < DIAG_N; ++i)
        wake_ts[i].store(0, cpp::MemoryOrder::RELAXED);

      ThreadArg tas[DIAG_N];
      pthread_t handles[DIAG_N];
      for (int t = 0; t < DIAG_N; ++t)
        handles[t] = start_thread(woa_diag_waiter, &ctx, tas[t]);
      while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < DIAG_N)
        ;
      { LARGE_INTEGER d; d.QuadPart = -20000; ::NtDelayExecution(FALSE, &d); }

      int64_t t_pre = now_ns();
      ctx.val.store(1, cpp::MemoryOrder::RELEASE);
      ::RtlWakeAddressAll(&ctx.val.val);
      int64_t t_post = now_ns();
      while (ctx.done.load(cpp::MemoryOrder::ACQUIRE) < DIAG_N)
        ;
      int64_t t_all = now_ns();

      write_str("    WoA  WakeAddressAll call:   ");
      write_i64(t_post - t_pre); write_str(" ns\n");
      write_str("    WoA  all-done latency:      ");
      write_i64(t_all - t_pre); write_str(" ns\n");

      int64_t deltas[DIAG_N];
      for (int i = 0; i < DIAG_N; ++i)
        deltas[i] = wake_ts[i].load(cpp::MemoryOrder::RELAXED) - t_pre;
      insertion_sort(deltas, DIAG_N);

      write_str("    WoA  per-thread wake (ns): first=");
      write_i64(deltas[0]);
      write_str("  p25="); write_i64(deltas[DIAG_N / 4]);
      write_str("  med="); write_i64(deltas[DIAG_N / 2]);
      write_str("  p75="); write_i64(deltas[DIAG_N * 3 / 4]);
      write_str("  last="); write_i64(deltas[DIAG_N - 1]);
      write_str("\n");

      for (int t = 0; t < DIAG_N; ++t)
        join_thread(handles[t]);
    }
  }

  write_str("  Mutex contention:\n");

  bench_mutex_woa(timings, 8);
  report("WaitOnAddress mutex (8 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_rawmutex(timings, 8);
  report("RawMutex (8 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_woa(timings, 16);
  report("WaitOnAddress mutex (16 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_rawmutex(timings, 16);
  report("RawMutex (16 threads)", timings, HEAVY_SAMPLES);

  // --- K: Variable critical section length ---
  // The key question: does our spin-to-park transition behave correctly
  // for realistic critical section lengths? With 1ns CS (counter++),
  // we only test the degenerate case where the spin is always longer
  // than the CS and handoff dominates. Real mutexes protect real work.
  quiet_sleep_ns(50'000'000);
  write_str("\n[K] Variable-CS mutex — 50k ops total (ns, lower=better):\n");
  write_str("    (cs_ns = busy-wait inside critical section)\n");
  {
    static constexpr int VCS_ITERS = 75000;
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
          while (*pre) *p++ = *pre++;
          if (nt >= 10) *p++ = '0' + (nt / 10);
          *p++ = '0' + (nt % 10);
          *p++ = 'T'; *p++ = ')'; *p = '\0';
        };

        {
          bench_mutex_rawmutex_params(timings, nt, VCS_ITERS, cs_ns,
                                       HEAVY_SAMPLES);
          char name[64];
          make_name("RawMutex(", name);
          report(name, timings, HEAVY_SAMPLES);
          quiet_sleep_ns(50'000'000);
        }

        {
          bench_mutex_woa_params(timings, nt, VCS_ITERS, cs_ns,
                                  HEAVY_SAMPLES);
          char name[64];
          make_name("WoA     (", name);
          report(name, timings, HEAVY_SAMPLES);
          quiet_sleep_ns(50'000'000);
        }
      }
    }
  }

  // --- L: Oversubscription (32T mutex, 1ns CS) ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[L] Oversubscription — 32T mutex 50k ops:\n");
  {
    static constexpr int OS_ITERS = 75000;
    int64_t times[HEAVY_SAMPLES];

    bench_mutex_rawmutex_params(times, 32, OS_ITERS, 0, HEAVY_SAMPLES);
    report("RawMutex (32T)", times, HEAVY_SAMPLES);
    quiet_sleep_ns(50'000'000);

    bench_mutex_woa_params(times, 32, OS_ITERS, 0, HEAVY_SAMPLES);
    report("WoA      (32T)", times, HEAVY_SAMPLES);
    quiet_sleep_ns(50'000'000);
  }

  // --- M: Multiple independent futexes ---
  // Tests wait-slot pool and memory subsystem under parallel contention
  // on DIFFERENT futexes (no cross-futex interference expected).
  quiet_sleep_ns(50'000'000);
  write_str("\n[M] Independent futexes — 8 mutexes x 2T each (50k ops/mutex):\n");
  {
    static constexpr int MIF_MUTEXES = 8;
    static constexpr int MIF_ITERS = 75000;
    static constexpr int MIF_SAMPLES = 50;
    int64_t times[MIF_SAMPLES];

    bench_independent_mutex_rawmutex(times, MIF_MUTEXES, MIF_ITERS,
                                      MIF_SAMPLES);
    report("8 x RawMutex 2T (parallel)", times, MIF_SAMPLES);
    quiet_sleep_ns(50'000'000);

    // Baseline: same iter budget but only one mutex. Should be close
    // to the parallel case if there's no cross-bucket contention. 2T
    // window is short — use 200µs drain to avoid cache-cold dominance.
    bench_mutex_rawmutex_params(times, 2, MIF_ITERS, 0, MIF_SAMPLES, 200'000);
    report("1 x RawMutex 2T (baseline)", times, MIF_SAMPLES);
    quiet_sleep_ns(50'000'000);
  }

  // --- H: Stress test (race exposure) ---
  quiet_sleep_ns(50'000'000);
  write_str("\n[H] Contended stress test (8-pair, 50k iters):\n");
  stress_contended();

  // --- J: DLL cancel-race stress test ---
  // Exercises the specific race between waiter cancel and waker claim:
  //   Waiter: insert → unlock → value changed → cancel (lock → remove)
  //   Waker:  write value → lock → scan → CAS WAITING→SIGNALED → remove
  // With correct CAS-guarded cancel, only one side removes the node.
  // Without it, both remove → stale prev/next → DLL corruption → hang.
  quiet_sleep_ns(50'000'000);
  write_str("\n[J] DLL cancel-race stress test:\n");
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

      // Single-round run — correctness check on counter.
      RawMutexBenchShared ctx;
      ctx.lock.reset();
      ctx.counter = 0;
      ctx.iters = DLL_STRESS_ITERS / nt;
      ctx.cs_ns = 0;
      int64_t elapsed = fresh_run_mutex_style(nt, mutex_rawmutex_round, &ctx,
                                              instrument_enter,
                                              instrument_exit);

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
