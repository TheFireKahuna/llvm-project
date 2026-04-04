//===-- ThreadRing / BatchEngineT<Cfg> microbenchmark ----------*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Exercises the real production ThreadRing + BatchEngineT code paths (no
// mocks) across the full policy surface exposed by DefaultBatchCfg and
// the split primitives (fill_ring / flush_submit / drain_available /
// wait_one / cancel_in_flight / compute_result).
//
// Test matrix:
//
//   Cfg variants:
//     A. CfgDefault    — INITIAL_QD=∞,            INITIAL_WAIT=1  (ship)
//     B. CfgNowait     — INITIAL_QD=∞,            INITIAL_WAIT=0
//     C. CfgUniformQD  — INITIAL_QD=PIPELINE_QD,  INITIAL_WAIT=0  (pure pipeline)
//     D. CfgSubmit1    — SUBMIT_BATCH=1           (submit on every refill)
//     E. CfgSubmit8    — SUBMIT_BATCH=8           (rare mid-loop submits)
//     F. CfgBigQD      — PIPELINE_QD=16           (deeper steady-state)
//
//   Strategy variants (all use the real ring, via split primitives):
//     1. submit_and_drain   — production composition (reference)
//     2. split_batch_drain  — fill→submit_wait(in_flight)→drain; loop for N>SQ
//     3. split_wait_per_cqe — fill→submit_nowait; drain_available + wait_one<false>
//
//   Batch sizes: N ∈ {1, 8, 32, 64}    (single, sub-QD, 2×SQ, MAX_OPS)
//   Op length:    4 KB (sector-aligned; works for both cached and O_DIRECT)
//
//   Access pattern: sequential sweep, wrap at EOF. Each iteration reads
//   the NEXT (n_ops × op_len) window of the file; a global cursor advances
//   across ALL cells. This matches what real streaming readers (cat,
//   grep, md5sum, source-file parsers) actually do. Working-set size is
//   the whole file (default 32 MiB, configurable via BENCH_FILE_SIZE_MIB)
//   so cells don't collapse into an L2/L3-resident hot region — the
//   page-cache/controller-cache hit rates reflect realistic file I/O.
//
//   I/O modes (top-level sections):
//     1. Cached — buffered handle, use_regbuf=false. Measures engine
//        overhead with zero device latency (page cache hit).
//     2. O_DIRECT — FILE_NO_INTERMEDIATE_BUFFERING, use_regbuf=true.
//        Exercises the registered bounce buffer (pre-pinned MDL) path.
//        Kernel DMAs into the registered region, engine memcpy's to user.
//     3. O_DIRECT regbuf impact — StrategyAutomatic×CfgDefault at each N
//        with use_regbuf=false vs true. Isolates the MDL-savings number
//        from all other dimensions.
//
// Precision:
//   rdtsc (cycle-precise) calibrated against QPC over a 10 ms interval.
//   Realtime priority class + single-CPU pin eliminate migration jitter.
//   WARMUP=200, SAMPLES=500 per cell. Min / p50 / p95 / avg reported.
//   Byte-total + errno verified per iteration; EINTR and genuine mismatches
//   counted separately so signal-storm noise is distinguishable from a
//   real engine failure.
//   Per-slot destination buffers with distinct per-op file offsets —
//   mirrors production readv/preadv scatter-gather, surfaces per-op MDL
//   cost in the unregistered O_DIRECT path, and surfaces distinct-VA
//   memcpy cost in the registered path. (Single-buffer shared reads
//   collapse both costs to near-zero; this is the fidelity refinement.)
//   File cache pre-primed via ThreadRing itself (uniform hot path).
//   Registered buffer pre-initialized via a dummy O_DIRECT read so the
//   lazy NtAllocateVirtualMemory + IORING_OP_REGISTER_BUFFERS syscall
//   cost doesn't leak into the first O_DIRECT cell's samples.
//   Each strategy × CfgDefault pre-warmed so the first cell's first
//   sample doesn't absorb icache-cold cost later cells don't pay.
//   Split-primitive strategies install a SyscallFrameGuard to match the
//   production submit_and_drain path's cost exactly — without this, the
//   hand-composed paths would measure ~40 ns less work per iteration
//   for reasons unrelated to submit/drain shape.
//
//   Caveat: O_DIRECT bypasses the OS page cache but not storage-level
//   caches (NVMe controller / physical-disk cache). After warmup the
//   reads hit the controller cache, so absolute latency reflects cache-
//   warm async I/O, not cold-cache disk seek. Relative comparisons across
//   strategies/Cfgs under these conditions are still meaningful — what we
//   measure is engine overhead layered over uniform async latency.
//
// Freestanding build — the libc test framework (get_object_files_for_test)
// links in the specific internal .obj files the bench touches. No c.dll in
// the process; internal symbols resolve directly. See CMakeLists.txt.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/OSUtil/windows/io/batch_engine.h"
#include "src/__support/OSUtil/windows/io/io_ring_helpers.h"
#include "src/__support/OSUtil/windows/io/thread_ring.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/ntdll.h"

namespace LIBC_NAMESPACE_DECL {

// ───────────────────────────────────────────────────────────────────────────
// Output — write_str / write_i64 (PEB StandardOutput)
// ───────────────────────────────────────────────────────────────────────────

static HANDLE g_stdout;

static void write_str(const char *s) {
  const char *p = s;
  while (*p)
    ++p;
  IO_STATUS_BLOCK iosb = {};
  ::NtWriteFile(g_stdout, nullptr, nullptr, nullptr, &iosb,
                const_cast<char *>(s), static_cast<ULONG>(p - s), nullptr,
                nullptr);
}

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

static void write_padded(const char *s, int width) {
  write_str(s);
  int len = 0;
  while (s[len])
    ++len;
  for (int i = len; i < width; ++i)
    write_str(" ");
}

// ───────────────────────────────────────────────────────────────────────────
// TSC calibration (cycle-precise, QPC-anchored)
// ───────────────────────────────────────────────────────────────────────────

static int64_t g_tsc_freq_khz;

static inline uint64_t rdtsc() { return __builtin_ia32_rdtsc(); }

static void init_timer() {
  LARGE_INTEGER qpc_freq, qpc0, qpc1;
  ::RtlQueryPerformanceFrequency(&qpc_freq);

  ::RtlQueryPerformanceCounter(&qpc0);
  uint64_t tsc0 = rdtsc();
  LARGE_INTEGER delay;
  delay.QuadPart = -100000; // 10 ms
  ::NtDelayExecution(FALSE, &delay);
  uint64_t tsc1 = rdtsc();
  ::RtlQueryPerformanceCounter(&qpc1);

  int64_t qpc_elapsed = qpc1.QuadPart - qpc0.QuadPart;
  int64_t tsc_elapsed = static_cast<int64_t>(tsc1 - tsc0);
  g_tsc_freq_khz = tsc_elapsed * qpc_freq.QuadPart / (qpc_elapsed * 1000);
}

static inline int64_t tsc_to_ns(int64_t tsc_delta) {
  return tsc_delta * 1000000LL / g_tsc_freq_khz;
}

// ───────────────────────────────────────────────────────────────────────────
// Stats
// ───────────────────────────────────────────────────────────────────────────

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

static void print_stats(const char *label, int64_t *samples, int n) {
  insertion_sort(samples, n);
  int64_t sum = 0;
  for (int i = 0; i < n; ++i)
    sum += samples[i];

  write_padded(label, 48);
  write_str("  min=");
  write_i64(tsc_to_ns(samples[0]));
  write_str("  p50=");
  write_i64(tsc_to_ns(samples[n / 2]));
  write_str("  p95=");
  write_i64(tsc_to_ns(samples[n * 95 / 100]));
  write_str("  avg=");
  write_i64(tsc_to_ns(sum / n));
  write_str(" ns  (");
  write_i64(samples[n / 2]);
  write_str(" cyc)\n");
}

// ───────────────────────────────────────────────────────────────────────────
// Affinity / priority
// ───────────────────────────────────────────────────────────────────────────

static void pin_thread(HANDLE thread, int lp) {
  ULONG_PTR mask = 1ull << lp;
  ::NtSetInformationThread(thread, 4 /*ThreadAffinityMask*/, &mask,
                           sizeof(mask));
}

static void boost_priority() {
  struct {
    BOOLEAN Foreground;
    UCHAR PriorityClass;
  } ppc = {FALSE, 3}; // PROCESS_PRIORITY_CLASS_HIGH
  ::NtSetInformationProcess(NtCurrentProcess(), 18 /*ProcessPriorityClass*/,
                            &ppc, sizeof(ppc));
}

// ───────────────────────────────────────────────────────────────────────────
// Cfg variants
// ───────────────────────────────────────────────────────────────────────────
//
// Each variant inherits DefaultBatchCfg and overrides specific fields.
// Non-overridden fields fall through to DefaultBatchCfg's values, so
// variant semantics stay minimal and explicit.

using CfgDefault = ioring::DefaultBatchCfg;

struct CfgNowait : ioring::DefaultBatchCfg {
  static constexpr bool INITIAL_WAIT = false;
};

struct CfgUniformQD : ioring::DefaultBatchCfg {
  static constexpr uint32_t INITIAL_QD = PIPELINE_QD;
  static constexpr bool INITIAL_WAIT = false;
};

struct CfgSubmit1 : ioring::DefaultBatchCfg {
  static constexpr uint32_t SUBMIT_BATCH = 1;
};

struct CfgSubmit8 : ioring::DefaultBatchCfg {
  static constexpr uint32_t SUBMIT_BATCH = 8;
};

struct CfgBigQD : ioring::DefaultBatchCfg {
  static constexpr uint32_t PIPELINE_QD = 16;
};

// ───────────────────────────────────────────────────────────────────────────
// Strategies — composed from BatchEngineT's public surface
// ───────────────────────────────────────────────────────────────────────────
//
// Each strategy returns the BatchResult so the harness can verify
// total_bytes == expected. The push-loop is shared; the submit/drain
// shape is what differs.

inline constexpr ULONG OP_LEN = 4096;
inline constexpr uint32_t MAX_N = 64;

// ───────────────────────────────────────────────────────────────────────────
// Test-file size — configurable
// ───────────────────────────────────────────────────────────────────────────
//
// Default: 32 MiB. Exceeds typical desktop L3 (16-32 MiB) so the sweep
// genuinely evicts older windows from cache as newer ones arrive — i.e.,
// cells are timing "engine overhead reading a file", not "engine overhead
// over L2/L3-resident bytes". Override at compile time:
//
//     clang++ ... -DBENCH_FILE_SIZE_MIB=20 ...
//
// Minimum is MAX_N × OP_LEN = 256 KiB (one sweep window); anything smaller
// trips the static_assert below. Practical upper bound is ambient RAM;
// beyond a few hundred MiB the fill_test_file warmup becomes the bottleneck.

#ifndef BENCH_FILE_SIZE_MIB
#define BENCH_FILE_SIZE_MIB 32
#endif

static constexpr SIZE_T kTestFileSize =
    static_cast<SIZE_T>(BENCH_FILE_SIZE_MIB) * 1024u * 1024u;

static_assert(kTestFileSize >= MAX_N * OP_LEN,
              "BENCH_FILE_SIZE_MIB must be large enough to hold one sweep "
              "window (MAX_N * OP_LEN = 256 KiB).");

// ───────────────────────────────────────────────────────────────────────────
// Destination buffers
// ───────────────────────────────────────────────────────────────────────────
//
// Per-slot destination buffers. Distinct VAs per slot mirror production
// readv/preadv where each iovec points to a different address. For the
// unregistered O_DIRECT path this means the kernel pins a distinct MDL
// per op (exposing per-op MDL cost). For the registered path the single
// pre-pinned MDL is reused — and the engine memcpy's to N distinct VAs
// post-CQE (exposing distinct-VA memcpy cost). Both match production.
//
// 4 KiB outer alignment → each slot is 4 KiB aligned (required for
// FILE_NO_INTERMEDIATE_BUFFERING sector alignment and for registered-
// buffer VA expectations). Total size: 64 × 4 KiB = 256 KiB .bss.
alignas(4096) static char g_slot_bufs[MAX_N][OP_LEN];

// Single buffer retained for prime_regbuf — one-off dummy op at startup.
alignas(4096) static char g_bench_buf[OP_LEN];

// ───────────────────────────────────────────────────────────────────────────
// Sequential sweep cursor — real file-read pattern
// ───────────────────────────────────────────────────────────────────────────
//
// Each iteration reads the NEXT (n_ops * op_len) window of the file. On
// wrap (cursor + window > file), reset to 0. Global across all cells so
// cell N doesn't start from the same cache state as cell 0 — the bench
// measures engine overhead over a steady-state sweep, matching what
// `cat`, `grep`, `md5sum`, tar-extract, or any streaming reader does.
//
// Single-threaded (bench is pinned to one CPU), no atomic needed.
static uint64_t g_sweep_off = 0;

template <class Cfg>
static void push_reads(ioring::BatchEngineT<Cfg> &batch, uint32_t n_ops,
                       ULONG op_len) {
  const uint64_t window = static_cast<uint64_t>(n_ops) * op_len;
  if (g_sweep_off + window > kTestFileSize)
    g_sweep_off = 0;
  const ULONGLONG base = g_sweep_off;
  g_sweep_off += window;
  for (uint32_t i = 0; i < n_ops; ++i) {
    (void)batch.push_read(g_slot_bufs[i], op_len,
                          base + static_cast<ULONGLONG>(i) * op_len);
  }
}

struct StrategyAutomatic {
  static constexpr const char *name = "submit_and_drain ";

  template <class Cfg>
  static ioring::BatchResult run(HANDLE file, uint32_t n_ops, ULONG op_len,
                                 bool use_regbuf) {
    auto *tr = ioring::get_thread_ring();
    ioring::BatchEngineT<Cfg> batch;
    batch.init(tr, file, use_regbuf);
    push_reads(batch, n_ops, op_len);
    return batch.submit_and_drain();
  }
};

struct StrategyBatchDrain {
  static constexpr const char *name = "split batch-drain";

  // fill → submit_wait(in_flight) → drain_all; loop until all ops done.
  // This is the classic "push all, wait all, drain all" shape — not a
  // pipeline. For N > SQ_size it iterates the block multiple times.
  template <class Cfg>
  static ioring::BatchResult run(HANDLE file, uint32_t n_ops, ULONG op_len,
                                 bool use_regbuf) {
    auto *tr = ioring::get_thread_ring();
    ioring::BatchEngineT<Cfg> batch;
    batch.init(tr, file, use_regbuf);
    // Match submit_and_drain's SyscallFrame install cost — two atomic
    // RELEASE stores on lc->active_syscall. Without this the split-
    // primitive strategies would measure ~40 ns less work per iteration
    // than the reference automatic path, making them look artificially
    // faster for reasons unrelated to submit/drain shape.
    SyscallFrameGuard frame_guard(file, &tr->ring, tr->event,
                                  ioring::OpTag{batch.generation(), 0}
                                      .as_user_data(),
                                  /*batch_count=*/n_ops);
    auto regbuf_cleanup =
        cpp::make_scope_guard([&] { batch.release_all_regbufs(); });
    push_reads(batch, n_ops, op_len);

    while (batch.pushed() < batch.count() || batch.in_flight() > 0) {
      batch.fill_ring(batch.count() - batch.pushed());
      if (batch.in_flight() == 0)
        break;
      NTSTATUS s = batch.flush_submit(batch.in_flight()); // submit_wait(N)
      if (!NT_SUCCESS(s))
        return batch.compute_result(EIO);
      while (batch.drain_available() > 0) { /* drain until empty */
      }
    }
    return batch.compute_result(0);
  }
};

struct StrategyWaitPerCqe {
  static constexpr const char *name = "split wait-per-cqe";

  // fill → submit_nowait → { drain_available OR wait_one<false> }.
  // Uniform-QD path that never uses submit_wait — drives the ring entirely
  // via the hardware-monitor-first wait path. For cached reads, CQEs are
  // already posted by the time drain_available runs. For async, wait_one
  // catches the reactor signal.
  template <class Cfg>
  static ioring::BatchResult run(HANDLE file, uint32_t n_ops, ULONG op_len,
                                 bool use_regbuf) {
    auto *tr = ioring::get_thread_ring();
    ioring::BatchEngineT<Cfg> batch;
    batch.init(tr, file, use_regbuf);
    SyscallFrameGuard frame_guard(file, &tr->ring, tr->event,
                                  ioring::OpTag{batch.generation(), 0}
                                      .as_user_data(),
                                  /*batch_count=*/n_ops);
    auto regbuf_cleanup =
        cpp::make_scope_guard([&] { batch.release_all_regbufs(); });
    push_reads(batch, n_ops, op_len);

    uint32_t drained = 0;
    while (drained < batch.count()) {
      // Keep the in-flight queue ≤ PIPELINE_QD so this is a true pipeline.
      if (batch.pushed() < batch.count() &&
          batch.in_flight() < Cfg::PIPELINE_QD) {
        uint32_t room = Cfg::PIPELINE_QD - batch.in_flight();
        uint32_t want = batch.count() - batch.pushed();
        batch.fill_ring(want < room ? want : room);
      }
      if (batch.unflushed() > 0)
        batch.flush_submit(0); // nowait

      uint32_t d = batch.drain_available();
      drained += d;
      if (d > 0)
        continue;
      if (batch.in_flight() == 0 && batch.pushed() == batch.count())
        break;

      // No CQE yet — block for one.
      long r = batch.template wait_one<false>();
      if (r != 0)
        break; // unexpected; exit to avoid infinite loop
    }
    return batch.compute_result(0);
  }
};

// ───────────────────────────────────────────────────────────────────────────
// Test file setup — NT path, overlapped handle, pre-filled with pattern
// ───────────────────────────────────────────────────────────────────────────
//
// Manually construct UNICODE_STRING over a char16_t literal — avoids the
// L"" / wchar_t-width mismatch (NTPOSIX has 32-bit wchar_t, but NT wants
// UTF-16). PWSTR punning is ABI-safe: both are 16-bit code units.

static constexpr char16_t kTestPath[] =
    u"\\??\\C:\\Windows\\Temp\\llvm_libc_trbench.bin";
static constexpr USHORT kTestPathBytes =
    static_cast<USHORT>(sizeof(kTestPath) - sizeof(char16_t));

// kTestFileSize is defined near MAX_N / OP_LEN above — configurable via
// the BENCH_FILE_SIZE_MIB compile-time flag.

static bool open_test_file(HANDLE *out_handle) {
  UNICODE_STRING us;
  us.Buffer = reinterpret_cast<PWSTR>(const_cast<char16_t *>(kTestPath));
  us.Length = kTestPathBytes;
  us.MaximumLength = kTestPathBytes + sizeof(char16_t);

  OBJECT_ATTRIBUTES oa = {};
  oa.Length = sizeof(oa);
  oa.ObjectName = &us;
  oa.Attributes = OBJ_CASE_INSENSITIVE;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = ::NtCreateFile(
      out_handle,
      FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE_ACCESS, &oa, &iosb,
      nullptr, FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OVERWRITE_IF,
      FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE
          /* overlapped — no SYNCHRONOUS_IO — unlink at last close */,
      nullptr, 0);
  return NT_SUCCESS(s);
}

// Open a second handle to the same file with FILE_NO_INTERMEDIATE_BUFFERING
// for O_DIRECT reads. The buffered handle above populated the file; this
// handle bypasses the page cache on every read. The kernel requires
// sector-aligned offsets, lengths, and buffer addresses — all satisfied
// by OP_LEN=4096 and the alignas(4096) g_bench_buf / registered region.
static bool open_test_file_direct(HANDLE *out_handle) {
  UNICODE_STRING us;
  us.Buffer = reinterpret_cast<PWSTR>(const_cast<char16_t *>(kTestPath));
  us.Length = kTestPathBytes;
  us.MaximumLength = kTestPathBytes + sizeof(char16_t);

  OBJECT_ATTRIBUTES oa = {};
  oa.Length = sizeof(oa);
  oa.ObjectName = &us;
  oa.Attributes = OBJ_CASE_INSENSITIVE;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = ::NtCreateFile(
      out_handle, FILE_GENERIC_READ, &oa, &iosb, nullptr,
      FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
      FILE_NON_DIRECTORY_FILE | FILE_NO_INTERMEDIATE_BUFFERING, nullptr, 0);
  return NT_SUCCESS(s);
}

static bool fill_test_file(HANDLE h) {
  static char pattern[65536];
  for (SIZE_T i = 0; i < sizeof(pattern); ++i)
    pattern[i] = static_cast<char>((i * 131) & 0xFF);

  SIZE_T written = 0;
  while (written < kTestFileSize) {
    ULONG chunk = static_cast<ULONG>(kTestFileSize - written > sizeof(pattern)
                                         ? sizeof(pattern)
                                         : kTestFileSize - written);
    LARGE_INTEGER off;
    off.QuadPart = static_cast<LONGLONG>(written);
    IO_STATUS_BLOCK iosb = {};
    NTSTATUS s = ::NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, pattern,
                               chunk, &off, nullptr);
    if (s == STATUS_PENDING) {
      s = ::NtWaitForSingleObject(h, FALSE, nullptr);
      if (NT_SUCCESS(s))
        s = iosb.Status;
    }
    if (!NT_SUCCESS(s))
      return false;
    written += chunk;
  }
  return true;
}

// Read the whole file through the ThreadRing once. This ensures the file
// cache is warm AND exercises the same code path the bench measures — so
// any first-touch cost (lazy reg-buf, generation init, reactor setup) is
// amortized before any sample is collected.
static void prime_cache(HANDLE file) {
  auto *tr = ioring::get_thread_ring();
  char tmp[65536];
  ULONGLONG off = 0;
  while (off < kTestFileSize) {
    ULONG chunk = static_cast<ULONG>(kTestFileSize - off > sizeof(tmp)
                                         ? sizeof(tmp)
                                         : kTestFileSize - off);
    auto r = ::read_single_op_wait(tr, file, tmp, chunk, off,
                                   tr->next_generation());
    if (r.error || r.value == 0)
      break;
    off += r.value;
  }
}

// Run one O_DIRECT read with use_regbuf=true so ThreadRing::ensure_reg_buf()
// runs off the measurement path. First use lazy-allocates 256 KiB and does
// IORING_OP_REGISTER_BUFFERS — ~microseconds of kernel work that would
// otherwise skew the first O_DIRECT cell's samples.
static void prime_regbuf(HANDLE direct_file) {
  auto *tr = ioring::get_thread_ring();
  ioring::BatchEngine b;
  b.init(tr, direct_file, /*use_regbuf=*/true);
  (void)b.push_read(g_bench_buf, OP_LEN, /*offset=*/0);
  (void)b.submit_and_drain();
}

// Run a few iterations of each strategy through its CfgDefault specialization
// so the engine's per-strategy instruction cache lines are resident before
// the first timed cell. Per-cell WARMUP=200 still covers every (Cfg,Strategy)
// specialization's own icache, but this pre-warm ensures the FIRST sample of
// the FIRST cell doesn't absorb a full icache-cold cost that later cells
// benefit from implicitly.
//
// Also exercises the production path at moderate N so the ThreadRing's
// registered buffer lifecycle (acquire → release → head-wrap) has already
// been driven before any cell starts sampling.
static void prime_batch_paths(HANDLE file, HANDLE direct_file) {
  for (int i = 0; i < 16; ++i) {
    (void)StrategyAutomatic::run<CfgDefault>(file, 8, OP_LEN, false);
    (void)StrategyBatchDrain::run<CfgDefault>(file, 8, OP_LEN, false);
    (void)StrategyWaitPerCqe::run<CfgDefault>(file, 8, OP_LEN, false);
    if (direct_file) {
      (void)StrategyAutomatic::run<CfgDefault>(direct_file, 8, OP_LEN, true);
      (void)StrategyBatchDrain::run<CfgDefault>(direct_file, 8, OP_LEN, true);
      (void)StrategyWaitPerCqe::run<CfgDefault>(direct_file, 8, OP_LEN, true);
    }
  }
}

// ───────────────────────────────────────────────────────────────────────────
// Bench harness
// ───────────────────────────────────────────────────────────────────────────

static constexpr int WARMUP = 200;
static constexpr int SAMPLES = 500;

template <class Strategy, class Cfg>
static void bench_cell(const char *cfg_label, HANDLE file, uint32_t n_ops,
                       ULONG op_len, bool use_regbuf) {
  static int64_t samples[SAMPLES];
  const size_t expected = static_cast<size_t>(n_ops) * op_len;
  int eintr = 0;   // signal delivery — not an engine bug, just noisy hosts
  int bad = 0;     // genuine mismatch — short read, wrong errno, etc.

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    uint64_t t0 = rdtsc();
    ioring::BatchResult r =
        Strategy::template run<Cfg>(file, n_ops, op_len, use_regbuf);
    uint64_t t1 = rdtsc();

    if (r.error == EINTR)
      ++eintr;
    else if (r.error != 0 || r.total_bytes != expected)
      ++bad;

    if (i >= 0)
      samples[i] = static_cast<int64_t>(t1 - t0);
  }

  print_stats(cfg_label, samples, SAMPLES);
  if (eintr > 0 || bad > 0) {
    write_str("    !! ");
    write_i64(eintr);
    write_str(" EINTR, ");
    write_i64(bad);
    write_str(" other mismatches\n");
  }
}

template <class Strategy>
static void bench_strategy_row(HANDLE file, uint32_t n_ops, ULONG op_len,
                               bool use_regbuf) {
  write_str("--- ");
  write_str(Strategy::name);
  write_str(" ---\n");
  bench_cell<Strategy, CfgDefault>   ("  CfgDefault   (QD0=inf, iw=1, SB=4, QD=8) ", file, n_ops, op_len, use_regbuf);
  bench_cell<Strategy, CfgNowait>    ("  CfgNowait    (QD0=inf, iw=0, SB=4, QD=8) ", file, n_ops, op_len, use_regbuf);
  bench_cell<Strategy, CfgUniformQD> ("  CfgUniformQD (QD0=8,   iw=0, SB=4, QD=8) ", file, n_ops, op_len, use_regbuf);
  bench_cell<Strategy, CfgSubmit1>   ("  CfgSubmit1   (QD0=inf, iw=1, SB=1, QD=8) ", file, n_ops, op_len, use_regbuf);
  bench_cell<Strategy, CfgSubmit8>   ("  CfgSubmit8   (QD0=inf, iw=1, SB=8, QD=8) ", file, n_ops, op_len, use_regbuf);
  bench_cell<Strategy, CfgBigQD>     ("  CfgBigQD     (QD0=inf, iw=1, SB=4, QD=16)", file, n_ops, op_len, use_regbuf);
}

static void bench_N(HANDLE file, uint32_t n_ops, ULONG op_len,
                    bool use_regbuf) {
  write_str("\n=== N=");
  write_i64(n_ops);
  write_str(" ops, op_len=");
  write_i64(op_len);
  write_str(" bytes (total=");
  write_i64(static_cast<int64_t>(n_ops) * op_len);
  write_str(" bytes) ===\n");
  bench_strategy_row<StrategyAutomatic>(file, n_ops, op_len, use_regbuf);
  bench_strategy_row<StrategyBatchDrain>(file, n_ops, op_len, use_regbuf);
  bench_strategy_row<StrategyWaitPerCqe>(file, n_ops, op_len, use_regbuf);
}

// ───────────────────────────────────────────────────────────────────────────
// Registered-buffer impact — isolates the MDL-savings dimension
// ───────────────────────────────────────────────────────────────────────────
//
// Holds everything else constant (StrategyAutomatic × CfgDefault) and runs
// each N with use_regbuf=false vs use_regbuf=true. Subtract the two
// median-cycle numbers to get the direct MDL-savings contribution at that
// batch size. All other Cfg/Strategy cells bench with use_regbuf fixed, so
// this block is where the regbuf axis lives.

static void bench_regbuf_impact(HANDLE direct_file) {
  write_str("\n### Registered-buffer impact (O_DIRECT, StrategyAutomatic × "
            "CfgDefault) ###\n");
  static const uint32_t Ns[] = {1, 8, 32, 64};
  for (uint32_t i = 0; i < sizeof(Ns) / sizeof(Ns[0]); ++i) {
    uint32_t n = Ns[i];
    write_str("\n-- N=");
    write_i64(n);
    write_str(" ops, op_len=");
    write_i64(OP_LEN);
    write_str(" --\n");
    bench_cell<StrategyAutomatic, CfgDefault>(
        "  use_regbuf=false (per-op MDL)           ", direct_file, n, OP_LEN,
        /*use_regbuf=*/false);
    bench_cell<StrategyAutomatic, CfgDefault>(
        "  use_regbuf=true  (pre-pinned MDL)       ", direct_file, n, OP_LEN,
        /*use_regbuf=*/true);
  }
}

// ───────────────────────────────────────────────────────────────────────────
// Entry point
// ───────────────────────────────────────────────────────────────────────────

void run_all_benchmarks() {
  g_stdout = ::NtCurrentPeb()->ProcessParameters->StandardOutput;
  init_timer();
  boost_priority();
  pin_thread(NtCurrentThread(), 2);

  write_str("=== ThreadRing / BatchEngineT<Cfg> Benchmark ===\n");
  write_str("TSC freq: ");
  write_i64(g_tsc_freq_khz);
  write_str(" kHz (");
  write_i64(g_tsc_freq_khz / 1000);
  write_str(" MHz). Pinned CPU 2, high-priority.\n");
  write_str("WARMUP=");
  write_i64(WARMUP);
  write_str("  SAMPLES=");
  write_i64(SAMPLES);
  write_str("  file=");
  write_i64(static_cast<int64_t>(kTestFileSize / (1024 * 1024)));
  write_str(" MiB, ");
  write_i64(OP_LEN);
  write_str("-byte ops, sequential-sweep reads (wrap at EOF).\n");

  if (::is_ioring_emulated()) {
    write_str("\n!! IoRing is kernel-emulated on this host — results are\n"
              "   not representative of native IoRing performance.\n");
    // Bench still proceeds; caller interprets.
  }

  HANDLE file = nullptr;
  if (!open_test_file(&file)) {
    write_str("!! open_test_file failed\n");
    return;
  }
  if (!fill_test_file(file)) {
    write_str("!! fill_test_file failed\n");
    ::NtClose(file);
    return;
  }
  prime_cache(file);

  HANDLE direct_file = nullptr;
  bool have_direct = open_test_file_direct(&direct_file);
  if (have_direct)
    prime_regbuf(direct_file);
  else
    write_str("!! open_test_file_direct failed — O_DIRECT sections skipped\n");

  prime_batch_paths(file, have_direct ? direct_file : nullptr);

  // ── Cached (buffered handle, no regbuf) ──
  write_str(
      "\n########  CACHED — buffered handle, use_regbuf=false  ########\n");
  bench_N(file, 1, OP_LEN, /*use_regbuf=*/false);
  bench_N(file, 8, OP_LEN, /*use_regbuf=*/false);
  bench_N(file, 32, OP_LEN, /*use_regbuf=*/false);
  bench_N(file, 64, OP_LEN, /*use_regbuf=*/false);

  if (have_direct) {
    // ── O_DIRECT (unbuffered, registered bounce buffer) ──
    write_str("\n########  O_DIRECT — unbuffered handle, use_regbuf=true  "
              "########\n");
    bench_N(direct_file, 1, OP_LEN, /*use_regbuf=*/true);
    bench_N(direct_file, 8, OP_LEN, /*use_regbuf=*/true);
    bench_N(direct_file, 32, OP_LEN, /*use_regbuf=*/true);
    bench_N(direct_file, 64, OP_LEN, /*use_regbuf=*/true);

    // ── Registered-buffer A/B ──
    bench_regbuf_impact(direct_file);

    ::NtClose(direct_file);
  }

  ::NtClose(file); // DELETE on close — file removed
  write_str("\nDone.\n");
}

} // namespace LIBC_NAMESPACE_DECL

extern "C" int main() {
  LIBC_NAMESPACE::run_all_benchmarks();
  return 0;
}
