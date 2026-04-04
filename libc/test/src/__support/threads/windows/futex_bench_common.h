//===-- Shared benchmark infrastructure for futex / futex_addr benches ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single source of truth for the futex / futex_addr benchmark harnesses.
// Every inline definition here is driven by a hazard observed in a real
// bench run — *not* generic engineering "clean code" polish. Notes inline.
//
// Roster:
//   - no-CRT output (write_str / write_i64 / write_padded)
//   - RDTSC-based timer calibrated against QPC (init_timer / now_ns)
//   - thermal_warmup to push cores past idle C-states before timing
//   - LP schedule: primary SMT siblings first, then secondary
//   - pin_thread / boost_priority
//   - ThreadArg + start_thread / join_thread helpers
//   - WorkerPool — reusable pool of pinned workers, for harnesses that
//     DO NOT use Futex in the worker body (e.g. pure WoA / IOCP).
//     Rejected for anything touching our Futex: the pool's round_seq
//     Futex::wait collides with the benched primitive on the same
//     wait-slot bucket and deadlocks at 8T+ contention.
//   - fresh_run_mutex_style — fresh-thread-per-sample runner. Busy-wait
//     atomic barriers only, zero Futex anywhere in the harness, so the
//     benched primitive owns the only Futex in flight. This is the
//     runner every mutex / Futex-touching bench MUST use.
//   - insertion_sort, trimmed-mean / IQR / CV report()
//   - bench_prelude()
//
// Everything is `inline` so both bench TUs can include this header
// without ODR trouble.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TEST_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_BENCH_COMMON_H
#define LLVM_LIBC_TEST_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_BENCH_COMMON_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/threads/raw_mutex.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_join.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

// ---- Output (no CRT) ----

inline HANDLE g_stdout;

inline void write_str(const char *s) {
  const char *p = s;
  while (*p)
    ++p;
  test_support::write_handle(g_stdout, s, static_cast<DWORD>(p - s));
}

// int64 to decimal string, returns pointer into buf.
inline char *i64_to_str(int64_t v, char *buf, int buflen) {
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

inline void write_i64(int64_t v) {
  char buf[24];
  write_str(i64_to_str(v, buf, sizeof(buf)));
}

// Right-pad name to width.
inline void write_padded(const char *s, int width) {
  write_str(s);
  int len = 0;
  while (s[len])
    ++len;
  for (int i = len; i < width; ++i)
    write_str(" ");
}

// ---- Timing ----
//
// RDTSC-based clock with one-time calibration against QPC. Rationale:
//   - QPC call overhead is ~20-40ns (ntdll thunk + syscall-like path)
//     which is a material fraction of ~700ns ping-pong samples.
//   - RDTSC is ~20-25 cycles on modern x86 and has 1-cycle resolution,
//     far below QPC's ~100ns tick.
//   - TSC is invariant (constant-rate regardless of P-state) on every
//     CPU this bench targets — verified by CPUID.80000007H:EDX bit 8
//     on all Zen / Nehalem+ parts.
//
// Calibration: sample TSC+QPC twice with a brief gap, derive cycles-per-ns
// as a Q32 fixed-point scale. ns = (cycles * scale_q32) >> 32. With
// scale_q32 ≈ 0.22 for 4.5GHz TSC, overflow only happens past ~8e14
// cycles (~50 hours of runtime) — irrelevant here.

inline uint64_t g_tsc_start;
inline uint64_t g_ns_per_cycle_q32; // ns = (cycles * q32) >> 32

LIBC_INLINE uint64_t rdtsc_now() {
  // `rdtsc` retires in ~20 cycles on Zen/Skylake; no serialization needed
  // for benchmark deltas since OoO reordering within a ~20-cycle window
  // is dwarfed by the intervals we measure.
  return __builtin_ia32_rdtsc();
}

LIBC_INLINE int64_t now_ns() {
  uint64_t cycles = rdtsc_now() - g_tsc_start;
  // Two-step 64-bit: ns_per_qpc first (fits 64-bit for any sane QPC freq),
  // then (cycles * q32) >> 32. __uint128_t multiplication lowers to a
  // single `mulq` on x86-64 with no runtime helper (the prior comment
  // about __udivti3 was about division — irrelevant here).
  __uint128_t prod =
      static_cast<__uint128_t>(cycles) * g_ns_per_cycle_q32;
  uint64_t hi = static_cast<uint64_t>(prod >> 64);
  uint64_t lo = static_cast<uint64_t>(prod);
  return static_cast<int64_t>((hi << 32) | (lo >> 32));
}

inline void init_timer() {
  LARGE_INTEGER qpc_freq;
  ::RtlQueryPerformanceFrequency(&qpc_freq);

  // Calibrate TSC against QPC. Three 20ms windows, take the window with
  // the tightest QPC delta so a context switch doesn't bias the result.
  uint64_t best_tsc = 0, best_qpc = 0;
  int64_t best_span = 1LL << 62;
  for (int trial = 0; trial < 3; ++trial) {
    LARGE_INTEGER q0, q1;
    ::RtlQueryPerformanceCounter(&q0);
    uint64_t t0 = rdtsc_now();
    LARGE_INTEGER delay;
    delay.QuadPart = -20 * 10000LL; // 20ms
    ::NtDelayExecution(FALSE, &delay);
    uint64_t t1 = rdtsc_now();
    ::RtlQueryPerformanceCounter(&q1);
    int64_t span = q1.QuadPart - q0.QuadPart;
    if (span < best_span) {
      best_span = span;
      best_tsc = t1 - t0;
      best_qpc = static_cast<uint64_t>(q1.QuadPart - q0.QuadPart);
    }
  }
  // ns_elapsed = best_qpc * 1e9 / qpc_freq
  // ns_per_cycle = ns_elapsed / best_tsc
  // q32 = ns_per_cycle * 2^32
  uint64_t ns_per_qpc_q32 =
      (uint64_t)((1000000000ull << 32) / qpc_freq.QuadPart);
  g_ns_per_cycle_q32 = (best_qpc * ns_per_qpc_q32) / best_tsc;
  g_tsc_start = rdtsc_now();
}

// Spin for ~300ms on main to push the core out of idle C-states and let
// boost clocks engage. Without this the first few benches measure a
// ramping frequency instead of steady-state.
inline void thermal_warmup() {
  volatile uint64_t sink = 0;
  uint64_t start = rdtsc_now();
  // ~300ms; upper bound based on 3GHz lower-bound TSC.
  uint64_t budget = 900000000ull;
  while (rdtsc_now() - start < budget) {
    for (int i = 0; i < 4096; ++i)
      sink += i * 1103515245ull + 12345ull;
  }
  (void)sink;
}

// ---- Sort ----

inline void insertion_sort(int64_t *a, int n) {
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
//
// Default: pin main to LP 2 (avoids LP 0/1 which share physical core 0
// and handle most OS DPCs / interrupts — keeps benchmark threads quiet).
inline void pin_thread(HANDLE thread, int logical_processor) {
  ULONG_PTR mask = 1ull << logical_processor;
  ::NtSetInformationThread(thread, 4 /*ThreadAffinityMask*/, &mask,
                           sizeof(mask));
}

// Boost process to ABOVE_NORMAL priority class to reduce preemption noise.
// Deliberately NOT HIGH (class 3, base 13): HIGH ties Task Manager and
// explorer.exe, and AutoBoost on NtAlertMultipleThreadByThreadId can lift
// waiters one above that. With 32+ contending workers on a small machine
// the desktop becomes inert and the user cannot launch taskkill / Task
// Manager to recover — observed empirically (hard reboot required).
// ABOVE_NORMAL (class 6, base 10) sits above background NORMAL=8 noise
// while remaining strictly below the kill-switch chain at HIGH=13.
inline void boost_priority() {
  struct {
    BOOLEAN Foreground;
    UCHAR PriorityClass;
  } ppc = {FALSE, 6};
  ::NtSetInformationProcess(NtCurrentProcess(), 18 /*ProcessPriorityClass*/,
                            &ppc, sizeof(ppc));
}

// ---- Thread helper (pthread) ----
//
// LP schedule: primary SMT siblings of distinct physical cores first,
// then secondary siblings, so small-N tests get real parallelism rather
// than contending on one physical core's shared L1/L2. Main is pinned
// to LP 2, so workers skip LP 2 and its sibling LP 3.
//
// Typical Windows LP enumeration pairs (2k, 2k+1) on the same physical
// core, so even LPs = distinct cores. On 16-LP Zen (8 cores), schedule
// is [4,6,8,10,12,14, 5,7,9,11,13,15] — 12 entries, wraps for 16T+
// tests where oversubscription is the point.
inline int g_lp_schedule[128];
inline int g_lp_schedule_len;
inline cpp::Atomic<int> g_next_lp_idx{0};

inline void init_lp_schedule() {
  uint32_t lp_count =
      *reinterpret_cast<const volatile uint32_t *>(0x7FFE03C0ull);
  if (lp_count == 0 || lp_count > 128)
    lp_count = 16; // sane fallback
  constexpr int main_lp = 2;
  constexpr int main_sibling = 3;
  int idx = 0;
  // Pass 1: primary SMT siblings (even LPs), skipping main's core and LP 0
  // (boot processor tends to run heavier DPC load).
  for (uint32_t lp = 2; lp < lp_count; lp += 2) {
    if ((int)lp != main_lp && (int)lp != main_sibling)
      g_lp_schedule[idx++] = lp;
  }
  // Pass 2: secondary SMT siblings (odd LPs), same skips.
  for (uint32_t lp = 1; lp < lp_count; lp += 2) {
    if ((int)lp != main_lp && (int)lp != main_sibling)
      g_lp_schedule[idx++] = lp;
  }
  g_lp_schedule_len = idx;
}

struct ThreadArg {
  void (*func)(void *);
  void *ctx;
  int lp; // logical processor to pin to
};

inline void *thread_trampoline(void *arg) {
  auto *ta = static_cast<ThreadArg *>(arg);
  pin_thread(NtCurrentThread(), ta->lp);
  ta->func(ta->ctx);
  return nullptr;
}

inline pthread_t start_thread(void (*func)(void *), void *ctx,
                              ThreadArg &arg) {
  arg.func = func;
  arg.ctx = ctx;
  int i = g_next_lp_idx.fetch_add(1, cpp::MemoryOrder::RELAXED);
  arg.lp = g_lp_schedule[i % g_lp_schedule_len];
  pthread_t tid;
  LIBC_NAMESPACE::pthread_create(&tid, nullptr, thread_trampoline, &arg);
  return tid;
}

// Reset LP cursor between benchmarks so each bench starts on a clean
// physical-core-first walk of the schedule.
inline void reset_lp() {
  g_next_lp_idx.store(0, cpp::MemoryOrder::RELAXED);
}

inline void join_thread(pthread_t tid) {
  LIBC_NAMESPACE::pthread_join(tid, nullptr);
}

// ---- Quiet sleep between tests ----
//
// Short sleep to decorrelate noise between tests. Background ISR bursts,
// Defender scans, and other transient load that hit during one test
// shouldn't bleed into the next. 50ms is long enough for the scheduler
// to drain pending DPCs without being noticeable in wall time.
LIBC_INLINE void quiet_sleep_ns(int64_t ns) {
  LARGE_INTEGER delay;
  delay.QuadPart = -ns / 100; // negative = relative, 100ns ticks
  ::NtDelayExecution(FALSE, &delay);
}

// ============================================================
// WorkerPool — persistent pinned workers for NON-Futex harnesses
//
// Rejected for any bench whose worker body touches our Futex: the
// pool's round_seq Futex::wait collides with the benched primitive
// on the same wait-slot bucket at 8T+ contention and deadlocks.
// Use fresh_run_mutex_style (below) for those. This pool is retained
// for pure-WoA / IOCP / non-Futex harnesses where the per-round
// Futex::wait on round_seq doesn't interact with the bench.
// ============================================================

struct WorkerPool {
  static constexpr int MAX_WORKERS = 64;

  struct Slot {
    WorkerPool *pool;
    int idx;
    int lp;
  };

  int num_workers = 0;
  pthread_t handles[MAX_WORKERS];
  Slot slots[MAX_WORKERS];

  // Round coordination. round_seq is bumped by main to start a new
  // round; workers Futex::wait on it between rounds with a preceding
  // heartbeat spin budget.
  Futex round_seq{0};
  cpp::Atomic<int> arrived{0};   // workers at work_go barrier
  cpp::Atomic<int> completed{0}; // workers finished round
  cpp::Atomic<int> work_go{0};   // released by main to start work
  cpp::Atomic<bool> shutdown{false};

  // Current round payload.
  void (*round_fn)(void *ctx, int idx) = nullptr;
  void *round_ctx = nullptr;
};

// Short spin budget between rounds — ~100μs at 4GHz. Long enough to
// catch back-to-back rounds without a kernel wake round-trip, short
// enough that a just-completed worker's spin doesn't hog the full
// 15.6ms scheduler quantum from its pinned-LP sibling still trying
// to run fn. An earlier revision used 10ms here; that caused fan-out
// [E]/[G] to regress 280× on oversubscribed LPs (8ms instead of
// 100μs for 16 waiters on 13 LPs).
inline constexpr uint64_t POOL_HEARTBEAT_CYCLES = 400000ull;

// Hard-fail a pool misconfiguration. Writes to stdout + NtTerminateProcess
// avoids pulling in std::abort / assert, which would drag CRT surface we
// don't link. Used only on "this bench is structurally wrong" paths —
// never on a measurement.
[[noreturn]] inline void pool_fatal(const char *msg) {
  write_str("  FATAL: pool: ");
  write_str(msg);
  write_str("\n");
  ::NtTerminateProcess(NtCurrentProcess(), 2);
  for (;;) // satisfy [[noreturn]] if TerminateProcess races
    ;
}

inline void *pool_worker_trampoline(void *arg) {
  auto *slot = static_cast<WorkerPool::Slot *>(arg);
  WorkerPool *p = slot->pool;
  int idx = slot->idx;

  pin_thread(NtCurrentThread(), slot->lp);

  uint32_t last_seq = 0;
  for (;;) {
    // Wait for next round. Short spin first, then Futex::wait. The
    // spin catches tight inter-round gaps without a kernel wake,
    // while the park path lets oversubscribed siblings run.
    for (;;) {
      uint32_t cur = p->round_seq.load(cpp::MemoryOrder::ACQUIRE);
      if (cur != last_seq) {
        last_seq = cur;
        break;
      }
      if (p->shutdown.load(cpp::MemoryOrder::ACQUIRE))
        return nullptr;

      uint64_t t0 = rdtsc_now();
      bool caught = false;
      while (rdtsc_now() - t0 < POOL_HEARTBEAT_CYCLES) {
        cur = p->round_seq.load(cpp::MemoryOrder::ACQUIRE);
        if (cur != last_seq) {
          last_seq = cur;
          caught = true;
          break;
        }
        if (p->shutdown.load(cpp::MemoryOrder::ACQUIRE))
          return nullptr;
        __builtin_ia32_pause();
      }
      if (caught)
        break;

      p->round_seq.wait(last_seq);
    }

    if (p->shutdown.load(cpp::MemoryOrder::ACQUIRE))
      return nullptr;

    // Arrive at work_go barrier.
    p->arrived.fetch_add(1, cpp::MemoryOrder::RELEASE);

    // Spin until main releases work_go. No parking — main spins on
    // pool.arrived so the whole barrier is sub-microsecond.
    //
    // Shutdown check every 64 pauses: a bench that calls
    // pool_start_round without a matching pool_release_work (error
    // path, test-list abort, etc.) would otherwise trap every worker
    // here forever and hang pool_shutdown's join. 1-in-64 load is
    // well below the barrier's sub-microsecond budget.
    for (int spin_i = 0; p->work_go.load(cpp::MemoryOrder::ACQUIRE) == 0;
         ++spin_i) {
      if ((spin_i & 63) == 0 && p->shutdown.load(cpp::MemoryOrder::ACQUIRE))
        return nullptr;
      __builtin_ia32_pause();
    }

    p->round_fn(p->round_ctx, idx);

    p->completed.fetch_add(1, cpp::MemoryOrder::RELEASE);
  }
}

inline void pool_init(WorkerPool &p, int n) {
  if (n <= 0 || n > WorkerPool::MAX_WORKERS)
    pool_fatal("num_workers out of range");
  if (g_lp_schedule_len <= 0)
    pool_fatal("LP schedule empty — init_lp_schedule not run?");

  p.num_workers = n;
  p.shutdown.store(false, cpp::MemoryOrder::RELEASE);
  p.round_seq.store(0);
  p.arrived.store(0, cpp::MemoryOrder::RELAXED);
  p.completed.store(0, cpp::MemoryOrder::RELAXED);
  p.work_go.store(0, cpp::MemoryOrder::RELAXED);

  reset_lp();
  for (int i = 0; i < n; ++i) {
    p.slots[i].pool = &p;
    p.slots[i].idx = i;
    int lp_idx = g_next_lp_idx.fetch_add(1, cpp::MemoryOrder::RELAXED);
    p.slots[i].lp = g_lp_schedule[lp_idx % g_lp_schedule_len];
    p.handles[i] = 0;
    int rc = LIBC_NAMESPACE::pthread_create(
        &p.handles[i], nullptr, pool_worker_trampoline, &p.slots[i]);
    // pthread_create failure is non-recoverable: pool_start_round would
    // spin forever on arrived < num_workers. Tear down cleanly — otherwise
    // pool_fatal leaks kernel objects for the remaining handles.
    if (rc != 0) {
      p.num_workers = i;
      p.shutdown.store(true, cpp::MemoryOrder::RELEASE);
      uint32_t next = p.round_seq.load(cpp::MemoryOrder::RELAXED) + 1;
      p.round_seq.store_and_notify_all(next);
      for (int j = 0; j < i; ++j)
        LIBC_NAMESPACE::pthread_join(p.handles[j], nullptr);
      pool_fatal("pthread_create failed");
    }
  }
}

// Reap all worker threads. Analogous to waitpid() for processes — must
// observe termination of every spawned worker, otherwise:
//   - the pool struct is reused (subsequent pool_init on the same
//     storage) with stale handles → silent handle reuse / crash;
//   - kernel thread objects leak for the rest of the process lifetime;
//   - an outstanding worker may still touch freed round_ctx memory.
inline void pool_shutdown(WorkerPool &p) {
  p.shutdown.store(true, cpp::MemoryOrder::RELEASE);
  uint32_t next = p.round_seq.load(cpp::MemoryOrder::RELAXED) + 1;
  p.round_seq.store_and_notify_all(next);
  for (int i = 0; i < p.num_workers; ++i) {
    int rc = LIBC_NAMESPACE::pthread_join(p.handles[i], nullptr);
    if (rc != 0)
      pool_fatal("pthread_join failed — worker leaked");
    p.handles[i] = 0;
  }
  p.num_workers = 0;
}

// Set up a round and wait for all workers to arrive at the work_go
// barrier. On return, workers are spinning on work_go; timing can
// start and work_go released immediately to begin the work phase.
inline void pool_start_round(WorkerPool &p, void (*fn)(void *, int),
                             void *ctx) {
  p.round_fn = fn;
  p.round_ctx = ctx;
  p.arrived.store(0, cpp::MemoryOrder::RELEASE);
  p.completed.store(0, cpp::MemoryOrder::RELEASE);
  p.work_go.store(0, cpp::MemoryOrder::RELEASE);

  uint32_t next = p.round_seq.load(cpp::MemoryOrder::RELAXED) + 1;
  p.round_seq.store_and_notify_all(next);

  while (p.arrived.load(cpp::MemoryOrder::ACQUIRE) < p.num_workers)
    __builtin_ia32_pause();
}

// Release work_go; workers start running round_fn.
LIBC_INLINE void pool_release_work(WorkerPool &p) {
  p.work_go.store(1, cpp::MemoryOrder::RELEASE);
}

// Spin until all workers have finished round_fn.
LIBC_INLINE void pool_wait_done(WorkerPool &p) {
  while (p.completed.load(cpp::MemoryOrder::ACQUIRE) < p.num_workers)
    __builtin_ia32_pause();
}

// One-shot: set up round, time work_go-release → all-completed.
// Returns wall time in ns. Use ONLY for harnesses whose worker body
// does not touch our Futex. For mutex / Futex-touching benches use
// fresh_run_mutex_style instead.
inline int64_t pool_run_mutex_style(WorkerPool &p, void (*fn)(void *, int),
                                    void *ctx) {
  pool_start_round(p, fn, ctx);
  int64_t t0 = now_ns();
  pool_release_work(p);
  pool_wait_done(p);
  return now_ns() - t0;
}

// ============================================================
// Fresh-thread-per-sample mutex-style runner
// ============================================================
//
// Pool-based mutex benches hit reproducible hangs at contention scale
// (cs=100ns 8T+, cs=1000ns 16T+) because the pool's own round_seq
// Futex::wait contends with the bench's per-round Futex::wait — the
// same wait-slot bucket collision the fan-out benches already worked
// around by spawning fresh threads per sample.
//
// The thread-create/exit cost that originally motivated the pool stays
// OUTSIDE the timing window here: main waits for all workers to arrive
// at a plain atomic barrier, starts the timer, releases `go`, spins
// for `done == N`, stops the timer, then joins. Spawn + join sit
// outside; the measured window is release-to-all-done, matching the
// pool's `pool_run_mutex_style` semantics.
//
// No Futex::wait anywhere in the harness — arrival / go / done are
// all busy-wait on plain atomics, so the harness contributes zero
// wait-slot churn that could interfere with the benched primitive.
//
// FreshHook: optional per-worker enter/exit callback (e.g. for
// futex_instrument binding). Called on the worker thread before the
// arrival barrier (enter) and after round_fn returns, before the
// done fetch_add (exit). Either may be null.
using FreshHook = void (*)(int idx);

struct FreshHarness {
  void (*fn)(void *, int);
  void *user_ctx;
  int idx;
  cpp::Atomic<int> *arrived;
  cpp::Atomic<int> *go;
  cpp::Atomic<int> *done;
  FreshHook on_enter;
  FreshHook on_exit;
};

inline void fresh_harness_trampoline(void *arg) {
  auto *h = static_cast<FreshHarness *>(arg);
  if (h->on_enter)
    h->on_enter(h->idx);
  h->arrived->fetch_add(1, cpp::MemoryOrder::RELEASE);
  while (h->go->load(cpp::MemoryOrder::ACQUIRE) == 0)
    __builtin_ia32_pause();
  h->fn(h->user_ctx, h->idx);
  if (h->on_exit)
    h->on_exit(h->idx);
  h->done->fetch_add(1, cpp::MemoryOrder::RELEASE);
}

inline int64_t fresh_run_mutex_style(int num_threads, void (*fn)(void *, int),
                                     void *ctx, FreshHook on_enter = nullptr,
                                     FreshHook on_exit = nullptr) {
  reset_lp();
  cpp::Atomic<int> arrived{0};
  cpp::Atomic<int> go{0};
  cpp::Atomic<int> done{0};

  FreshHarness hs[64];
  ThreadArg tas[64];
  pthread_t handles[64];
  for (int t = 0; t < num_threads; ++t) {
    hs[t] = {fn, ctx, t, &arrived, &go, &done, on_enter, on_exit};
    handles[t] = start_thread(fresh_harness_trampoline, &hs[t], tas[t]);
  }

  while (arrived.load(cpp::MemoryOrder::ACQUIRE) < num_threads)
    __builtin_ia32_pause();
  quiet_sleep_ns(1'000'000); // 1ms settle

  int64_t t0 = now_ns();
  go.store(1, cpp::MemoryOrder::RELEASE);
  while (done.load(cpp::MemoryOrder::ACQUIRE) < num_threads)
    __builtin_ia32_pause();
  int64_t elapsed = now_ns() - t0;

  for (int t = 0; t < num_threads; ++t)
    join_thread(handles[t]);
  return elapsed;
}

// ---- Constants ----

// Ping-pong benches use batched sampling: each sample times a BATCH-sized
// inner loop and divides, amortizing timer overhead and pushing sample
// duration well above any clock quantum. 100 × 500 = 50k iters per bench;
// enough to reach steady-state without burning wall clock.
inline constexpr int PING_WARMUP_ITERS = 5000;
inline constexpr int PING_SAMPLES = 100;
inline constexpr int PING_BATCH = 500;

// Total pingpong iters the responder thread must service:
// warmup + samples × batch.
inline constexpr int PING_TOTAL = PING_WARMUP_ITERS + PING_SAMPLES * PING_BATCH;

// Legacy single-sample budget retained for bench points where batching
// doesn't apply (fan-out, mutex total-time, throughput). 150 samples
// gives sqrt(1.5)≈1.22x tighter SE than 100 without crossing into
// "this bench takes a perceptible second" territory.
inline constexpr int HEAVY_SAMPLES = 150;

// Warmup rounds discarded to absorb cold-cache / park-wake costs
// incurred on first-sample-after-test-boundary. 10% of measured
// samples, matching the "drop top/bottom 10%" trim policy in report().
inline constexpr int HEAVY_WARMUP = HEAVY_SAMPLES / 10;

// ---- Report ----

inline void report(const char *name, int64_t *timings, int n) {
  insertion_sort(timings, n);
  // Trimmed mean: exclude bottom/top 10% to remove outliers.
  int lo = n / 10, hi = n - n / 10;
  int64_t sum = 0;
  for (int i = lo; i < hi; ++i)
    sum += timings[i];
  int64_t tmean = (hi > lo) ? sum / (hi - lo) : timings[n / 2];

  // Coefficient of variation over trimmed samples. CV = std/mean
  // expressed as percent — useful for "is this test noisy?" flagging
  // independent of absolute scale. Computed via integer arithmetic on
  // trimmed samples to avoid depending on floating-point in freestanding
  // builds.
  //
  // sum_sq_dev = Σ (x - mean)^2. std = √(sum_sq_dev / (hi-lo)).
  int cv_pct = 0;      // integer percent
  int cv_pct_frac = 0; // tenths
  if (hi > lo && tmean > 0) {
    uint64_t sq = 0;
    for (int i = lo; i < hi; ++i) {
      int64_t d = timings[i] - tmean;
      sq += static_cast<uint64_t>(d * d);
    }
    uint64_t var = sq / static_cast<uint64_t>(hi - lo);
    // Integer sqrt via Newton-Raphson — converges in <6 iters for
    // values that fit in 64 bits.
    uint64_t std_ = 0;
    if (var > 0) {
      uint64_t x = var;
      for (int i = 0; i < 32; ++i)
        x = (x + var / (x ? x : 1)) / 2;
      std_ = x;
    }
    uint64_t cv_q10 = (std_ * 1000u) / static_cast<uint64_t>(tmean);
    cv_pct = static_cast<int>(cv_q10 / 10);
    cv_pct_frac = static_cast<int>(cv_q10 % 10);
  }
  bool noisy = cv_pct >= 15; // ≥15% CV — treat deltas skeptically

  // IQR (p25/p75) is the robust spread indicator; min is almost always
  // a single fluke, p99 catches rare preemption stalls.
  write_str("  ");
  write_padded(name, 38);
  write_str("med=");
  write_i64(timings[n / 2]);
  write_str("  avg=");
  write_i64(tmean);
  write_str("  p25=");
  write_i64(timings[n / 4]);
  write_str("  p75=");
  write_i64(timings[n * 3 / 4]);
  write_str("  p99=");
  write_i64(timings[n * 99 / 100]);
  write_str(" ns  cv=");
  write_i64(cv_pct);
  write_str(".");
  write_i64(cv_pct_frac);
  write_str("%");
  if (noisy)
    write_str(" [NOISY]");
  write_str("\n");
}

// ---- Shared prelude ----
//
// One-shot environment setup used by every bench main: open stdout,
// calibrate timer, compute LP schedule, boost priority, pin main to
// LP 2, thermal-warmup.
inline void bench_prelude() {
  g_stdout = ::NtCurrentStandardOutput();
  init_timer();
  init_lp_schedule();
  boost_priority();
  pin_thread(NtCurrentThread(), 2);
  thermal_warmup();
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_BENCH_COMMON_H
