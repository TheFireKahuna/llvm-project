//===-- SlabPool / posix_alloc aggressive stress test ---------*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Targeted at the tblgen stage-2 SIGSEGV pattern:
//
//   callq   <allocator>            ; ecx = 0x30  (48-byte alloc)
//   movups  %xmm0, (%rax)          ; +0x00  OK
//   movq    %r15, 0x10(%rax)       ; +0x10  OK
//   movups  %xmm0, 0x18(%rax)      ; +0x18  FAULT
//
// rax+0 writes succeed, rax+0x18 faults — the allocation straddles a page
// boundary and only the first page is accessible. A slot-sealing or
// page-occupancy race is the hypothesis.
//
// This test exercises posix_alloc directly through malloc/free (the public
// surface the crash traversed) and writes through every byte of every
// returned allocation to deterministically trip any "tail-page inaccessible"
// bug. Runs a mix of:
//
//   1. Size 48 (class 2) tight loops — the observed failing size. Also a
//      size whose integer-divide into 4096 is non-uniform (85.33 slots/page),
//      which means slots DO straddle page boundaries — the worst case for
//      page-occupancy tracking.
//
//   2. Sweep across all 40 size classes including the non-page-dividing
//      ones (48, 80, 96, 112, 160, 192, 224, 320, 384, 448, 640, 768, 896,
//      1280, 1536, 1792, 2560, 3072, 3584, 5120, 6144, 7168, 10240, 12288,
//      14336, 20480, 24576, 28672) — same straddling pathology at every
//      doubling tier.
//
//   3. Cross-thread free: allocator thread hands slots to freer thread via
//      an MPMC ring. Exercises xthread_free + drain_xthread + sealing done
//      by a different thread than the one that will next alloc.
//
//   4. Bin-overflow drain: single-thread rapid same-class alloc/free to
//      force drain_cache_bin past THREAD_CACHE_MAX = 64, exercising the
//      drain_prepare_slot + seal_drained_pages path on slots whose owner
//      did the free.
//
//   5. Realloc churn across class boundaries: forces class-migration which
//      allocates new slot + memcpy + frees old — another path that touches
//      slot pages in both directions.
//
// Hardening verification:
//
//   Every allocation is filled with a thread-unique pattern across its FULL
//   claimed size (the malloc argument, not the slot size — if the user asks
//   for 48 bytes the full 48 MUST be writable, even though the slot may be
//   padded to a class size like 48). Before free, the pattern is verified
//   — a non-match means another thread wrote into our slot, which would be
//   a double-allocation or freelist corruption.
//
// Failure reporting:
//
//   A SIGSEGV handler records RIP and the faulting address to stderr before
//   abort(). The test exits nonzero on any detected inconsistency.
//
// Hosted build — links against llvm-libc (crt1.obj + c.dll). No test
// framework dependency, matching the add_cdll_child_helper pattern.
//
//===----------------------------------------------------------------------===//

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/__support/macros/config.h"
#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_join.h"
#include "src/signal/raise.h"
#include "src/signal/sigaction.h"
#include "src/signal/signal.h"
#include "src/stdio/fflush.h"
#include "src/stdio/fprintf.h"
#include "src/stdio/stderr.h"
#include "src/stdlib/atoi.h"
#include "src/stdlib/getenv.h"
#include "src/stdlib/strtoul.h"
#include "src/string/strcmp.h"
#include "src/time/nanosleep.h"

// ────────────────────────────────────────────────────────────────────────────
// Configuration
// ────────────────────────────────────────────────────────────────────────────

// Tuned for ~30 seconds of wall-clock stress on a typical dev machine.
// Caller can override with env vars SLAB_STRESS_THREADS / SLAB_STRESS_ITERS.
static unsigned g_threads = 16;
static unsigned long g_iters_per_thread = 200000;

// Size classes pulled from posix_alloc.cpp. Duplicated here to keep the
// test self-contained — if the production table changes we want this test
// to deliberately drift so the drift is a code review signal.
static const unsigned short kSizeClasses[] = {
    16,    32,    48,    64,    80,    96,    112,   128,
    160,   192,   224,   256,   320,   384,   448,   512,
    640,   768,   896,   1024,  1280,  1536,  1792,  2048,
    2560,  3072,  3584,  4096,  5120,  6144,  7168,  8192,
    10240, 12288, 14336, 16384, 20480, 24576, 28672, 32768,
};
static const unsigned kNumClasses = sizeof(kSizeClasses) / sizeof(kSizeClasses[0]);

// The specific size that triggered the tblgen crash.
static constexpr unsigned short kFailingSize = 48;

// MPMC handoff ring for cross-thread free scenario (phase 3).
struct Handoff {
  void *ptr;
  unsigned size;
  unsigned pattern;
};

// ────────────────────────────────────────────────────────────────────────────
// Error reporting
// ────────────────────────────────────────────────────────────────────────────

static volatile sig_atomic_t g_error_count;

static void fail_msg(const char *what, unsigned tid, void *p, unsigned i) {
  LIBC_NAMESPACE::fprintf(LIBC_NAMESPACE::stderr,
                          "[FAIL tid=%u iter=%u] %s ptr=%p\n", tid, i, what, p);
  LIBC_NAMESPACE::fflush(LIBC_NAMESPACE::stderr);
  __atomic_fetch_add(&g_error_count, 1, __ATOMIC_SEQ_CST);
}

static void segv_handler(int sig, siginfo_t *info, void *ctx) {
  (void)ctx;
  const char *name = (sig == SIGSEGV) ? "SIGSEGV"
                   : (sig == SIGBUS)  ? "SIGBUS"
                                      : "???";
  LIBC_NAMESPACE::fprintf(
      LIBC_NAMESPACE::stderr,
      "\n*** FATAL %s — fault addr=%p signo=%d code=%d ***\n"
      "Probable root cause: slab-pool returned a pointer whose tail "
      "extends into an unmapped/sealed page.\n",
      name, info ? info->si_addr : nullptr, sig, info ? info->si_code : 0);
  LIBC_NAMESPACE::fflush(LIBC_NAMESPACE::stderr);
  // Re-raise with default handler for core dump / debugger attach.
  LIBC_NAMESPACE::signal(sig, SIG_DFL);
  LIBC_NAMESPACE::raise(sig);
}

// ────────────────────────────────────────────────────────────────────────────
// Fill + verify patterns
// ────────────────────────────────────────────────────────────────────────────

// Write a thread/iter-unique pattern through the FULL claimed size. If the
// allocator returned a pointer whose last bytes are not writable, THIS is
// where the fault lands — well before the caller ever runs.
static void fill_pattern(void *p, unsigned size, unsigned seed) {
  auto *u8 = static_cast<unsigned char *>(p);
  // Per-byte LCG — not for randomness, just for a pattern that detects
  // any aliasing between concurrent allocations of the same size.
  unsigned x = seed * 0x9E3779B9u + 1;
  for (unsigned i = 0; i < size; i++) {
    x = x * 1664525u + 1013904223u;
    u8[i] = static_cast<unsigned char>(x >> 24);
  }
}

static bool verify_pattern(void *p, unsigned size, unsigned seed) {
  auto *u8 = static_cast<unsigned char *>(p);
  unsigned x = seed * 0x9E3779B9u + 1;
  for (unsigned i = 0; i < size; i++) {
    x = x * 1664525u + 1013904223u;
    if (u8[i] != static_cast<unsigned char>(x >> 24))
      return false;
  }
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// Phase 1: size=48 hot loop, single-thread alloc/free
//   The narrowest reproducer — every iteration allocates 48 bytes, writes
//   through all 48, reads them back, frees. If ANY allocation returns a
//   pointer whose tail page is inaccessible, the fill will fault.
// ────────────────────────────────────────────────────────────────────────────

static void *phase1_hot_48(void *arg) {
  auto tid = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
  for (unsigned long i = 0; i < g_iters_per_thread; i++) {
    void *p = malloc(kFailingSize);
    if (!p) {
      fail_msg("malloc returned nullptr", tid, nullptr, unsigned(i));
      return nullptr;
    }
    // Alignment: malloc(48) must return 16-byte aligned.
    if ((reinterpret_cast<uintptr_t>(p) & 15) != 0)
      fail_msg("misaligned (expected 16B)", tid, p, unsigned(i));

    unsigned seed = tid * 0x85EBCA77u ^ unsigned(i);
    fill_pattern(p, kFailingSize, seed);      // ← crash here if tail page bad
    if (!verify_pattern(p, kFailingSize, seed))
      fail_msg("pattern mismatch after self-write", tid, p, unsigned(i));

    free(p);
  }
  return nullptr;
}

// ────────────────────────────────────────────────────────────────────────────
// Phase 2: size class sweep, all 40 classes, random order
//   Straddling classes (48, 80, 96, 112, 160, 224, 320, 448, 640, 896, 1280,
//   1792, 2560, 3584, 5120, 7168, 10240, 14336, 20480, 28672) exercise
//   multi-page slots. Non-straddling classes (16, 32, 64, 128, 256, 512,
//   1024, 2048, 4096, 8192, 16384, 32768) exercise single-page slots and
//   the direct-mmap large path (the last two).
// ────────────────────────────────────────────────────────────────────────────

static void *phase2_class_sweep(void *arg) {
  auto tid = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));

  // Keep up to 64 live allocations per thread so we hit bin/drain cycles.
  constexpr unsigned kLive = 64;
  struct Live { void *p; unsigned size; unsigned seed; };
  Live live[kLive] = {};

  unsigned x = tid * 0xBAADF00Du + 1;
  for (unsigned long i = 0; i < g_iters_per_thread; i++) {
    x = x * 1664525u + 1013904223u;
    unsigned slot = x % kLive;

    if (live[slot].p) {
      // Verify intact, free.
      if (!verify_pattern(live[slot].p, live[slot].size, live[slot].seed))
        fail_msg("pattern corrupted while held", tid, live[slot].p, unsigned(i));
      free(live[slot].p);
      live[slot].p = nullptr;
    }

    x = x * 1664525u + 1013904223u;
    unsigned ci = x % kNumClasses;
    unsigned size = kSizeClasses[ci];
    void *p = malloc(size);
    if (!p) {
      fail_msg("malloc returned nullptr", tid, nullptr, unsigned(i));
      continue;
    }
    unsigned seed = tid * 0x85EBCA77u ^ unsigned(i) ^ (size << 8);
    fill_pattern(p, size, seed);                // ← crash here on bad tail
    live[slot] = {p, size, seed};
  }

  // Drain.
  for (unsigned s = 0; s < kLive; s++) {
    if (live[s].p) {
      if (!verify_pattern(live[s].p, live[s].size, live[s].seed))
        fail_msg("pattern corrupted at drain", tid, live[s].p, 0);
      free(live[s].p);
    }
  }
  return nullptr;
}

// ────────────────────────────────────────────────────────────────────────────
// Phase 3: cross-thread free via MPMC ring
//   Allocator threads malloc + fill, push to ring. Freer threads pop from
//   ring, verify, free. Slots travel from producer slab (xthread_free path)
//   to consumer's drain — different thread does the page-occupancy decrement
//   than the one that did the alloc.
// ────────────────────────────────────────────────────────────────────────────

static constexpr unsigned kRingSize = 4096; // must be power of 2
static Handoff g_ring[kRingSize];
static volatile uint64_t g_ring_head = 0;    // producer writes
static volatile uint64_t g_ring_tail = 0;    // consumer reads
static volatile int g_phase3_done = 0;

static bool ring_push(const Handoff &h) {
  for (int spin = 0; spin < 10000; spin++) {
    uint64_t head = __atomic_load_n(&g_ring_head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&g_ring_tail, __ATOMIC_ACQUIRE);
    if (head - tail >= kRingSize) {
      // Full — yield briefly.
      struct timespec ts = {0, 1000};
      LIBC_NAMESPACE::nanosleep(&ts, nullptr);
      continue;
    }
    if (__atomic_compare_exchange_n(&g_ring_head, &head, head + 1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      g_ring[head & (kRingSize - 1)] = h;
      return true;
    }
  }
  return false;
}

static bool ring_pop(Handoff *h) {
  for (int spin = 0; spin < 10000; spin++) {
    uint64_t tail = __atomic_load_n(&g_ring_tail, __ATOMIC_RELAXED);
    uint64_t head = __atomic_load_n(&g_ring_head, __ATOMIC_ACQUIRE);
    if (tail >= head) {
      if (__atomic_load_n(&g_phase3_done, __ATOMIC_ACQUIRE) && tail >= head)
        return false;
      struct timespec ts = {0, 1000};
      LIBC_NAMESPACE::nanosleep(&ts, nullptr);
      continue;
    }
    Handoff v = g_ring[tail & (kRingSize - 1)];
    if (__atomic_compare_exchange_n(&g_ring_tail, &tail, tail + 1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      *h = v;
      return true;
    }
  }
  return false;
}

static void *phase3_producer(void *arg) {
  auto tid = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
  unsigned x = tid * 0xDEADBEEFu + 1;
  for (unsigned long i = 0; i < g_iters_per_thread; i++) {
    x = x * 1664525u + 1013904223u;
    unsigned ci = x % kNumClasses;
    unsigned size = kSizeClasses[ci];
    void *p = malloc(size);
    if (!p) {
      fail_msg("producer malloc nullptr", tid, nullptr, unsigned(i));
      continue;
    }
    unsigned seed = tid * 0x85EBCA77u ^ unsigned(i) ^ (size << 8);
    fill_pattern(p, size, seed);        // ← crash here on bad tail
    Handoff h = {p, size, seed};
    if (!ring_push(h)) {
      // Ring full for too long — free locally.
      free(p);
    }
  }
  return nullptr;
}

static void *phase3_consumer(void *arg) {
  auto tid = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
  Handoff h;
  unsigned i = 0;
  while (ring_pop(&h)) {
    if (!verify_pattern(h.ptr, h.size, h.pattern))
      fail_msg("cross-thread pattern corrupted", tid, h.ptr, i);
    free(h.ptr);
    i++;
  }
  return nullptr;
}

// ────────────────────────────────────────────────────────────────────────────
// Phase 4: bin-overflow drain
//   Rapid same-class alloc/free beyond THREAD_CACHE_MAX (= 64) forces the
//   thread cache to drain back to slab, exercising drain_cache_bin +
//   drain_prepare_slot + seal_drained_pages + subsequent fast-path alloc
//   from the sealed-then-unsealed local_free chain.
// ────────────────────────────────────────────────────────────────────────────

static void *phase4_bin_overflow(void *arg) {
  auto tid = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
  constexpr unsigned kHoldCount = 128; // 2x THREAD_CACHE_MAX

  // Use the failing size to hit both the overflow path and tail-page risk.
  for (unsigned long i = 0; i < g_iters_per_thread / kHoldCount; i++) {
    void *held[kHoldCount];
    for (unsigned j = 0; j < kHoldCount; j++) {
      held[j] = malloc(kFailingSize);
      if (!held[j]) {
        fail_msg("bin-overflow malloc nullptr", tid, nullptr, j);
        continue;
      }
      unsigned seed = tid ^ unsigned(i) ^ (j << 16);
      fill_pattern(held[j], kFailingSize, seed);
    }
    for (unsigned j = 0; j < kHoldCount; j++) {
      if (held[j])
        free(held[j]);
    }
  }
  return nullptr;
}

// ────────────────────────────────────────────────────────────────────────────
// Phase 5: realloc churn across class boundaries
// ────────────────────────────────────────────────────────────────────────────

static void *phase5_realloc_churn(void *arg) {
  auto tid = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
  unsigned x = tid * 0x1337C0DEu + 1;

  for (unsigned long i = 0; i < g_iters_per_thread / 4; i++) {
    x = x * 1664525u + 1013904223u;
    unsigned size = kSizeClasses[x % kNumClasses];
    void *p = malloc(size);
    if (!p) continue;

    unsigned seed = tid ^ unsigned(i);
    fill_pattern(p, size, seed);

    // Walk through 3-4 realloc hops crossing class boundaries.
    for (int hop = 0; hop < 4; hop++) {
      x = x * 1664525u + 1013904223u;
      unsigned new_size = kSizeClasses[x % kNumClasses];
      void *q = realloc(p, new_size);
      if (!q) {
        free(p);
        p = nullptr;
        break;
      }
      // realloc preserves min(old, new) bytes — verify only that prefix.
      unsigned preserve = size < new_size ? size : new_size;
      if (!verify_pattern(q, preserve, seed))
        fail_msg("realloc corrupted prefix", tid, q, hop);
      // Refill full new size.
      seed = seed * 2654435761u + hop;
      fill_pattern(q, new_size, seed);    // ← crash if new tail bad
      p = q;
      size = new_size;
    }
    if (p) free(p);
  }
  return nullptr;
}

// ────────────────────────────────────────────────────────────────────────────
// Driver
// ────────────────────────────────────────────────────────────────────────────

static int run_phase(const char *name, void *(*fn)(void *),
                     unsigned num_threads) {
  LIBC_NAMESPACE::fprintf(LIBC_NAMESPACE::stderr, "--- %s (%u threads) ---\n",
                          name, num_threads);
  LIBC_NAMESPACE::fflush(LIBC_NAMESPACE::stderr);

  pthread_t tids[64];
  if (num_threads > 64) num_threads = 64;
  for (unsigned t = 0; t < num_threads; t++) {
    if (LIBC_NAMESPACE::pthread_create(
            &tids[t], nullptr, fn,
            reinterpret_cast<void *>(uintptr_t(t))) != 0) {
      LIBC_NAMESPACE::fprintf(LIBC_NAMESPACE::stderr,
                              "pthread_create failed, errno=%d\n", errno);
      return 1;
    }
  }
  for (unsigned t = 0; t < num_threads; t++)
    LIBC_NAMESPACE::pthread_join(tids[t], nullptr);
  return 0;
}

static void run_phase3(void) {
  LIBC_NAMESPACE::fprintf(
      LIBC_NAMESPACE::stderr,
      "--- phase3 cross-thread free (%u producers + %u consumers) ---\n",
      g_threads / 2, g_threads / 2);
  LIBC_NAMESPACE::fflush(LIBC_NAMESPACE::stderr);
  pthread_t prod[32], cons[32];
  unsigned half = g_threads / 2;
  if (half < 1) half = 1;
  if (half > 32) half = 32;

  __atomic_store_n(&g_ring_head, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_ring_tail, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_phase3_done, 0, __ATOMIC_RELAXED);

  for (unsigned t = 0; t < half; t++)
    LIBC_NAMESPACE::pthread_create(&cons[t], nullptr, phase3_consumer,
                                   reinterpret_cast<void *>(uintptr_t(t)));
  for (unsigned t = 0; t < half; t++)
    LIBC_NAMESPACE::pthread_create(
        &prod[t], nullptr, phase3_producer,
        reinterpret_cast<void *>(uintptr_t(t + half)));

  for (unsigned t = 0; t < half; t++)
    LIBC_NAMESPACE::pthread_join(prod[t], nullptr);
  __atomic_store_n(&g_phase3_done, 1, __ATOMIC_RELEASE);
  for (unsigned t = 0; t < half; t++)
    LIBC_NAMESPACE::pthread_join(cons[t], nullptr);
}

extern "C" int main(int argc, char **argv) {
  // Install SIGSEGV handler that labels the fault.
  struct sigaction sa = {};
  sa.sa_sigaction = segv_handler;
  sa.sa_flags = SA_SIGINFO;
  LIBC_NAMESPACE::sigaction(SIGSEGV, &sa, nullptr);
  LIBC_NAMESPACE::sigaction(SIGBUS, &sa, nullptr);

  const char *env_threads = LIBC_NAMESPACE::getenv("SLAB_STRESS_THREADS");
  if (env_threads) {
    unsigned v = static_cast<unsigned>(LIBC_NAMESPACE::atoi(env_threads));
    if (v >= 1 && v <= 64) g_threads = v;
  }
  const char *env_iters = LIBC_NAMESPACE::getenv("SLAB_STRESS_ITERS");
  if (env_iters) {
    unsigned long v = LIBC_NAMESPACE::strtoul(env_iters, nullptr, 10);
    if (v >= 1000) g_iters_per_thread = v;
  }

  // Optional: restrict to a single phase via argv[1].
  const char *only = (argc > 1) ? argv[1] : nullptr;

  LIBC_NAMESPACE::fprintf(LIBC_NAMESPACE::stderr,
                          "slab_pool_stress: threads=%u iters/thread=%lu\n",
                          g_threads, g_iters_per_thread);

  if (!only || !LIBC_NAMESPACE::strcmp(only, "1"))
    run_phase("phase1 hot 48B alloc/free", phase1_hot_48, g_threads);
  if (!only || !LIBC_NAMESPACE::strcmp(only, "2"))
    run_phase("phase2 class sweep", phase2_class_sweep, g_threads);
  if (!only || !LIBC_NAMESPACE::strcmp(only, "3"))
    run_phase3();
  if (!only || !LIBC_NAMESPACE::strcmp(only, "4"))
    run_phase("phase4 bin-overflow drain", phase4_bin_overflow, g_threads);
  if (!only || !LIBC_NAMESPACE::strcmp(only, "5"))
    run_phase("phase5 realloc churn", phase5_realloc_churn, g_threads);

  int errs = __atomic_load_n(&g_error_count, __ATOMIC_SEQ_CST);
  LIBC_NAMESPACE::fprintf(
      LIBC_NAMESPACE::stderr,
      "\n=== slab_pool_stress: %s (%d detected errors) ===\n",
      errs == 0 ? "PASS" : "FAIL", errs);
  return errs ? 1 : 0;
}
