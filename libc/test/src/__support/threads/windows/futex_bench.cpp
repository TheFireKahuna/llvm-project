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

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

// ---- Output (no CRT) ----

static HANDLE g_stdout;

static void write_str(const char *s) {
  const char *p = s;
  while (*p)
    ++p;
  test_support::write_handle(
      g_stdout, s, static_cast<DWORD>(p - s));
}


// int64 to decimal string, returns pointer into buf.
static char *i64_to_str(int64_t v, char *buf, int buflen) {
  char *end = buf + buflen - 1;
  *end = '\0';
  bool neg = v < 0;
  if (neg)
    v = -v;
  if (v == 0) {
    *--end = '0';
    return end;
  }
  while (v > 0) {
    *--end = '0' + static_cast<char>(v % 10);
    v /= 10;
  }
  if (neg)
    *--end = '-';
  return end;
}

static void write_i64(int64_t v) {
  char buf[24];
  write_str(i64_to_str(v, buf, sizeof(buf)));
}

// Right-pad name to width.
static void write_padded(const char *s, int width) {
  write_str(s);
  int len = 0;
  while (s[len])
    ++len;
  for (int i = len; i < width; ++i)
    write_str(" ");
}

// ---- Timing ----

static int64_t qpc_freq;

static int64_t now_ns() {
  LARGE_INTEGER qpc;
  ::RtlQueryPerformanceCounter(&qpc);
  return qpc.QuadPart * 1000000000LL / qpc_freq;
}

static void init_timer() {
  LARGE_INTEGER freq;
  ::RtlQueryPerformanceFrequency(&freq);
  qpc_freq = freq.QuadPart;
}

// ---- Sort ----

static void insertion_sort(int64_t *a, int n) {
  for (int i = 1; i < n; ++i) {
    int64_t key = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > key) {
      a[j + 1] = a[j];
      --j;
    }
    a[j + 1] = key;
  }
}

// ---- Affinity / priority ----

// Pin the current thread to a specific logical processor.
// Default: core 2 (avoids LP 0/1 which share physical core 0 and
// handle most OS DPCs / interrupts — keeps benchmark threads quiet).
static void pin_thread(HANDLE thread, int logical_processor) {
  ULONG_PTR mask = 1ull << logical_processor;
  ::NtSetInformationThread(thread, 4 /*ThreadAffinityMask*/,
                           &mask, sizeof(mask));
}

// Boost process to HIGH priority class to reduce preemption noise.
static void boost_priority() {
  struct { BOOLEAN Foreground; UCHAR PriorityClass; } ppc = {FALSE, 3};
  ::NtSetInformationProcess(NtCurrentProcess(), 18 /*ProcessPriorityClass*/,
                            &ppc, sizeof(ppc));
}

// ---- Thread helper (pthread) ----

// Next logical processor to assign. Starts at LP 4 (main is on LP 2;
// LP 2/3 share a physical core, so workers start at LP 4 = core 2).
static cpp::Atomic<int> next_lp{4};

struct ThreadArg {
  void (*func)(void *);
  void *ctx;
  int lp; // logical processor to pin to
};

static void *thread_trampoline(void *arg) {
  auto *ta = static_cast<ThreadArg *>(arg);
  pin_thread(NtCurrentThread(), ta->lp);
  ta->func(ta->ctx);
  return nullptr;
}

static pthread_t start_thread(void (*func)(void *), void *ctx, ThreadArg &arg) {
  arg.func = func;
  arg.ctx = ctx;
  arg.lp = next_lp.fetch_add(1, cpp::MemoryOrder::RELAXED);
  pthread_t tid;
  pthread_create(&tid, nullptr, thread_trampoline, &arg);
  return tid;
}

// Reset LP counter between benchmarks so threads reuse the same cores.
// Starts at LP 4 — main on LP 2, LP 3 is its SMT sibling.
static void reset_lp() {
  next_lp.store(4, cpp::MemoryOrder::RELAXED);
}

static void join_thread(pthread_t tid) {
  pthread_join(tid, nullptr);
}

// ---- Constants ----

static constexpr int WARMUP = 1000;
static constexpr int ITERS = 100000;
static constexpr int SAMPLES = 1000;
// Reduced sample count for tests that spawn threads per sample.
static constexpr int HEAVY_SAMPLES = 100;

// ============================================================
// Bench 1: WaitOnAddress / WakeByAddressSingle
// ============================================================

struct WoaPP {
  cpp::Atomic<uint32_t> val{0};
  cpp::Atomic<bool> ready{false};
};

static void woa_responder(void *arg) {
  auto *pp = static_cast<WoaPP *>(arg);
  pp->ready.store(true, cpp::MemoryOrder::RELEASE);
  for (int i = 0; i < WARMUP + ITERS; ++i) {
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

  int s = 0;
  for (int i = -WARMUP; i < ITERS; ++i) {
    int64_t t0 = 0;
    bool sampling = (i >= ITERS - SAMPLES);
    if (sampling)
      t0 = now_ns();

    pp.val.store(1, cpp::MemoryOrder::RELEASE);
    ::RtlWakeAddressSingle(&pp.val.val);

    for (;;) {
      uint32_t v = pp.val.load(cpp::MemoryOrder::ACQUIRE);
      if (v == 0)
        break;
      uint32_t expected = 1;
      ::RtlWaitOnAddress(&pp.val.val, &expected, sizeof(expected), nullptr);
    }

    if (sampling)
      timings[s++] = now_ns() - t0;
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

static void futex_responder(void *arg) {
  auto *pp = static_cast<FutexPP *>(arg);
  pp->ready.store(true, cpp::MemoryOrder::RELEASE);
  for (int i = 0; i < WARMUP + ITERS; ++i) {
    while (pp->val.load(cpp::MemoryOrder::ACQUIRE) != 1)
      pp->val.wait(0);
    pp->val.store(0, cpp::MemoryOrder::RELEASE);
    pp->val.notify_one();
  }
}

static void bench_futex(int64_t *timings) {
  reset_lp();
  FutexPP pp;
  ThreadArg ta;
  pthread_t h = start_thread(futex_responder, &pp, ta);
  while (!pp.ready.load(cpp::MemoryOrder::ACQUIRE))
    ;

  int s = 0;
  for (int i = -WARMUP; i < ITERS; ++i) {
    int64_t t0 = 0;
    bool sampling = (i >= ITERS - SAMPLES);
    if (sampling)
      t0 = now_ns();

    pp.val.store(1, cpp::MemoryOrder::RELEASE);
    pp.val.notify_one();

    while (pp.val.load(cpp::MemoryOrder::ACQUIRE) != 0)
      pp.val.wait(1);

    if (sampling)
      timings[s++] = now_ns() - t0;
  }
  join_thread(h);
}

// ============================================================
// Bench 3: Futex::store_and_notify
// ============================================================

static void futex_saw_responder(void *arg) {
  auto *pp = static_cast<FutexPP *>(arg);
  pp->ready.store(true, cpp::MemoryOrder::RELEASE);
  for (int i = 0; i < WARMUP + ITERS; ++i) {
    while (pp->val.load(cpp::MemoryOrder::ACQUIRE) != 1)
      pp->val.wait(0);
    pp->val.store_and_notify(0);
  }
}

static void bench_futex_saw(int64_t *timings) {
  reset_lp();
  FutexPP pp;
  ThreadArg ta;
  pthread_t h = start_thread(futex_saw_responder, &pp, ta);
  while (!pp.ready.load(cpp::MemoryOrder::ACQUIRE))
    ;

  int s = 0;
  for (int i = -WARMUP; i < ITERS; ++i) {
    int64_t t0 = 0;
    bool sampling = (i >= ITERS - SAMPLES);
    if (sampling)
      t0 = now_ns();

    pp.val.store_and_notify(1);

    while (pp.val.load(cpp::MemoryOrder::ACQUIRE) != 0)
      pp.val.wait(1);

    if (sampling)
      timings[s++] = now_ns() - t0;
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
  for (int i = 0; i < WARMUP + ITERS; ++i) {
    while (ctx->val.load(cpp::MemoryOrder::ACQUIRE) != 1)
      ctx->val.wait(0);
    ctx->val.store_and_notify(0);
  }
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

  int s = 0;
  for (int i = -WARMUP; i < ITERS; ++i) {
    int64_t t0 = 0;
    bool sampling = (i >= ITERS - SAMPLES);
    if (sampling)
      t0 = now_ns();

    for (int p = 0; p < NUM_PAIRS; ++p)
      ctx[p].val.store_and_notify(1);
    for (int p = 0; p < NUM_PAIRS; ++p)
      while (ctx[p].val.load(cpp::MemoryOrder::ACQUIRE) != 0)
        ctx[p].val.wait(1);

    if (sampling)
      timings[s++] = now_ns() - t0;
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
static constexpr int STRESS_ITERS = 50000;

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
  for (int i = 0; i < WARMUP + ITERS; ++i) {
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

  int s = 0;
  for (int i = -WARMUP; i < ITERS; ++i) {
    int64_t t0 = 0;
    bool sampling = (i >= ITERS - SAMPLES);
    if (sampling)
      t0 = now_ns();

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
        ::RtlWaitOnAddress(&ctx[p].val.val, &expected, sizeof(expected),
                           nullptr);
      }
    }

    if (sampling)
      timings[s++] = now_ns() - t0;
  }
  for (int p = 0; p < NUM_PAIRS; ++p)
    join_thread(handles[p]);
}

// ============================================================
// Bench 6: Uncontended wake — Futex (live_count fast path)
// ============================================================

static void bench_wake_empty_futex(int64_t *timings) {
  Futex f{0};
  for (int i = -WARMUP; i < SAMPLES; ++i) {
    int64_t t0 = now_ns();
    f.notify_one();
    int64_t t1 = now_ns();
    if (i >= 0)
      timings[i] = t1 - t0;
  }
}

// ============================================================
// Bench 7: Uncontended wake — WaitOnAddress
// ============================================================

static void bench_wake_empty_woa(int64_t *timings) {
  cpp::Atomic<uint32_t> val{0};
  for (int i = -WARMUP; i < SAMPLES; ++i) {
    int64_t t0 = now_ns();
    ::RtlWakeAddressSingle(&val.val);
    int64_t t1 = now_ns();
    if (i >= 0)
      timings[i] = t1 - t0;
  }
}

// ============================================================
// Bench 8: Timeout return — Futex (lock-free timeout)
// ============================================================

static Futex::Timeout make_immediate_timeout() {
  struct timespec ts = {0, 1}; // 1 ns — effectively immediate
  return *internal::AbsTimeout::from_timespec(ts, /*is_realtime=*/false);
}

static void bench_timeout_futex(int64_t *timings) {
  Futex f{42};
  // Fewer iterations — the Futex fast path skips the kernel entirely
  // for expired timeouts, but we keep it comparable with the WoA test.
  for (int i = -10; i < HEAVY_SAMPLES; ++i) {
    auto to = make_immediate_timeout();
    int64_t t0 = now_ns();
    f.wait(42, {to});
    int64_t t1 = now_ns();
    if (i >= 0)
      timings[i] = t1 - t0;
  }
}

// ============================================================
// Bench 9: Timeout return — WaitOnAddress
// ============================================================

static void bench_timeout_woa(int64_t *timings) {
  cpp::Atomic<uint32_t> val{42};
  // RtlWaitOnAddress with -1 timeout hits the kernel timer tick (~15.6ms
  // per call), so use fewer iterations to keep the benchmark reasonable.
  for (int i = -10; i < HEAVY_SAMPLES; ++i) {
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

struct FetchAddCtx {
  Futex *target;
  int iters;
  cpp::Atomic<bool> ready{false};
  cpp::Atomic<bool> go{false};
};

static void fetch_add_worker(void *arg) {
  auto *ctx = static_cast<FetchAddCtx *>(arg);
  ctx->ready.store(true, cpp::MemoryOrder::RELEASE);
  while (!ctx->go.load(cpp::MemoryOrder::ACQUIRE))
    ;
  for (int i = 0; i < ctx->iters; ++i)
    ctx->target->fetch_add(1, cpp::MemoryOrder::RELAXED);
}

static void bench_fetch_add(int64_t *timings, int num_threads) {
  static constexpr int FETCH_ITERS = 1000000;
  Futex counter{0};

  for (int s = 0; s < HEAVY_SAMPLES; ++s) {
    reset_lp();
    counter.set(0);

    FetchAddCtx ctxs[8]; // max 8 threads
    ThreadArg tas[8];
    pthread_t handles[8];

    for (int t = 0; t < num_threads; ++t) {
      ctxs[t].target = &counter;
      ctxs[t].iters = FETCH_ITERS / num_threads;
      ctxs[t].ready.store(false, cpp::MemoryOrder::RELAXED);
      ctxs[t].go.store(false, cpp::MemoryOrder::RELAXED);
      handles[t] = start_thread(fetch_add_worker, &ctxs[t], tas[t]);
    }
    for (int t = 0; t < num_threads; ++t)
      while (!ctxs[t].ready.load(cpp::MemoryOrder::ACQUIRE))
        ;

    int64_t t0 = now_ns();
    for (int t = 0; t < num_threads; ++t)
      ctxs[t].go.store(true, cpp::MemoryOrder::RELEASE);
    for (int t = 0; t < num_threads; ++t)
      join_thread(handles[t]);
    timings[s] = now_ns() - t0;
  }
}

// ============================================================
// Bench 11: fetch_add throughput — raw InterlockedAdd64
// ============================================================

struct RawFetchAddCtx {
  volatile int64_t *target;
  int iters;
  cpp::Atomic<bool> ready{false};
  cpp::Atomic<bool> go{false};
};

static void raw_fetch_add_worker(void *arg) {
  auto *ctx = static_cast<RawFetchAddCtx *>(arg);
  ctx->ready.store(true, cpp::MemoryOrder::RELEASE);
  while (!ctx->go.load(cpp::MemoryOrder::ACQUIRE))
    ;
  for (int i = 0; i < ctx->iters; ++i)
    __atomic_fetch_add(ctx->target, 1, __ATOMIC_RELAXED);
}

static void bench_raw_fetch_add(int64_t *timings, int num_threads) {
  static constexpr int FETCH_ITERS = 1000000;
  volatile int64_t counter = 0;

  for (int s = 0; s < HEAVY_SAMPLES; ++s) {
    reset_lp();
    counter = 0;

    RawFetchAddCtx ctxs[8];
    ThreadArg tas[8];
    pthread_t handles[8];

    for (int t = 0; t < num_threads; ++t) {
      ctxs[t].target = &counter;
      ctxs[t].iters = FETCH_ITERS / num_threads;
      ctxs[t].ready.store(false, cpp::MemoryOrder::RELAXED);
      ctxs[t].go.store(false, cpp::MemoryOrder::RELAXED);
      handles[t] = start_thread(raw_fetch_add_worker, &ctxs[t], tas[t]);
    }
    for (int t = 0; t < num_threads; ++t)
      while (!ctxs[t].ready.load(cpp::MemoryOrder::ACQUIRE))
        ;

    int64_t t0 = now_ns();
    for (int t = 0; t < num_threads; ++t)
      ctxs[t].go.store(true, cpp::MemoryOrder::RELEASE);
    for (int t = 0; t < num_threads; ++t)
      join_thread(handles[t]);
    timings[s] = now_ns() - t0;
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

static void fanout_waiter(void *arg) {
  auto *ctx = static_cast<FanOutCtx *>(arg);
  ctx->arrived.fetch_add(1, cpp::MemoryOrder::RELEASE);
  while (ctx->futex->load(cpp::MemoryOrder::ACQUIRE) == 0)
    ctx->futex->wait(0);
  ctx->done.fetch_add(1, cpp::MemoryOrder::RELEASE);
}

static void bench_fanout_futex(int64_t *timings, int num_waiters) {
  for (int s = 0; s < HEAVY_SAMPLES; ++s) {
    reset_lp();
    Futex f{0};
    FanOutCtx ctx;
    ctx.futex = &f;
    ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
    ctx.done.store(0, cpp::MemoryOrder::RELAXED);

    ThreadArg tas[64];
    pthread_t handles[64];
    for (int t = 0; t < num_waiters; ++t)
      handles[t] = start_thread(fanout_waiter, &ctx, tas[t]);

    while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < num_waiters)
      ;
    // Brief delay to let all threads reach kernel sleep.
    {
      LARGE_INTEGER delay;
      delay.QuadPart = -10000; // 1ms
      ::NtDelayExecution(FALSE, &delay);
    }

    int64_t t0 = now_ns();
    f.store_and_notify_all(1);
    while (ctx.done.load(cpp::MemoryOrder::ACQUIRE) < num_waiters)
      ;
    timings[s] = now_ns() - t0;

    for (int t = 0; t < num_waiters; ++t)
      join_thread(handles[t]);
  }
}

// Variant with explicit chunk size and yield control for A/B testing.
static void bench_fanout_futex_chunked(int64_t *timings, int num_waiters,
                                        uint32_t chunk_size,
                                        bool do_yield = true) {
  for (int s = 0; s < HEAVY_SAMPLES; ++s) {
    reset_lp();
    Futex f{0};
    FanOutCtx ctx;
    ctx.futex = &f;
    ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
    ctx.done.store(0, cpp::MemoryOrder::RELAXED);

    ThreadArg tas[64];
    pthread_t handles[64];
    for (int t = 0; t < num_waiters; ++t)
      handles[t] = start_thread(fanout_waiter, &ctx, tas[t]);

    while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < num_waiters)
      ;
    {
      LARGE_INTEGER delay;
      delay.QuadPart = -10000;
      ::NtDelayExecution(FALSE, &delay);
    }

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

static void woa_fanout_waiter(void *arg) {
  auto *ctx = static_cast<WoaFanOutCtx *>(arg);
  ctx->arrived.fetch_add(1, cpp::MemoryOrder::RELEASE);
  for (;;) {
    uint32_t v = ctx->val.load(cpp::MemoryOrder::ACQUIRE);
    if (v == 1)
      break;
    uint32_t expected = 0;
    ::RtlWaitOnAddress(&ctx->val.val, &expected, sizeof(expected), nullptr);
  }
  ctx->done.fetch_add(1, cpp::MemoryOrder::RELEASE);
}

static void bench_fanout_woa(int64_t *timings, int num_waiters) {
  for (int s = 0; s < HEAVY_SAMPLES; ++s) {
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
    {
      LARGE_INTEGER delay;
      delay.QuadPart = -10000;
      ::NtDelayExecution(FALSE, &delay);
    }

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

struct MutexBenchShared {
  Futex lock{0}; // 0 = unlocked, 1 = locked
  volatile int64_t counter;
  int iters;
  cpp::Atomic<int> arrived{0};
  cpp::Atomic<bool> go{false};
};

static void mutex_futex_worker(void *arg) {
  auto *ctx = static_cast<MutexBenchShared *>(arg);
  ctx->arrived.fetch_add(1, cpp::MemoryOrder::RELEASE);
  while (!ctx->go.load(cpp::MemoryOrder::ACQUIRE))
    ;
  for (int i = 0; i < ctx->iters; ++i) {
    // Lock: mirrors RawMutex::lock_slow with handoff support.
    for (;;) {
      FutexValueType expected = 0;
      if (ctx->lock.compare_exchange_weak(expected, 1u,
                                          cpp::MemoryOrder::ACQUIRE))
        break;
      long ret = ctx->lock.wait(1);
      if (ret == 1) // handoff — we own the lock
        break;
    }
    ctx->counter++;
    // Unlock: mirrors RawMutex::unlock — single scan, handoff or Dekker.
    ctx->lock.unlock_notify(0);
  }
}

static void bench_mutex_futex(int64_t *timings, int num_threads) {
  static constexpr int MUTEX_ITERS = 100000;

  for (int s = 0; s < HEAVY_SAMPLES; ++s) {
    reset_lp();
    MutexBenchShared ctx;
    ctx.lock.set(0);
    ctx.counter = 0;
    ctx.iters = MUTEX_ITERS / num_threads;
    ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
    ctx.go.store(false, cpp::MemoryOrder::RELAXED);

    ThreadArg tas[16];
    pthread_t handles[16];
    for (int t = 0; t < num_threads; ++t)
      handles[t] = start_thread(mutex_futex_worker, &ctx, tas[t]);
    while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < num_threads)
      ;

    int64_t t0 = now_ns();
    ctx.go.store(true, cpp::MemoryOrder::RELEASE);
    for (int t = 0; t < num_threads; ++t)
      join_thread(handles[t]);
    timings[s] = now_ns() - t0;
  }
}

// ============================================================
// Bench 15: Mutex-style acquire/release — WaitOnAddress
// ============================================================

struct WoaMutexShared {
  cpp::Atomic<uint32_t> lock{0};
  volatile int64_t counter;
  int iters;
  cpp::Atomic<int> arrived{0};
  cpp::Atomic<bool> go{false};
};

static void mutex_woa_worker(void *arg) {
  auto *ctx = static_cast<WoaMutexShared *>(arg);
  ctx->arrived.fetch_add(1, cpp::MemoryOrder::RELEASE);
  while (!ctx->go.load(cpp::MemoryOrder::ACQUIRE))
    ;
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
    ctx->counter++;
    ctx->lock.store(0, cpp::MemoryOrder::RELEASE);
    ::RtlWakeAddressSingle(&ctx->lock.val);
  }
}

static void bench_mutex_woa(int64_t *timings, int num_threads) {
  static constexpr int MUTEX_ITERS = 100000;

  for (int s = 0; s < HEAVY_SAMPLES; ++s) {
    reset_lp();
    WoaMutexShared ctx;
    ctx.lock.store(0, cpp::MemoryOrder::RELAXED);
    ctx.counter = 0;
    ctx.iters = MUTEX_ITERS / num_threads;
    ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
    ctx.go.store(false, cpp::MemoryOrder::RELAXED);

    ThreadArg tas[16];
    pthread_t handles[16];
    for (int t = 0; t < num_threads; ++t)
      handles[t] = start_thread(mutex_woa_worker, &ctx, tas[t]);
    while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < num_threads)
      ;

    int64_t t0 = now_ns();
    ctx.go.store(true, cpp::MemoryOrder::RELEASE);
    for (int t = 0; t < num_threads; ++t)
      join_thread(handles[t]);
    timings[s] = now_ns() - t0;
  }
}

// ============================================================
// Report
// ============================================================

static void report(const char *name, int64_t *timings, int n) {
  insertion_sort(timings, n);
  // Trimmed mean: exclude bottom/top 10% to remove outliers.
  int lo = n / 10, hi = n - n / 10;
  int64_t sum = 0;
  for (int i = lo; i < hi; ++i)
    sum += timings[i];
  int64_t tmean = (hi > lo) ? sum / (hi - lo) : timings[n / 2];

  write_str("  ");
  write_padded(name, 38);
  write_str("avg=");
  write_i64(tmean);
  write_str("  med=");
  write_i64(timings[n / 2]);
  write_str("  p99=");
  write_i64(timings[n * 99 / 100]);
  write_str("  min=");
  write_i64(timings[0]);
  write_str(" ns\n");
}

} // namespace LIBC_NAMESPACE_DECL

// ============================================================
// Entry
// ============================================================

static int bench_main() {
  using namespace LIBC_NAMESPACE;

  g_stdout = ::NtCurrentStandardOutput();
  init_timer();

  // Reduce scheduling noise: HIGH priority + pin main thread to LP 2.
  boost_priority();
  pin_thread(NtCurrentThread(), 2);

  write_str("=== Futex comprehensive benchmark ===\n");
  write_str("    Ping-pong: 100k iters, 1k samples. Heavy: 100 samples.\n");
  write_str("    Pinned to LP 2+, HIGH priority class.\n");
  write_str("    kuser_spin_threshold=");
  write_i64(spin_wait::kuser_spin_threshold());
  write_str("  backend=");
  write_i64(static_cast<int>(spin_wait::get_backend()));
  write_str(" (0=RELAX 1=UMWAIT 2=MWAITX)\n\n");

  static int64_t timings[SAMPLES];

  // --- A: Ping-pong latency ---
  write_str("[A] Single-pair ping-pong round-trip (ns):\n");

  bench_woa(timings);
  report("WaitOnAddress/WakeByAddressSingle", timings, SAMPLES);

  bench_futex(timings);
  report("Futex::wait/notify_one", timings, SAMPLES);

  bench_futex_saw(timings);
  report("Futex::store_and_notify", timings, SAMPLES);

  write_str("\n[A] 4-pair contended ping-pong (ns):\n");

  bench_woa_contended(timings);
  report("WaitOnAddress (4-pair)", timings, SAMPLES);

  bench_contended(timings);
  report("Futex::store_and_notify (4-pair)", timings, SAMPLES);

  // --- B: Uncontended wake ---
  write_str("\n[B] Uncontended wake — no waiters (ns):\n");

  bench_wake_empty_woa(timings);
  report("RtlWakeAddressSingle (empty)", timings, SAMPLES);

  bench_wake_empty_futex(timings);
  report("Futex::notify_one (empty)", timings, SAMPLES);

  // --- C: Timeout return ---
  write_str("\n[C] Timeout return latency (ns):\n");

  bench_timeout_woa(timings);
  report("RtlWaitOnAddress (timeout)", timings, HEAVY_SAMPLES);

  bench_timeout_futex(timings);
  report("Futex::wait (lock-free timeout)", timings, HEAVY_SAMPLES);

  // --- D: Atomic RMW throughput ---
  write_str("\n[D] fetch_add 1M ops total time (ns, lower=better):\n");

  bench_raw_fetch_add(timings, 1);
  report("raw lock xadd (1 thread)", timings, HEAVY_SAMPLES);

  bench_fetch_add(timings, 1);
  report("Futex::fetch_add (1 thread)", timings, HEAVY_SAMPLES);

  bench_raw_fetch_add(timings, 4);
  report("raw lock xadd (4 threads)", timings, HEAVY_SAMPLES);

  bench_fetch_add(timings, 4);
  report("Futex::fetch_add (4 threads)", timings, HEAVY_SAMPLES);

  // --- E: notify_all fan-out ---
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
  write_str("\n[F] Mutex 100k acquire/release total time (ns):\n");

  bench_mutex_woa(timings, 2);
  report("WaitOnAddress mutex (2 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_futex(timings, 2);
  report("Futex mutex (2 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_woa(timings, 4);
  report("WaitOnAddress mutex (4 threads)", timings, HEAVY_SAMPLES);
  // --- G: High thread count ---
  write_str("\n[G] High thread count:\n");

  write_str("  notify_all fan-out:\n");

  bench_fanout_woa(timings, 32);
  report("RtlWakeAddressAll (32 waiters)", timings, HEAVY_SAMPLES);

  bench_fanout_futex(timings, 32);
  report("Futex::store_and_notify_all (32)", timings, HEAVY_SAMPLES);

  bench_fanout_woa(timings, 64);
  report("RtlWakeAddressAll (64 waiters)", timings, HEAVY_SAMPLES);

  bench_fanout_futex(timings, 64);
  report("Futex::store_and_notify_all (64)", timings, HEAVY_SAMPLES);

  // --- Chunk size reference (64 waiters) ---
  // Retained for future A/B testing. Production notify_all uses
  // single-pass (no chunking) since chunking + yield introduces
  // quantum-delay tails from NtYieldExecution.
  write_str("\n  Chunk reference (64 waiters):\n");
  {
    uint32_t lp = *reinterpret_cast<const volatile uint32_t *>(0x7FFE03C0ull);
    write_str("    LP count=");
    write_i64(lp);
    write_str("\n");

    // Production: single-pass all-at-once (matches notify_all)
    bench_fanout_futex_chunked(timings, 64, 64, false);
    report("single-pass (production)", timings, HEAVY_SAMPLES);

    // Reference: LP-chunked + yield (previous approach)
    bench_fanout_futex_chunked(timings, 64, lp, true);
    { char n[64]; char *p = n;
      const char *s = "LP-chunked+yield ("; while(*s)*p++=*s++;
      if(lp>=10)*p++='0'+(lp/10); *p++='0'+(lp%10);
      *p++=')'; *p='\0';
      report(n, timings, HEAVY_SAMPLES); }
  }

  // --- Diagnostic: 64-waiter wake latency breakdown ---
  write_str("\n  64-waiter wake breakdown (single run):\n");
  {
    static constexpr int DIAG_N = 64;
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

  bench_mutex_futex(timings, 8);
  report("Futex mutex (8 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_woa(timings, 16);
  report("WaitOnAddress mutex (16 threads)", timings, HEAVY_SAMPLES);

  bench_mutex_futex(timings, 16);
  report("Futex mutex (16 threads)", timings, HEAVY_SAMPLES);

  // --- K: Variable critical section length ---
  // The key question: does our spin-to-park transition behave correctly
  // for realistic critical section lengths? With 1ns CS (counter++),
  // we only test the degenerate case where the spin is always longer
  // than the CS and handoff dominates. Real mutexes protect real work.
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

        // --- Futex mutex ---
        {
          int64_t times[HEAVY_SAMPLES];
          for (int s = 0; s < HEAVY_SAMPLES; ++s) {
            reset_lp();
            struct VCSCtx {
              Futex lock{0};
              volatile int64_t counter;
              int iters;
              int cs_ns;
              cpp::Atomic<int> arrived{0};
              cpp::Atomic<bool> go{false};
            };
            auto vcs_worker = +[](void *arg) {
              auto *c = static_cast<VCSCtx *>(arg);
              c->arrived.fetch_add(1, cpp::MemoryOrder::RELEASE);
              while (!c->go.load(cpp::MemoryOrder::ACQUIRE))
                ;
              for (int i = 0; i < c->iters; ++i) {
                for (;;) {
                  FutexValueType exp = 0;
                  if (c->lock.compare_exchange_weak(
                          exp, 1u, cpp::MemoryOrder::ACQUIRE))
                    break;
                  long ret = c->lock.wait(1);
                  if (ret == 1)
                    break;
                }
                // Busy-wait critical section.
                int64_t t0 = now_ns();
                while (now_ns() - t0 < c->cs_ns)
                  ;
                c->counter++;
                c->lock.unlock_notify(0);
              }
            };

            VCSCtx ctx;
            ctx.lock.set(0);
            ctx.counter = 0;
            ctx.iters = VCS_ITERS / nt;
            ctx.cs_ns = cs_ns;
            ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
            ctx.go.store(false, cpp::MemoryOrder::RELAXED);

            ThreadArg tas[16];
            pthread_t handles[16];
            for (int t = 0; t < nt; ++t)
              handles[t] = start_thread(vcs_worker, &ctx, tas[t]);
            while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < nt)
              ;
            int64_t t0 = now_ns();
            ctx.go.store(true, cpp::MemoryOrder::RELEASE);
            for (int t = 0; t < nt; ++t)
              join_thread(handles[t]);
            times[s] = now_ns() - t0;
          }
          char name[64];
          char *p = name;
          const char *pre = "Futex (";
          while (*pre) *p++ = *pre++;
          if (nt >= 10) *p++ = '0' + (nt / 10);
          *p++ = '0' + (nt % 10);
          const char *suf = "T)";
          while (*suf) *p++ = *suf++;
          *p = '\0';
          report(name, times, HEAVY_SAMPLES);
        }

        // --- WoA baseline ---
        {
          int64_t times[HEAVY_SAMPLES];
          for (int s = 0; s < HEAVY_SAMPLES; ++s) {
            reset_lp();
            struct WoaVCSCtx {
              cpp::Atomic<uint32_t> lock{0};
              volatile int64_t counter;
              int iters;
              int cs_ns;
              cpp::Atomic<int> arrived{0};
              cpp::Atomic<bool> go{false};
            };
            auto woa_worker = +[](void *arg) {
              auto *c = static_cast<WoaVCSCtx *>(arg);
              c->arrived.fetch_add(1, cpp::MemoryOrder::RELEASE);
              while (!c->go.load(cpp::MemoryOrder::ACQUIRE))
                ;
              for (int i = 0; i < c->iters; ++i) {
                for (;;) {
                  uint32_t exp = 0;
                  if (c->lock.compare_exchange_weak(
                          exp, 1u, cpp::MemoryOrder::ACQUIRE,
                          cpp::MemoryOrder::RELAXED))
                    break;
                  uint32_t one = 1;
                  ::RtlWaitOnAddress(&c->lock.val, &one,
                                     sizeof(one), nullptr);
                }
                int64_t t0 = now_ns();
                while (now_ns() - t0 < c->cs_ns)
                  ;
                c->counter++;
                c->lock.store(0, cpp::MemoryOrder::RELEASE);
                ::RtlWakeAddressSingle(&c->lock.val);
              }
            };

            WoaVCSCtx ctx;
            ctx.lock.store(0, cpp::MemoryOrder::RELAXED);
            ctx.counter = 0;
            ctx.iters = VCS_ITERS / nt;
            ctx.cs_ns = cs_ns;
            ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
            ctx.go.store(false, cpp::MemoryOrder::RELAXED);

            ThreadArg tas[16];
            pthread_t handles[16];
            for (int t = 0; t < nt; ++t)
              handles[t] = start_thread(woa_worker, &ctx, tas[t]);
            while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < nt)
              ;
            int64_t t0 = now_ns();
            ctx.go.store(true, cpp::MemoryOrder::RELEASE);
            for (int t = 0; t < nt; ++t)
              join_thread(handles[t]);
            times[s] = now_ns() - t0;
          }
          char name[64];
          char *p = name;
          const char *pre = "WoA   (";
          while (*pre) *p++ = *pre++;
          if (nt >= 10) *p++ = '0' + (nt / 10);
          *p++ = '0' + (nt % 10);
          const char *suf = "T)";
          while (*suf) *p++ = *suf++;
          *p = '\0';
          report(name, times, HEAVY_SAMPLES);
        }
      }
    }
  }

  // --- L: Oversubscription (32T mutex, 1ns CS) ---
  write_str("\n[L] Oversubscription — 32T mutex 50k ops:\n");
  {
    static constexpr int OS_ITERS = 50000;
    int64_t times[HEAVY_SAMPLES];

    for (int s = 0; s < HEAVY_SAMPLES; ++s) {
      reset_lp();
      MutexBenchShared ctx;
      ctx.lock.set(0);
      ctx.counter = 0;
      ctx.iters = OS_ITERS / 32;
      ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
      ctx.go.store(false, cpp::MemoryOrder::RELAXED);

      ThreadArg tas[32];
      pthread_t handles[32];
      for (int t = 0; t < 32; ++t)
        handles[t] = start_thread(mutex_futex_worker, &ctx, tas[t]);
      while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < 32)
        ;
      int64_t t0 = now_ns();
      ctx.go.store(true, cpp::MemoryOrder::RELEASE);
      for (int t = 0; t < 32; ++t)
        join_thread(handles[t]);
      times[s] = now_ns() - t0;
    }
    report("Futex mutex (32T)", times, HEAVY_SAMPLES);

    for (int s = 0; s < HEAVY_SAMPLES; ++s) {
      reset_lp();
      WoaMutexShared ctx;
      ctx.lock.store(0, cpp::MemoryOrder::RELAXED);
      ctx.counter = 0;
      ctx.iters = OS_ITERS / 32;
      ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
      ctx.go.store(false, cpp::MemoryOrder::RELAXED);

      ThreadArg tas[32];
      pthread_t handles[32];
      for (int t = 0; t < 32; ++t)
        handles[t] = start_thread(mutex_woa_worker, &ctx, tas[t]);
      while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < 32)
        ;
      int64_t t0 = now_ns();
      ctx.go.store(true, cpp::MemoryOrder::RELEASE);
      for (int t = 0; t < 32; ++t)
        join_thread(handles[t]);
      times[s] = now_ns() - t0;
    }
    report("WoA mutex   (32T)", times, HEAVY_SAMPLES);
  }

  // --- M: Multiple independent futexes ---
  // Tests wait-slot pool and memory subsystem under parallel contention
  // on DIFFERENT futexes (no cross-futex interference expected).
  write_str("\n[M] Independent futexes — 8 mutexes x 2T each (50k ops/mutex):\n");
  {
    static constexpr int MIF_MUTEXES = 8;
    static constexpr int MIF_ITERS = 50000;
    int64_t times[HEAVY_SAMPLES];

    for (int s = 0; s < HEAVY_SAMPLES; ++s) {
      reset_lp();
      MutexBenchShared ctxs[MIF_MUTEXES];
      ThreadArg tas[MIF_MUTEXES * 2];
      pthread_t handles[MIF_MUTEXES * 2];

      for (int m = 0; m < MIF_MUTEXES; ++m) {
        ctxs[m].lock.set(0);
        ctxs[m].counter = 0;
        ctxs[m].iters = MIF_ITERS / 2;
        ctxs[m].arrived.store(0, cpp::MemoryOrder::RELAXED);
        ctxs[m].go.store(false, cpp::MemoryOrder::RELAXED);
        for (int t = 0; t < 2; ++t)
          handles[m * 2 + t] = start_thread(
              mutex_futex_worker, &ctxs[m], tas[m * 2 + t]);
      }
      for (int m = 0; m < MIF_MUTEXES; ++m)
        while (ctxs[m].arrived.load(cpp::MemoryOrder::ACQUIRE) < 2)
          ;

      int64_t t0 = now_ns();
      for (int m = 0; m < MIF_MUTEXES; ++m)
        ctxs[m].go.store(true, cpp::MemoryOrder::RELEASE);
      for (int i = 0; i < MIF_MUTEXES * 2; ++i)
        join_thread(handles[i]);
      times[s] = now_ns() - t0;
    }
    report("8 x Futex 2T (parallel)", times, HEAVY_SAMPLES);

    // Expected: should be close to single-mutex 2T time (no cross-interference).
    // If significantly slower, the wait-slot pool or bucket contention is the issue.
    for (int s = 0; s < HEAVY_SAMPLES; ++s) {
      reset_lp();
      MutexBenchShared ctx;
      ctx.lock.set(0);
      ctx.counter = 0;
      ctx.iters = MIF_ITERS / 2;
      ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
      ctx.go.store(false, cpp::MemoryOrder::RELAXED);

      ThreadArg tas[2];
      pthread_t handles[2];
      for (int t = 0; t < 2; ++t)
        handles[t] = start_thread(mutex_futex_worker, &ctx, tas[t]);
      while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < 2)
        ;
      int64_t t0 = now_ns();
      ctx.go.store(true, cpp::MemoryOrder::RELEASE);
      for (int t = 0; t < 2; ++t)
        join_thread(handles[t]);
      times[s] = now_ns() - t0;
    }
    report("1 x Futex 2T (baseline)", times, HEAVY_SAMPLES);
  }

  // --- H: Stress test (race exposure) ---
  write_str("\n[H] Contended stress test (8-pair, 50k iters):\n");
  stress_contended();

  // --- J: DLL cancel-race stress test ---
  // Exercises the specific race between waiter cancel and waker claim:
  //   Waiter: insert → unlock → value changed → cancel (lock → remove)
  //   Waker:  write value → lock → scan → CAS WAITING→SIGNALED → remove
  // With correct CAS-guarded cancel, only one side removes the node.
  // Without it, both remove → stale prev/next → DLL corruption → hang.
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

      reset_lp();
      MutexBenchShared ctx;
      ctx.lock.set(0);
      ctx.counter = 0;
      ctx.iters = DLL_STRESS_ITERS / nt;
      ctx.arrived.store(0, cpp::MemoryOrder::RELAXED);
      ctx.go.store(false, cpp::MemoryOrder::RELAXED);

      ThreadArg tas[16];
      pthread_t handles[16];
      for (int t = 0; t < nt; ++t)
        handles[t] = start_thread(mutex_futex_worker, &ctx, tas[t]);
      while (ctx.arrived.load(cpp::MemoryOrder::ACQUIRE) < nt)
        ;

      int64_t t0 = now_ns();
      ctx.go.store(true, cpp::MemoryOrder::RELEASE);
      for (int t = 0; t < nt; ++t)
        join_thread(handles[t]);
      int64_t elapsed = now_ns() - t0;

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
