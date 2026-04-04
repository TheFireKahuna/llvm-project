//===-- Per-thread IOCP overhead benchmark ---------------------*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Microbenchmark measuring per-thread IOCP creation/destruction overhead
// for the proposed per-thread reactor design. Compares against baseline
// (no per-thread IOCP) and measures all constituent operations.
//
// Test matrix:
//   A. NtCreateIoCompletion cost (bare IOCP creation)
//   B. NtClose(IOCP) cost (IOCP destruction)
//   C. IOCP create + close round-trip
//   D. IOCP + WCP death watch setup (full per-thread overhead)
//   E. IOCP + WCP teardown (normal exit path)
//   F. NtSetIoCompletionEx post cost (writer wake — replaces AlertThread)
//   G. NtRemoveIoCompletionEx dequeue cost (reader wake — non-blocking)
//   H. NtRemoveIoCompletionEx dequeue cost (empty poll)
//   I. Post + dequeue round-trip latency (same thread)
//   J. Post + dequeue round-trip latency (cross-thread, cold path)
//   K. NtAlertThreadByThreadId cost (current cold-path wake for comparison)
//   L. NtWaitForAlertByThreadId + alert round-trip (current design)
//   M. NtAllocateReserveObject cost (per-thread reserve)
//   N. Scaled: create N threads, each with IOCP + reserve + WCP, teardown all
//   O. Scaled: create N threads, baseline (no IOCP), teardown all
//
// Hosted build — links against llvm-libc (crt1.obj + c.dll).
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/ntdll.h"

namespace LIBC_NAMESPACE_DECL {

// ---- Output ----

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

// ---- Timing (rdtsc cycles for precision, calibrated to ns for display) ----

static int64_t tsc_freq_khz; // TSC ticks per millisecond

// Read the TSC. ~10ns resolution on modern x86, vs 100ns for QPC.
// RDTSCP serializes — no reordering past the read.
static inline uint64_t rdtsc() { return __builtin_ia32_rdtsc(); }

// Calibrate TSC frequency by measuring TSC ticks over a QPC-timed interval.
static void init_timer() {
  LARGE_INTEGER qpc_freq, qpc0, qpc1;
  ::RtlQueryPerformanceFrequency(&qpc_freq);

  // Spin for ~10ms to calibrate.
  ::RtlQueryPerformanceCounter(&qpc0);
  uint64_t tsc0 = rdtsc();
  LARGE_INTEGER delay;
  delay.QuadPart = -100000; // 10ms
  ::NtDelayExecution(FALSE, &delay);
  uint64_t tsc1 = rdtsc();
  ::RtlQueryPerformanceCounter(&qpc1);

  int64_t qpc_elapsed = qpc1.QuadPart - qpc0.QuadPart;
  int64_t tsc_elapsed = static_cast<int64_t>(tsc1 - tsc0);

  // tsc_freq_khz = tsc_ticks / ms = tsc_elapsed / (qpc_elapsed / qpc_freq * 1000)
  //              = tsc_elapsed * qpc_freq / (qpc_elapsed * 1000)
  tsc_freq_khz = tsc_elapsed * qpc_freq.QuadPart / (qpc_elapsed * 1000);
}

// Convert TSC delta to nanoseconds.
static inline int64_t tsc_to_ns(int64_t tsc_delta) {
  // ns = tsc_delta * 1000000 / tsc_freq_khz
  return tsc_delta * 1000000LL / tsc_freq_khz;
}



// ---- Sort & Stats ----

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

// Print stats. Samples are in TSC cycles — convert to ns for display.
// Shows both cycles and ns for transparency.
static void print_stats(const char *label, int64_t *samples, int n,
                        bool is_ns = false) {
  insertion_sort(samples, n);
  int64_t sum = 0;
  for (int i = 0; i < n; ++i)
    sum += samples[i];

  write_padded(label, 46);
  if (is_ns) {
    // Already in ns (for cross-thread with absolute timestamps).
    write_str("  min=");
    write_i64(samples[0]);
    write_str("  p50=");
    write_i64(samples[n / 2]);
    write_str("  p95=");
    write_i64(samples[n * 95 / 100]);
    write_str("  avg=");
    write_i64(sum / n);
    write_str(" ns\n");
  } else {
    // TSC cycles — show both.
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
}

// ---- Affinity / priority ----

static void pin_thread(HANDLE thread, int lp) {
  ULONG_PTR mask = 1ull << lp;
  ::NtSetInformationThread(thread, 4 /*ThreadAffinityMask*/, &mask,
                           sizeof(mask));
}

static void boost_priority() {
  struct {
    BOOLEAN Foreground;
    UCHAR PriorityClass;
  } ppc = {FALSE, 3};
  ::NtSetInformationProcess(NtCurrentProcess(), 18 /*ProcessPriorityClass*/,
                            &ppc, sizeof(ppc));
}

// ---- Constants ----

inline constexpr ACCESS_MASK IO_COMPLETION_ALL_ACCESS = 0x001F0003;
inline constexpr ACCESS_MASK WAIT_COMPLETION_PACKET_ALL_ACCESS = 0x00000001;
inline constexpr MEMORY_RESERVE_TYPE MemoryReserveIoCompletion =
    static_cast<MEMORY_RESERVE_TYPE>(1);
static constexpr int SAMPLES = 1000;
static constexpr int WARMUP = 100;

// Create an IOCP + paired reserve object (the per-thread reactor pair).
// concurrency: 0 = unlimited, 1 = single consumer (per-thread IOCP).
static bool create_iocp_pair(HANDLE &iocp, HANDLE &reserve,
                             ULONG concurrency = 1) {
  iocp = nullptr;
  reserve = nullptr;
  NTSTATUS st =
      ::NtCreateIoCompletion(&iocp, IO_COMPLETION_ALL_ACCESS, nullptr,
                             concurrency);
  if (!NT_SUCCESS(st))
    return false;
  st = ::NtAllocateReserveObject(&reserve, nullptr,
                                 MemoryReserveIoCompletion);
  if (!NT_SUCCESS(st)) {
    ::NtClose(iocp);
    iocp = nullptr;
    return false;
  }
  return true;
}

// Post a completion using the guaranteed-delivery reserve path.
static NTSTATUS post_completion(HANDLE iocp, HANDLE reserve, PVOID key) {
  return ::NtSetIoCompletionEx(iocp, reserve, key, nullptr, STATUS_SUCCESS, 0);
}

// ---- Thread creation helper ----

// Thread-start routine invoked by the kernel under MS x64 ABI.
// `decltype` on an unreferenced external prototype — LIBC_MSABI attaches
// to the declaration, not to a type alias (trips `-Wgcc-compat`).
LIBC_MSABI DWORD __nt_thread_start_type_source(void *);
using NtThreadStartFn =
    decltype(__nt_thread_start_type_source); // function type, not pointer

static HANDLE create_suspended_thread(NtThreadStartFn *start, void *arg,
                                      DWORD *out_tid) {
  HANDLE h = nullptr;
  CLIENT_ID cid = {};
  struct {
    SIZE_T TotalLength;
    PS_ATTRIBUTE Attributes[1];
  } attr_list = {};
  attr_list.TotalLength = sizeof(SIZE_T) + sizeof(PS_ATTRIBUTE);
  attr_list.Attributes[0].Attribute = PS_ATTRIBUTE_CLIENT_ID;
  attr_list.Attributes[0].Size = sizeof(cid);
  attr_list.Attributes[0].ValuePtr = &cid;

  NTSTATUS st = ::NtCreateThreadEx(
      &h, THREAD_ALL_ACCESS, nullptr, NtCurrentProcess(),
      reinterpret_cast<PVOID>(start), arg,
      THREAD_CREATE_FLAGS_CREATE_SUSPENDED, 0, 0, 0,
      reinterpret_cast<PS_ATTRIBUTE_LIST *>(&attr_list));
  if (!NT_SUCCESS(st))
    return nullptr;
  if (out_tid)
    *out_tid =
        static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(cid.UniqueThread));
  return h;
}

static HANDLE create_running_thread(NtThreadStartFn *start, void *arg,
                                    DWORD *out_tid) {
  HANDLE h = nullptr;
  CLIENT_ID cid = {};
  struct {
    SIZE_T TotalLength;
    PS_ATTRIBUTE Attributes[1];
  } attr_list = {};
  attr_list.TotalLength = sizeof(SIZE_T) + sizeof(PS_ATTRIBUTE);
  attr_list.Attributes[0].Attribute = PS_ATTRIBUTE_CLIENT_ID;
  attr_list.Attributes[0].Size = sizeof(cid);
  attr_list.Attributes[0].ValuePtr = &cid;

  NTSTATUS st = ::NtCreateThreadEx(
      &h, THREAD_ALL_ACCESS, nullptr, NtCurrentProcess(),
      reinterpret_cast<PVOID>(start), arg, 0, 0, 0, 0,
      reinterpret_cast<PS_ATTRIBUTE_LIST *>(&attr_list));
  if (!NT_SUCCESS(st))
    return nullptr;
  if (out_tid)
    *out_tid =
        static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(cid.UniqueThread));
  return h;
}

NTAPI static DWORD park_forever(void *) {
  ::NtWaitForSingleObject(NtCurrentProcess(), FALSE, nullptr);
  return 0;
}

// ========================================================================
// Bench A: NtCreateIoCompletion (bare IOCP creation)
// ========================================================================

static void bench_iocp_create() {
  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    uint64_t t0 = rdtsc();
    HANDLE iocp = nullptr;
    ::NtCreateIoCompletion(&iocp, IO_COMPLETION_ALL_ACCESS, nullptr, 1);
    uint64_t t1 = rdtsc();

    if (iocp)
      ::NtClose(iocp);

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  print_stats("IOCP create (NtCreateIoCompletion)", samples, SAMPLES);
}

// ========================================================================
// Bench B: NtClose(IOCP) (IOCP destruction)
// ========================================================================

static void bench_iocp_close() {
  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    HANDLE iocp = nullptr;
    ::NtCreateIoCompletion(&iocp, IO_COMPLETION_ALL_ACCESS, nullptr, 1);

    uint64_t t0 = rdtsc();
    ::NtClose(iocp);
    uint64_t t1 = rdtsc();

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  print_stats("IOCP close (NtClose)", samples, SAMPLES);
}

// ========================================================================
// Bench C: IOCP create + close round-trip
// ========================================================================

static void bench_iocp_create_close() {
  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    uint64_t t0 = rdtsc();

    HANDLE iocp = nullptr;
    ::NtCreateIoCompletion(&iocp, IO_COMPLETION_ALL_ACCESS, nullptr, 1);
    ::NtClose(iocp);

    uint64_t t1 = rdtsc();

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  print_stats("IOCP create+close round-trip", samples, SAMPLES);
}

// ========================================================================
// Bench D: Full per-thread setup (IOCP + reserve + WCP death watch)
//   What pthread_create would pay in the new design.
// ========================================================================

static void bench_full_per_thread_setup() {
  // Process-wide reactor IOCP (for death watches).
  HANDLE reactor_iocp = nullptr;
  ::NtCreateIoCompletion(&reactor_iocp, IO_COMPLETION_ALL_ACCESS, nullptr, 0);

  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    HANDLE th = create_suspended_thread(park_forever, nullptr, nullptr);
    if (!th)
      continue;

    uint64_t t0 = rdtsc();

    // 1. Create per-thread IOCP + reserve object.
    HANDLE per_thread_iocp = nullptr;
    HANDLE per_thread_reserve = nullptr;
    create_iocp_pair(per_thread_iocp, per_thread_reserve);

    // 2. Create WCP + associate with reactor IOCP for death watch.
    HANDLE wcp = nullptr;
    ::NtCreateWaitCompletionPacket(&wcp, WAIT_COMPLETION_PACKET_ALL_ACCESS,
                                   nullptr);
    BOOLEAN already = FALSE;
    ::NtAssociateWaitCompletionPacket(wcp, reactor_iocp, th,
                                      reinterpret_cast<PVOID>(0xDEAD),
                                      nullptr, STATUS_SUCCESS, 0, &already);

    uint64_t t1 = rdtsc();

    // Cleanup.
    ::NtCancelWaitCompletionPacket(wcp, TRUE);
    ::NtClose(wcp);
    ::NtClose(per_thread_reserve);
    ::NtClose(per_thread_iocp);
    ::NtTerminateThread(th, 0);
    ::NtClose(th);

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  ::NtClose(reactor_iocp);
  print_stats("Full per-thread setup (IOCP+reserve+WCP)", samples, SAMPLES);
}

// ========================================================================
// Bench E: Full per-thread teardown (normal exit path)
//   What lifecycle_cleanup would pay: cancel WCP + close per-thread IOCP.
// ========================================================================

static void bench_full_per_thread_teardown() {
  HANDLE reactor_iocp = nullptr;
  ::NtCreateIoCompletion(&reactor_iocp, IO_COMPLETION_ALL_ACCESS, nullptr, 0);

  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    HANDLE th = create_suspended_thread(park_forever, nullptr, nullptr);
    if (!th)
      continue;

    // Setup.
    HANDLE per_thread_iocp = nullptr;
    HANDLE per_thread_reserve = nullptr;
    create_iocp_pair(per_thread_iocp, per_thread_reserve);
    HANDLE wcp = nullptr;
    ::NtCreateWaitCompletionPacket(&wcp, WAIT_COMPLETION_PACKET_ALL_ACCESS,
                                   nullptr);
    BOOLEAN already = FALSE;
    ::NtAssociateWaitCompletionPacket(wcp, reactor_iocp, th,
                                      reinterpret_cast<PVOID>(0xDEAD),
                                      nullptr, STATUS_SUCCESS, 0, &already);

    // Measure teardown (what lifecycle_cleanup pays on normal exit).
    uint64_t t0 = rdtsc();
    ::NtCancelWaitCompletionPacket(wcp, TRUE);
    ::NtClose(wcp);
    ::NtClose(per_thread_reserve);
    ::NtClose(per_thread_iocp);
    uint64_t t1 = rdtsc();

    ::NtTerminateThread(th, 0);
    ::NtClose(th);

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  ::NtClose(reactor_iocp);
  print_stats("Full per-thread teardown (cancel+close*3)", samples, SAMPLES);
}

// ========================================================================
// Bench F: NtSetIoCompletionEx post cost (writer wake)
// ========================================================================

static void bench_iocp_post() {
  HANDLE iocp = nullptr, reserve = nullptr;
  create_iocp_pair(iocp, reserve);

  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    uint64_t t0 = rdtsc();
    post_completion(iocp, reserve, reinterpret_cast<PVOID>(0x1));
    uint64_t t1 = rdtsc();

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  // Drain queued completions.
  for (int d = 0; d < WARMUP + SAMPLES; ++d) {
    FILE_IO_COMPLETION_INFORMATION info = {};
    ULONG removed = 0;
    LARGE_INTEGER zero = {};
    ::NtRemoveIoCompletionEx(iocp, &info, 1, &removed, &zero, FALSE);
  }

  ::NtClose(reserve);
  ::NtClose(iocp);
  print_stats("NtSetIoCompletionEx (post, reserved)", samples, SAMPLES);
}

// ========================================================================
// Bench G: NtRemoveIoCompletionEx non-blocking dequeue (pre-posted)
// ========================================================================

static void bench_iocp_dequeue_nonblocking() {
  HANDLE iocp = nullptr, reserve = nullptr;
  create_iocp_pair(iocp, reserve);

  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    // Pre-post a completion.
    post_completion(iocp, reserve, reinterpret_cast<PVOID>(0x1));

    FILE_IO_COMPLETION_INFORMATION info = {};
    ULONG removed = 0;
    LARGE_INTEGER zero = {};

    uint64_t t0 = rdtsc();
    ::NtRemoveIoCompletionEx(iocp, &info, 1, &removed, &zero, FALSE);
    uint64_t t1 = rdtsc();

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  ::NtClose(reserve);
  ::NtClose(iocp);
  print_stats("NtRemoveIoCompletionEx (non-blocking)", samples, SAMPLES);
}

// ========================================================================
// Bench H: NtRemoveIoCompletionEx empty (non-blocking, nothing queued)
//   Measures the cost of a "check if anything pending" poll.
// ========================================================================

static void bench_iocp_dequeue_empty() {
  HANDLE iocp = nullptr, reserve = nullptr;
  create_iocp_pair(iocp, reserve);

  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    FILE_IO_COMPLETION_INFORMATION info = {};
    ULONG removed = 0;
    LARGE_INTEGER zero = {};

    uint64_t t0 = rdtsc();
    ::NtRemoveIoCompletionEx(iocp, &info, 1, &removed, &zero, FALSE);
    uint64_t t1 = rdtsc();

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  ::NtClose(reserve);
  ::NtClose(iocp);
  print_stats("NtRemoveIoCompletionEx (empty, non-block)", samples, SAMPLES);
}

// ========================================================================
// Bench I: Post + dequeue round-trip (same thread)
// ========================================================================

static void bench_iocp_post_dequeue_same_thread() {
  HANDLE iocp = nullptr, reserve = nullptr;
  create_iocp_pair(iocp, reserve);

  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    uint64_t t0 = rdtsc();

    post_completion(iocp, reserve, reinterpret_cast<PVOID>(0x1));

    FILE_IO_COMPLETION_INFORMATION info = {};
    ULONG removed = 0;
    LARGE_INTEGER zero = {};
    ::NtRemoveIoCompletionEx(iocp, &info, 1, &removed, &zero, FALSE);

    uint64_t t1 = rdtsc();

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  ::NtClose(reserve);
  ::NtClose(iocp);
  print_stats("Post+dequeue same-thread round-trip", samples, SAMPLES);
}

// ========================================================================
// Bench F-I variants: concurrency=0 (unlimited) for comparison
// ========================================================================

static void bench_iocp_post_c0() {
  HANDLE iocp = nullptr, reserve = nullptr;
  create_iocp_pair(iocp, reserve, 0);
  int64_t samples[SAMPLES];
  for (int i = -WARMUP; i < SAMPLES; ++i) {
    uint64_t t0 = rdtsc();
    post_completion(iocp, reserve, reinterpret_cast<PVOID>(0x1));
    uint64_t t1 = rdtsc();
    if (i >= 0) samples[i] = t1 - t0;
  }
  for (int d = 0; d < WARMUP + SAMPLES; ++d) {
    FILE_IO_COMPLETION_INFORMATION info = {};
    ULONG removed = 0;
    LARGE_INTEGER zero = {};
    ::NtRemoveIoCompletionEx(iocp, &info, 1, &removed, &zero, FALSE);
  }
  ::NtClose(reserve); ::NtClose(iocp);
  print_stats("NtSetIoCompletionEx (post, c=0)", samples, SAMPLES);
}

static void bench_iocp_dequeue_nonblocking_c0() {
  HANDLE iocp = nullptr, reserve = nullptr;
  create_iocp_pair(iocp, reserve, 0);
  int64_t samples[SAMPLES];
  for (int i = -WARMUP; i < SAMPLES; ++i) {
    post_completion(iocp, reserve, reinterpret_cast<PVOID>(0x1));
    FILE_IO_COMPLETION_INFORMATION info = {};
    ULONG removed = 0;
    LARGE_INTEGER zero = {};
    uint64_t t0 = rdtsc();
    ::NtRemoveIoCompletionEx(iocp, &info, 1, &removed, &zero, FALSE);
    uint64_t t1 = rdtsc();
    if (i >= 0) samples[i] = t1 - t0;
  }
  ::NtClose(reserve); ::NtClose(iocp);
  print_stats("NtRemoveIoCompletionEx (non-block, c=0)", samples, SAMPLES);
}

static void bench_iocp_dequeue_empty_c0() {
  HANDLE iocp = nullptr, reserve = nullptr;
  create_iocp_pair(iocp, reserve, 0);
  int64_t samples[SAMPLES];
  for (int i = -WARMUP; i < SAMPLES; ++i) {
    FILE_IO_COMPLETION_INFORMATION info = {};
    ULONG removed = 0;
    LARGE_INTEGER zero = {};
    uint64_t t0 = rdtsc();
    ::NtRemoveIoCompletionEx(iocp, &info, 1, &removed, &zero, FALSE);
    uint64_t t1 = rdtsc();
    if (i >= 0) samples[i] = t1 - t0;
  }
  ::NtClose(reserve); ::NtClose(iocp);
  print_stats("NtRemoveIoCompletionEx (empty, c=0)", samples, SAMPLES);
}

static void bench_iocp_post_dequeue_same_thread_c0() {
  HANDLE iocp = nullptr, reserve = nullptr;
  create_iocp_pair(iocp, reserve, 0);
  int64_t samples[SAMPLES];
  for (int i = -WARMUP; i < SAMPLES; ++i) {
    uint64_t t0 = rdtsc();
    post_completion(iocp, reserve, reinterpret_cast<PVOID>(0x1));
    FILE_IO_COMPLETION_INFORMATION info = {};
    ULONG removed = 0;
    LARGE_INTEGER zero = {};
    ::NtRemoveIoCompletionEx(iocp, &info, 1, &removed, &zero, FALSE);
    uint64_t t1 = rdtsc();
    if (i >= 0) samples[i] = t1 - t0;
  }
  ::NtClose(reserve); ::NtClose(iocp);
  print_stats("Post+dequeue same-thread (c=0)", samples, SAMPLES);
}

// ========================================================================
// Bench J: Post + dequeue cross-thread latency
//   Thread B blocks on IOCP, thread A posts, measure wake latency.
// ========================================================================

struct CrossThreadArg {
  HANDLE iocp;
  HANDLE reserve;
  cpp::Atomic<uint64_t> wake_tsc{0};
  cpp::Atomic<int> phase{0}; // 0=init, 1=waiting, 2=woken, 3=done
};

NTAPI static DWORD cross_thread_waiter(void *arg) {
  auto *ct = static_cast<CrossThreadArg *>(arg);

  for (;;) {
    // Signal that we're about to wait.
    ct->phase.store(1, cpp::MemoryOrder::RELEASE);

    FILE_IO_COMPLETION_INFORMATION info = {};
    ULONG removed = 0;
    ::NtRemoveIoCompletionEx(ct->iocp, &info, 1, &removed, nullptr, FALSE);

    ct->wake_tsc.store(rdtsc(), cpp::MemoryOrder::RELAXED);
    ct->phase.store(2, cpp::MemoryOrder::RELEASE);

    // Wait for main to consume the result.
    while (ct->phase.load(cpp::MemoryOrder::ACQUIRE) == 2) {
      LARGE_INTEGER delay;
      delay.QuadPart = -100; // 10us
      ::NtDelayExecution(FALSE, &delay);
    }

    if (ct->phase.load(cpp::MemoryOrder::ACQUIRE) == 99)
      break;
  }

  return 0;
}

static void bench_iocp_cross_thread_latency() {
  HANDLE iocp = nullptr, reserve = nullptr;
  create_iocp_pair(iocp, reserve);

  CrossThreadArg ct;
  ct.iocp = iocp;
  ct.reserve = reserve;

  HANDLE th = create_running_thread(cross_thread_waiter, &ct, nullptr);
  if (!th) {
    write_str("  SKIP: thread creation failed\n");
    ::NtClose(iocp);
    return;
  }
  // Pin waiter to a different core.
  pin_thread(th, 4);

  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    // Wait for waiter to be blocking on IOCP.
    while (ct.phase.load(cpp::MemoryOrder::ACQUIRE) != 1) {
      LARGE_INTEGER delay;
      delay.QuadPart = -100;
      ::NtDelayExecution(FALSE, &delay);
    }
    // Small settle delay.
    LARGE_INTEGER delay;
    delay.QuadPart = -500; // 50us
    ::NtDelayExecution(FALSE, &delay);

    uint64_t t0 = rdtsc();
    post_completion(iocp, reserve, reinterpret_cast<PVOID>(0x1));

    // Wait for waiter to record wake time.
    while (ct.phase.load(cpp::MemoryOrder::ACQUIRE) != 2) {
      // Spin — tight loop for accurate measurement.
    }
    uint64_t t1 = ct.wake_tsc.load(cpp::MemoryOrder::RELAXED);

    // Reset for next iteration.
    ct.phase.store(0, cpp::MemoryOrder::RELEASE);

    if (i >= 0)
      samples[i] = static_cast<int64_t>(t1 - t0); // TSC delta
  }

  // Signal waiter to exit.
  ct.phase.store(99, cpp::MemoryOrder::RELEASE);
  post_completion(iocp, reserve, reinterpret_cast<PVOID>(0x1));
  ::NtWaitForSingleObject(th, FALSE, nullptr);
  ::NtClose(th);
  ::NtClose(reserve);
  ::NtClose(iocp);

  print_stats("IOCP cross-thread (c=1)", samples, SAMPLES);
}

static void bench_iocp_cross_thread_latency_c0() {
  HANDLE iocp = nullptr, reserve = nullptr;
  create_iocp_pair(iocp, reserve, 0);

  CrossThreadArg ct;
  ct.iocp = iocp;
  ct.reserve = reserve;

  HANDLE th = create_running_thread(cross_thread_waiter, &ct, nullptr);
  if (!th) {
    write_str("  SKIP: thread creation failed\n");
    ::NtClose(reserve); ::NtClose(iocp);
    return;
  }
  pin_thread(th, 4);

  int64_t samples[SAMPLES];
  for (int i = -WARMUP; i < SAMPLES; ++i) {
    while (ct.phase.load(cpp::MemoryOrder::ACQUIRE) != 1) {
      LARGE_INTEGER delay; delay.QuadPart = -100;
      ::NtDelayExecution(FALSE, &delay);
    }
    LARGE_INTEGER delay; delay.QuadPart = -500;
    ::NtDelayExecution(FALSE, &delay);

    uint64_t t0 = rdtsc();
    post_completion(iocp, reserve, reinterpret_cast<PVOID>(0x1));
    while (ct.phase.load(cpp::MemoryOrder::ACQUIRE) != 2) {}
    uint64_t t1 = ct.wake_tsc.load(cpp::MemoryOrder::RELAXED);

    ct.phase.store(0, cpp::MemoryOrder::RELEASE);
    if (i >= 0) samples[i] = static_cast<int64_t>(t1 - t0);
  }

  ct.phase.store(99, cpp::MemoryOrder::RELEASE);
  post_completion(iocp, reserve, reinterpret_cast<PVOID>(0x1));
  ::NtWaitForSingleObject(th, FALSE, nullptr);
  ::NtClose(th);
  ::NtClose(reserve); ::NtClose(iocp);
  print_stats("IOCP cross-thread (c=0)", samples, SAMPLES);
}

// ========================================================================
// Bench K: NtAlertThreadByThreadId cost (current design comparison)
// ========================================================================

struct AlertArg {
  cpp::Atomic<uint32_t> value{0};
  cpp::Atomic<uint64_t> wake_tsc{0};
  cpp::Atomic<int> phase{0};
};

NTAPI static DWORD alert_waiter(void *arg) {
  auto *aa = static_cast<AlertArg *>(arg);

  for (;;) {
    aa->phase.store(1, cpp::MemoryOrder::RELEASE);

    ::NtWaitForAlertByThreadId(&aa->value, nullptr);

    aa->wake_tsc.store(rdtsc(), cpp::MemoryOrder::RELAXED);
    aa->phase.store(2, cpp::MemoryOrder::RELEASE);

    while (aa->phase.load(cpp::MemoryOrder::ACQUIRE) == 2) {
      LARGE_INTEGER delay;
      delay.QuadPart = -100;
      ::NtDelayExecution(FALSE, &delay);
    }

    if (aa->phase.load(cpp::MemoryOrder::ACQUIRE) == 99)
      break;
  }

  return 0;
}

static void bench_alert_cross_thread_latency() {
  AlertArg aa;
  DWORD waiter_tid = 0;

  HANDLE th = create_running_thread(alert_waiter, &aa, &waiter_tid);
  if (!th) {
    write_str("  SKIP: thread creation failed\n");
    return;
  }
  pin_thread(th, 4);

  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    while (aa.phase.load(cpp::MemoryOrder::ACQUIRE) != 1) {
      LARGE_INTEGER delay;
      delay.QuadPart = -100;
      ::NtDelayExecution(FALSE, &delay);
    }
    LARGE_INTEGER delay;
    delay.QuadPart = -500;
    ::NtDelayExecution(FALSE, &delay);

    uint64_t t0 = rdtsc();
    ::NtAlertThreadByThreadId(
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(waiter_tid)));

    while (aa.phase.load(cpp::MemoryOrder::ACQUIRE) != 2) {
      // Spin.
    }
    uint64_t t1 = aa.wake_tsc.load(cpp::MemoryOrder::RELAXED);

    aa.phase.store(0, cpp::MemoryOrder::RELEASE);

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  aa.phase.store(99, cpp::MemoryOrder::RELEASE);
  ::NtAlertThreadByThreadId(
      reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(waiter_tid)));
  ::NtWaitForSingleObject(th, FALSE, nullptr);
  ::NtClose(th);

  print_stats("AlertThread cross-thread wake latency", samples, SAMPLES);
}

// ========================================================================
// Bench L: NtAlertThreadByThreadId post cost (writer side only)
// ========================================================================

static void bench_alert_post_cost() {
  // Alert the current thread (self-alert) — measures syscall overhead.
  // Self-alert sets the alert flag; consumed by next NtWaitForAlertByThreadId.
  DWORD self_tid = NtCurrentThreadId();
  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    uint64_t t0 = rdtsc();
    ::NtAlertThreadByThreadId(
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(self_tid)));
    uint64_t t1 = rdtsc();

    // Consume the alert.
    LARGE_INTEGER zero = {};
    ::NtWaitForAlertByThreadId(nullptr, &zero);

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  print_stats("NtAlertThreadByThreadId (post)", samples, SAMPLES);
}

// ========================================================================
// Bench M: NtAllocateReserveObject cost
// ========================================================================

static void bench_reserve_alloc() {
  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    uint64_t t0 = rdtsc();
    HANDLE reserve = nullptr;
    ::NtAllocateReserveObject(&reserve, nullptr, MemoryReserveIoCompletion);
    uint64_t t1 = rdtsc();

    if (reserve)
      ::NtClose(reserve);

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  print_stats("NtAllocateReserveObject", samples, SAMPLES);
}

// ========================================================================
// Bench: WCP associate-only cost (pooled — no create/close)
// ========================================================================

static void bench_wcp_associate_only() {
  HANDLE reactor_iocp = nullptr;
  ::NtCreateIoCompletion(&reactor_iocp, IO_COMPLETION_ALL_ACCESS, nullptr, 0);

  // Pre-create the WCP (simulating a pool).
  HANDLE wcp = nullptr;
  ::NtCreateWaitCompletionPacket(&wcp, WAIT_COMPLETION_PACKET_ALL_ACCESS,
                                 nullptr);

  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    HANDLE th = create_suspended_thread(park_forever, nullptr, nullptr);
    if (!th)
      continue;

    uint64_t t0 = rdtsc();
    BOOLEAN already = FALSE;
    ::NtAssociateWaitCompletionPacket(wcp, reactor_iocp, th,
                                      reinterpret_cast<PVOID>(0xDEAD),
                                      nullptr, STATUS_SUCCESS, 0, &already);
    uint64_t t1 = rdtsc();

    ::NtCancelWaitCompletionPacket(wcp, TRUE);
    ::NtTerminateThread(th, 0);
    ::NtClose(th);

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  ::NtClose(wcp);
  ::NtClose(reactor_iocp);
  print_stats("WCP associate only (pooled handle)", samples, SAMPLES);
}

// ========================================================================
// Bench: WCP cancel-only cost (pooled — return to pool)
// ========================================================================

static void bench_wcp_cancel_only() {
  HANDLE reactor_iocp = nullptr;
  ::NtCreateIoCompletion(&reactor_iocp, IO_COMPLETION_ALL_ACCESS, nullptr, 0);

  HANDLE wcp = nullptr;
  ::NtCreateWaitCompletionPacket(&wcp, WAIT_COMPLETION_PACKET_ALL_ACCESS,
                                 nullptr);

  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    HANDLE th = create_suspended_thread(park_forever, nullptr, nullptr);
    if (!th)
      continue;

    BOOLEAN already = FALSE;
    ::NtAssociateWaitCompletionPacket(wcp, reactor_iocp, th,
                                      reinterpret_cast<PVOID>(0xDEAD),
                                      nullptr, STATUS_SUCCESS, 0, &already);

    uint64_t t0 = rdtsc();
    ::NtCancelWaitCompletionPacket(wcp, TRUE);
    uint64_t t1 = rdtsc();

    ::NtTerminateThread(th, 0);
    ::NtClose(th);

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  ::NtClose(wcp);
  ::NtClose(reactor_iocp);
  print_stats("WCP cancel only (return to pool)", samples, SAMPLES);
}

// ========================================================================
// Bench: Full per-thread setup POOLED (IOCP + reserve + associate pooled WCP)
// ========================================================================

static void bench_full_per_thread_setup_pooled() {
  HANDLE reactor_iocp = nullptr;
  ::NtCreateIoCompletion(&reactor_iocp, IO_COMPLETION_ALL_ACCESS, nullptr, 0);

  // Pre-created WCP pool (one per iteration, simulating reuse).
  HANDLE wcp = nullptr;
  ::NtCreateWaitCompletionPacket(&wcp, WAIT_COMPLETION_PACKET_ALL_ACCESS,
                                 nullptr);

  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    HANDLE th = create_suspended_thread(park_forever, nullptr, nullptr);
    if (!th)
      continue;

    uint64_t t0 = rdtsc();

    // Per-thread IOCP + reserve.
    HANDLE per_thread_iocp = nullptr;
    HANDLE per_thread_reserve = nullptr;
    create_iocp_pair(per_thread_iocp, per_thread_reserve);

    // Associate pooled WCP with thread handle.
    BOOLEAN already = FALSE;
    ::NtAssociateWaitCompletionPacket(wcp, reactor_iocp, th,
                                      reinterpret_cast<PVOID>(0xDEAD),
                                      nullptr, STATUS_SUCCESS, 0, &already);

    uint64_t t1 = rdtsc();

    // Cleanup.
    ::NtCancelWaitCompletionPacket(wcp, TRUE);
    ::NtClose(per_thread_reserve);
    ::NtClose(per_thread_iocp);
    ::NtTerminateThread(th, 0);
    ::NtClose(th);

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  ::NtClose(wcp);
  ::NtClose(reactor_iocp);
  print_stats("Full setup POOLED (IOCP+rsv+assoc)", samples, SAMPLES);
}

// ========================================================================
// Bench: Full per-thread teardown POOLED (cancel + close IOCP + close reserve)
// ========================================================================

static void bench_full_per_thread_teardown_pooled() {
  HANDLE reactor_iocp = nullptr;
  ::NtCreateIoCompletion(&reactor_iocp, IO_COMPLETION_ALL_ACCESS, nullptr, 0);

  HANDLE wcp = nullptr;
  ::NtCreateWaitCompletionPacket(&wcp, WAIT_COMPLETION_PACKET_ALL_ACCESS,
                                 nullptr);

  int64_t samples[SAMPLES];

  for (int i = -WARMUP; i < SAMPLES; ++i) {
    HANDLE th = create_suspended_thread(park_forever, nullptr, nullptr);
    if (!th)
      continue;

    HANDLE per_thread_iocp = nullptr;
    HANDLE per_thread_reserve = nullptr;
    create_iocp_pair(per_thread_iocp, per_thread_reserve);
    BOOLEAN already = FALSE;
    ::NtAssociateWaitCompletionPacket(wcp, reactor_iocp, th,
                                      reinterpret_cast<PVOID>(0xDEAD),
                                      nullptr, STATUS_SUCCESS, 0, &already);

    uint64_t t0 = rdtsc();
    ::NtCancelWaitCompletionPacket(wcp, TRUE); // return WCP to pool (no close)
    ::NtClose(per_thread_reserve);
    ::NtClose(per_thread_iocp);
    uint64_t t1 = rdtsc();

    ::NtTerminateThread(th, 0);
    ::NtClose(th);

    if (i >= 0)
      samples[i] = t1 - t0;
  }

  ::NtClose(wcp);
  ::NtClose(reactor_iocp);
  print_stats("Full teardown POOLED (cancel+close*2)", samples, SAMPLES);
}

// ========================================================================
// Scaled — N threads with IOCP + reserve + WCP
// ========================================================================

static constexpr int SCALE_N = 64;
static constexpr int SCALE_SAMPLES = 50;

static void bench_scaled_with_iocp() {
  HANDLE reactor_iocp = nullptr;
  ::NtCreateIoCompletion(&reactor_iocp, IO_COMPLETION_ALL_ACCESS, nullptr, 0);

  int64_t create_samples[SCALE_SAMPLES];
  int64_t teardown_samples[SCALE_SAMPLES];

  for (int iter = 0; iter < SCALE_SAMPLES; ++iter) {
    HANDLE threads[SCALE_N];
    HANDLE iocps[SCALE_N];
    HANDLE reserves[SCALE_N];
    HANDLE wcps[SCALE_N];

    // Measure creation.
    uint64_t t0 = rdtsc();
    for (int j = 0; j < SCALE_N; ++j) {
      threads[j] = create_suspended_thread(park_forever, nullptr, nullptr);
      iocps[j] = nullptr;
      reserves[j] = nullptr;
      wcps[j] = nullptr;
      if (threads[j]) {
        create_iocp_pair(iocps[j], reserves[j]);
        ::NtCreateWaitCompletionPacket(&wcps[j],
                                       WAIT_COMPLETION_PACKET_ALL_ACCESS,
                                       nullptr);
        BOOLEAN already = FALSE;
        ::NtAssociateWaitCompletionPacket(
            wcps[j], reactor_iocp, threads[j],
            reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(j)), nullptr,
            STATUS_SUCCESS, 0, &already);
      }
    }
    uint64_t t1 = rdtsc();
    create_samples[iter] = t1 - t0;

    // Measure teardown.
    uint64_t t2 = rdtsc();
    for (int j = 0; j < SCALE_N; ++j) {
      if (wcps[j]) {
        ::NtCancelWaitCompletionPacket(wcps[j], TRUE);
        ::NtClose(wcps[j]);
      }
      if (reserves[j])
        ::NtClose(reserves[j]);
      if (iocps[j])
        ::NtClose(iocps[j]);
      if (threads[j]) {
        ::NtTerminateThread(threads[j], 0);
        ::NtClose(threads[j]);
      }
    }
    uint64_t t3 = rdtsc();
    teardown_samples[iter] = t3 - t2;
  }

  ::NtClose(reactor_iocp);
  write_str("  (");
  write_i64(SCALE_N);
  write_str(" threads)\n");
  print_stats("Scaled create (thread+IOCP+rsv+WCP)", create_samples,
              SCALE_SAMPLES);
  print_stats("Scaled teardown (cancel+close*3+term)", teardown_samples,
              SCALE_SAMPLES);
}

// ========================================================================
// Bench N: Scaled — N threads baseline (no IOCP, no WCP)
// ========================================================================

// ========================================================================
// Scaled — N threads with IOCP + reserve + POOLED WCP
// ========================================================================

static void bench_scaled_pooled() {
  HANDLE reactor_iocp = nullptr;
  ::NtCreateIoCompletion(&reactor_iocp, IO_COMPLETION_ALL_ACCESS, nullptr, 0);

  // Pre-create WCP pool.
  HANDLE wcp_pool[SCALE_N];
  for (int j = 0; j < SCALE_N; ++j)
    ::NtCreateWaitCompletionPacket(&wcp_pool[j],
                                   WAIT_COMPLETION_PACKET_ALL_ACCESS, nullptr);

  int64_t create_samples[SCALE_SAMPLES];
  int64_t teardown_samples[SCALE_SAMPLES];

  for (int iter = 0; iter < SCALE_SAMPLES; ++iter) {
    HANDLE threads[SCALE_N];
    HANDLE iocps[SCALE_N];
    HANDLE reserves[SCALE_N];

    // Measure creation (IOCP + reserve + associate pooled WCP).
    uint64_t t0 = rdtsc();
    for (int j = 0; j < SCALE_N; ++j) {
      threads[j] = create_suspended_thread(park_forever, nullptr, nullptr);
      iocps[j] = nullptr;
      reserves[j] = nullptr;
      if (threads[j]) {
        create_iocp_pair(iocps[j], reserves[j]);
        BOOLEAN already = FALSE;
        ::NtAssociateWaitCompletionPacket(
            wcp_pool[j], reactor_iocp, threads[j],
            reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(j)), nullptr,
            STATUS_SUCCESS, 0, &already);
      }
    }
    uint64_t t1 = rdtsc();
    create_samples[iter] = t1 - t0;

    // Measure teardown (cancel WCP + close reserve + close IOCP + term thread).
    uint64_t t2 = rdtsc();
    for (int j = 0; j < SCALE_N; ++j) {
      if (threads[j])
        ::NtCancelWaitCompletionPacket(wcp_pool[j], TRUE);
      if (reserves[j])
        ::NtClose(reserves[j]);
      if (iocps[j])
        ::NtClose(iocps[j]);
      if (threads[j]) {
        ::NtTerminateThread(threads[j], 0);
        ::NtClose(threads[j]);
      }
    }
    uint64_t t3 = rdtsc();
    teardown_samples[iter] = t3 - t2;
  }

  for (int j = 0; j < SCALE_N; ++j)
    ::NtClose(wcp_pool[j]);
  ::NtClose(reactor_iocp);

  write_str("  (");
  write_i64(SCALE_N);
  write_str(" threads, pooled WCP)\n");
  print_stats("Scaled create POOLED (thread+IOCP+rsv+assoc)", create_samples,
              SCALE_SAMPLES);
  print_stats("Scaled teardown POOLED (cancel+close*2+term)", teardown_samples,
              SCALE_SAMPLES);
}

// ========================================================================
// Scaled — N threads with IOCP(c=0) + reserve + POOLED WCP
// ========================================================================

static void bench_scaled_pooled_c0() {
  HANDLE reactor_iocp = nullptr;
  ::NtCreateIoCompletion(&reactor_iocp, IO_COMPLETION_ALL_ACCESS, nullptr, 0);

  HANDLE wcp_pool[SCALE_N];
  for (int j = 0; j < SCALE_N; ++j)
    ::NtCreateWaitCompletionPacket(&wcp_pool[j],
                                   WAIT_COMPLETION_PACKET_ALL_ACCESS, nullptr);

  int64_t create_samples[SCALE_SAMPLES];
  int64_t teardown_samples[SCALE_SAMPLES];

  for (int iter = 0; iter < SCALE_SAMPLES; ++iter) {
    HANDLE threads[SCALE_N];
    HANDLE iocps[SCALE_N];
    HANDLE reserves[SCALE_N];

    uint64_t t0 = rdtsc();
    for (int j = 0; j < SCALE_N; ++j) {
      threads[j] = create_suspended_thread(park_forever, nullptr, nullptr);
      iocps[j] = nullptr;
      reserves[j] = nullptr;
      if (threads[j]) {
        create_iocp_pair(iocps[j], reserves[j], 0); // c=0
        BOOLEAN already = FALSE;
        ::NtAssociateWaitCompletionPacket(
            wcp_pool[j], reactor_iocp, threads[j],
            reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(j)), nullptr,
            STATUS_SUCCESS, 0, &already);
      }
    }
    uint64_t t1 = rdtsc();
    create_samples[iter] = t1 - t0;

    uint64_t t2 = rdtsc();
    for (int j = 0; j < SCALE_N; ++j) {
      if (threads[j])
        ::NtCancelWaitCompletionPacket(wcp_pool[j], TRUE);
      if (reserves[j])
        ::NtClose(reserves[j]);
      if (iocps[j])
        ::NtClose(iocps[j]);
      if (threads[j]) {
        ::NtTerminateThread(threads[j], 0);
        ::NtClose(threads[j]);
      }
    }
    uint64_t t3 = rdtsc();
    teardown_samples[iter] = t3 - t2;
  }

  for (int j = 0; j < SCALE_N; ++j)
    ::NtClose(wcp_pool[j]);
  ::NtClose(reactor_iocp);

  write_str("  (");
  write_i64(SCALE_N);
  write_str(" threads, pooled WCP, c=0)\n");
  print_stats("Scaled create POOLED c=0 (thread+IOCP+rsv+assoc)", create_samples,
              SCALE_SAMPLES);
  print_stats("Scaled teardown POOLED c=0 (cancel+close*2+term)", teardown_samples,
              SCALE_SAMPLES);
}

// ========================================================================
// Scaled — N threads baseline (no IOCP, no WCP)
// ========================================================================

static void bench_scaled_baseline() {
  int64_t create_samples[SCALE_SAMPLES];
  int64_t teardown_samples[SCALE_SAMPLES];

  for (int iter = 0; iter < SCALE_SAMPLES; ++iter) {
    HANDLE threads[SCALE_N];

    uint64_t t0 = rdtsc();
    for (int j = 0; j < SCALE_N; ++j)
      threads[j] = create_suspended_thread(park_forever, nullptr, nullptr);
    uint64_t t1 = rdtsc();
    create_samples[iter] = t1 - t0;

    uint64_t t2 = rdtsc();
    for (int j = 0; j < SCALE_N; ++j) {
      if (threads[j]) {
        ::NtTerminateThread(threads[j], 0);
        ::NtClose(threads[j]);
      }
    }
    uint64_t t3 = rdtsc();
    teardown_samples[iter] = t3 - t2;
  }

  write_str("  (");
  write_i64(SCALE_N);
  write_str(" threads)\n");
  print_stats("Scaled create (thread only, baseline)", create_samples,
              SCALE_SAMPLES);
  print_stats("Scaled teardown (term+close, baseline)", teardown_samples,
              SCALE_SAMPLES);
}

// ========================================================================

void run_all_benchmarks() {
  g_stdout = ::NtCurrentPeb()->ProcessParameters->StandardOutput;
  init_timer();
  boost_priority();
  pin_thread(NtCurrentThread(), 2);

  write_str("=== Per-Thread IOCP Overhead Benchmark ===\n");
  write_str("Timing: rdtsc (cycle-precise). TSC freq: ");
  write_i64(tsc_freq_khz);
  write_str(" kHz (");
  write_i64(tsc_freq_khz / 1000);
  write_str(" MHz)\n");
  write_str("All values in nanoseconds (cycles in parentheses).\n\n");

  write_str("--- IOCP lifecycle costs ---\n");
  bench_iocp_create();
  bench_iocp_close();
  bench_iocp_create_close();

  write_str("\n--- Reserve + full per-thread setup/teardown ---\n");
  bench_reserve_alloc();
  bench_full_per_thread_setup();
  bench_full_per_thread_teardown();

  write_str("\n--- Pooled WCP (reuse handle, no create/close) ---\n");
  bench_wcp_associate_only();
  bench_wcp_cancel_only();
  bench_full_per_thread_setup_pooled();
  bench_full_per_thread_teardown_pooled();

  write_str("\n--- IOCP post/dequeue costs (concurrency=1, single consumer) ---\n");
  bench_iocp_post();
  bench_iocp_dequeue_nonblocking();
  bench_iocp_dequeue_empty();
  bench_iocp_post_dequeue_same_thread();

  write_str("\n--- IOCP post/dequeue costs (concurrency=0, unlimited) ---\n");
  bench_iocp_post_c0();
  bench_iocp_dequeue_nonblocking_c0();
  bench_iocp_dequeue_empty_c0();
  bench_iocp_post_dequeue_same_thread_c0();

  write_str("\n--- Cross-thread wake latency ---\n");
  bench_iocp_cross_thread_latency();
  bench_iocp_cross_thread_latency_c0();
  bench_alert_cross_thread_latency();
  bench_alert_post_cost();

  write_str("\n--- Scaled (");
  write_i64(SCALE_N);
  write_str(" threads) ---\n");
  bench_scaled_with_iocp();
  bench_scaled_pooled();
  bench_scaled_pooled_c0();
  bench_scaled_baseline();

  write_str("\nDone.\n");
}

} // namespace LIBC_NAMESPACE_DECL

int main() {
  LIBC_NAMESPACE::run_all_benchmarks();
  return 0;
}
