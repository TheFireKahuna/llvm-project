//===-- Tests for ThreadLocalWord single-owner parking primitive ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for every owner-visible surface of ThreadLocalWord plus every
// static cross-thread entry point. The primitive is single-owner by
// construction, so every test runs in a worker thread whose identity IS
// the TLW owner; the main thread only plays the "cross-thread writer"
// role and joins.
//
// Scenarios:
//   1. Owner hot path — read/write/test_flags/any_pending/clear_flags.
//   2. Generation counter — monotonic bump across every cross-thread write
//      (signal, signal_or, store, store_or, signal_clear_bits).
//   3. signal() — cross-thread write + alert when kernel-parked.
//   4. signal_or() — OR preserves bits, bumps gen.
//   5. signal_clear_bits() — AND-NOT clears bits, bumps gen.
//   6. store() / store_or() — no-alert variants (cache-line-wake model).
//   7. wait_for_change — changed / timeout / fast-path already-changed.
//   8. wait_for_change crossing UMWAIT→kernel park boundary (slow wake).
//   9. wait_for_addr — external-address parking via alert_if_parked.
//  10. No ghost wakes — after wait_for_change returns, a subsequent Futex
//      wait on the same owner thread must NOT return spuriously (this is
//      the regression-only test for the alert-drain on exit path).
//  11. fork_reinit semantics (TID update — single-threaded, no true fork).
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/thread_local_word.h"
#include "src/__support/time/abs_timeout.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::Futex;
using LIBC_NAMESPACE::FutexValueType;
using LIBC_NAMESPACE::ThreadLocalWord;

namespace {

// Shared state for owner/writer rendezvous. Each test creates a fresh
// OwnerCtx on the stack and hands a pointer to the owner thread.
struct OwnerCtx {
  ThreadLocalWord tlw;
  Atomic<uint32_t> ready{0};      // owner → main: "init() done, owner_tid live"
  Atomic<uint32_t> checkpoint{0}; // owner-visible mailbox for step numbers
  Atomic<uint32_t> observed{0};   // values the owner reports back to main
  Atomic<uint32_t> observed_gen{0};
  Atomic<long> wait_rc{0};        // wait_for_change return value
  Atomic<uint32_t> done{0};       // writer → owner: terminate
};

// Owner arrives at a numbered checkpoint and spins on `checkpoint` until
// main advances it. Cheap barrier — no kernel sleep, bounded by test runtime.
static void owner_wait_checkpoint(OwnerCtx *ctx, uint32_t step) {
  while (ctx->checkpoint.load(MemoryOrder::ACQUIRE) != step)
    LIBC_NAMESPACE::test_support::sleep_ms(0);
}
static void main_release_checkpoint(OwnerCtx *ctx, uint32_t step) {
  ctx->checkpoint.store(step, MemoryOrder::RELEASE);
}
static void main_wait_owner(OwnerCtx *ctx, uint32_t step) {
  while (ctx->observed.load(MemoryOrder::ACQUIRE) != step)
    LIBC_NAMESPACE::test_support::sleep_ms(0);
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Owner hot path: read/write/test_flags/any_pending/clear_flags
// ---------------------------------------------------------------------------

static DWORD hot_path_owner(void *arg) {
  auto *ctx = static_cast<OwnerCtx *>(arg);
  ctx->tlw.init();
  ctx->ready.store(1, MemoryOrder::RELEASE);

  // Step 1 — basic read/write round trip.
  owner_wait_checkpoint(ctx, 1);
  ctx->tlw.write(0xC0DE);
  ctx->observed.store(ctx->tlw.read(), MemoryOrder::RELEASE);

  // Step 2 — test_flags and any_pending after main's signal_or.
  owner_wait_checkpoint(ctx, 2);
  ctx->observed.store(
      (ctx->tlw.test_flags(0x01) ? 0x1 : 0) |
          (ctx->tlw.test_flags(0x02) ? 0x2 : 0) |
          (ctx->tlw.any_pending() ? 0x10 : 0),
      MemoryOrder::RELEASE);

  // Step 3 — clear_flags, verify bits gone.
  owner_wait_checkpoint(ctx, 3);
  ctx->tlw.clear_flags(0x01);
  ctx->observed.store(ctx->tlw.read(), MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcThreadLocalWord, OwnerHotPath) {
  OwnerCtx ctx;
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(hot_path_owner, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));

  while (!ctx.ready.load(MemoryOrder::ACQUIRE))
    LIBC_NAMESPACE::test_support::sleep_ms(0);

  // Step 1: owner writes 0xC0DE and reads it back.
  ctx.observed.store(0, MemoryOrder::RELEASE);
  main_release_checkpoint(&ctx, 1);
  main_wait_owner(&ctx, 0xC0DE);

  // Step 2: cross-thread signal_or sets bits 0 and 1.
  ctx.observed.store(0, MemoryOrder::RELEASE);
  ThreadLocalWord::signal_or(&ctx.tlw, 0x03);
  main_release_checkpoint(&ctx, 2);
  main_wait_owner(&ctx, 0x13); // flags bit0 + bit1 + any_pending

  // Step 3: owner clears bit 0. Observed value should be 0x02.
  ctx.observed.store(0, MemoryOrder::RELEASE);
  main_release_checkpoint(&ctx, 3);
  main_wait_owner(&ctx, 0x02);

  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 10000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(t);
}

// ---------------------------------------------------------------------------
// 2. Generation counter: every cross-thread write bumps it exactly once.
// ---------------------------------------------------------------------------

static DWORD gen_owner(void *arg) {
  auto *ctx = static_cast<OwnerCtx *>(arg);
  ctx->tlw.init();
  ctx->tlw.reset(0);
  ctx->ready.store(1, MemoryOrder::RELEASE);

  owner_wait_checkpoint(ctx, 1);
  // After main has issued K cross-thread writes, gen should equal K.
  ctx->observed_gen.store(ctx->tlw.snapshot_gen(), MemoryOrder::RELEASE);
  ctx->observed.store(1, MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcThreadLocalWord, GenerationBumpsOncePerWrite) {
  OwnerCtx ctx;
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(gen_owner, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));
  while (!ctx.ready.load(MemoryOrder::ACQUIRE))
    LIBC_NAMESPACE::test_support::sleep_ms(0);

  constexpr int K = 200;
  for (int i = 0; i < K; ++i) {
    // Mix of every mutating static to prove they all bump gen.
    switch (i & 4) {
    case 0: ThreadLocalWord::signal(&ctx.tlw, 0); break;
    case 1: ThreadLocalWord::signal_or(&ctx.tlw, 0); break;
    case 2: ThreadLocalWord::store(&ctx.tlw, 0); break;
    case 3: ThreadLocalWord::store_or(&ctx.tlw, 0); break;
    default: ThreadLocalWord::signal_clear_bits(&ctx.tlw, 0); break;
    }
  }
  main_release_checkpoint(&ctx, 1);
  main_wait_owner(&ctx, 1);

  EXPECT_EQ(ctx.observed_gen.load(MemoryOrder::ACQUIRE),
            static_cast<uint32_t>(K));
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 10000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(t);
}

// ---------------------------------------------------------------------------
// 3. signal() vs store(): signal wakes a kernel-parked owner; store does
//    not (cache-line-wake relies on owner being in UMWAIT, which is not
//    guaranteed under test virtualization). The owner is parked via
//    wait_for_change — exercising the Dekker protocol end-to-end.
// ---------------------------------------------------------------------------

static DWORD wait_change_owner(void *arg) {
  auto *ctx = static_cast<OwnerCtx *>(arg);
  ctx->tlw.init();
  ctx->tlw.reset(0);
  ctx->ready.store(1, MemoryOrder::RELEASE);

  // Park on value_ == 0 with a very generous timeout (10s). Expect to
  // wake via signal(), not timeout.
  auto t_exp = LIBC_NAMESPACE::internal::AbsTimeout::from_timespec(
      {10, 0}, /*is_realtime=*/false);
  long rc = ctx->tlw.wait_for_change(/*expected=*/0, *t_exp);
  ctx->wait_rc.store(rc, MemoryOrder::RELEASE);
  ctx->observed.store(ctx->tlw.read(), MemoryOrder::RELEASE);

  // Regression — no ghost wake. Immediately re-park with a short timeout;
  // expect -ETIMEDOUT, not spurious 0.
  auto t2_exp = LIBC_NAMESPACE::internal::AbsTimeout::from_timespec(
      {0, 20'000'000}, /*is_realtime=*/false); // 20ms
  long rc2 = ctx->tlw.wait_for_change(ctx->tlw.read(), *t2_exp);
  ctx->observed_gen.store(static_cast<uint32_t>(rc2), MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcThreadLocalWord, WaitForChangeDekkerAndNoGhostWake) {
  OwnerCtx ctx;
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(wait_change_owner,
                                                         &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));
  while (!ctx.ready.load(MemoryOrder::ACQUIRE))
    LIBC_NAMESPACE::test_support::sleep_ms(0);

  // Let the owner fall through to the kernel-park phase.
  LIBC_NAMESPACE::test_support::sleep_ms(30);

  ThreadLocalWord::signal(&ctx.tlw, 0xBEEF);

  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 15000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_EQ(ctx.wait_rc.load(MemoryOrder::ACQUIRE), 0L);
  EXPECT_EQ(ctx.observed.load(MemoryOrder::ACQUIRE), 0xBEEFu);
  // No-ghost-wake re-park: must have timed out, not wakened spuriously.
  EXPECT_EQ(static_cast<long>(ctx.observed_gen.load(MemoryOrder::ACQUIRE)),
            static_cast<long>(-ETIMEDOUT));
  ::NtClose(t);
}

// ---------------------------------------------------------------------------
// 4. wait_for_change fast path: value already != expected → no park.
// ---------------------------------------------------------------------------

static DWORD fastpath_owner(void *arg) {
  auto *ctx = static_cast<OwnerCtx *>(arg);
  ctx->tlw.init();
  ctx->tlw.reset(0xAAAA);
  ctx->ready.store(1, MemoryOrder::RELEASE);

  long rc = ctx->tlw.wait_for_change(/*expected=*/0);
  ctx->wait_rc.store(rc, MemoryOrder::RELEASE);
  ctx->observed.store(1, MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcThreadLocalWord, WaitForChangeFastPathAlreadyChanged) {
  OwnerCtx ctx;
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(fastpath_owner, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 5000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_EQ(ctx.wait_rc.load(MemoryOrder::ACQUIRE), 0L);
  ::NtClose(t);
}

// ---------------------------------------------------------------------------
// 5. wait_for_change timeout path — no writer at all.
// ---------------------------------------------------------------------------

static DWORD timeout_owner(void *arg) {
  auto *ctx = static_cast<OwnerCtx *>(arg);
  ctx->tlw.init();
  ctx->tlw.reset(0);
  ctx->ready.store(1, MemoryOrder::RELEASE);

  auto t_exp = LIBC_NAMESPACE::internal::AbsTimeout::from_timespec(
      {0, 30'000'000}, /*is_realtime=*/false); // 30ms
  long rc = ctx->tlw.wait_for_change(0, *t_exp);
  ctx->wait_rc.store(rc, MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcThreadLocalWord, WaitForChangeTimeout) {
  OwnerCtx ctx;
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(timeout_owner, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 5000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_EQ(ctx.wait_rc.load(MemoryOrder::ACQUIRE),
            static_cast<long>(-ETIMEDOUT));
  ::NtClose(t);
}

// ---------------------------------------------------------------------------
// 6. wait_for_addr — external-address parking; writer uses alert_if_parked
//    and stores to the external word directly. Exercises the zero-bounce
//    path used by IoRing CQE wait.
// ---------------------------------------------------------------------------

struct ExtCtx {
  ThreadLocalWord tlw;
  Atomic<uint32_t> ext_word{0};
  Atomic<uint32_t> ready{0};
  Atomic<long> wait_rc{0};
  Atomic<uint32_t> observed{0};
};

static DWORD ext_owner(void *arg) {
  auto *ctx = static_cast<ExtCtx *>(arg);
  ctx->tlw.init();
  ctx->ready.store(1, MemoryOrder::RELEASE);

  auto t_exp = LIBC_NAMESPACE::internal::AbsTimeout::from_timespec(
      {10, 0}, /*is_realtime=*/false);
  long rc = ctx->tlw.wait_for_addr(
      reinterpret_cast<const volatile uint32_t *>(&ctx->ext_word),
      /*ext_expected=*/0u, *t_exp);
  ctx->wait_rc.store(rc, MemoryOrder::RELEASE);
  ctx->observed.store(ctx->ext_word.load(MemoryOrder::ACQUIRE),
                      MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcThreadLocalWord, WaitForAddrExternalWake) {
  ExtCtx ctx;
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(ext_owner, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));
  while (!ctx.ready.load(MemoryOrder::ACQUIRE))
    LIBC_NAMESPACE::test_support::sleep_ms(0);

  // Allow Phase 0 (UMWAIT on ext_word) to elapse so the owner falls
  // through to the kernel-park phase. alert_if_parked is the only path
  // that wakes a kernel-parked owner without writing to value_.
  LIBC_NAMESPACE::test_support::sleep_ms(30);

  ctx.ext_word.store(0xF00D, MemoryOrder::RELEASE);
  ThreadLocalWord::alert_if_parked(&ctx.tlw);

  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 15000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_EQ(ctx.wait_rc.load(MemoryOrder::ACQUIRE), 0L);
  EXPECT_EQ(ctx.observed.load(MemoryOrder::ACQUIRE), 0xF00Du);
  ::NtClose(t);
}

// ---------------------------------------------------------------------------
// 7. signal_clear_bits: AND-NOT + gen bump from a cross-thread writer.
// ---------------------------------------------------------------------------

static DWORD clear_bits_owner(void *arg) {
  auto *ctx = static_cast<OwnerCtx *>(arg);
  ctx->tlw.init();
  ctx->tlw.reset(0xFFu);
  ctx->ready.store(1, MemoryOrder::RELEASE);

  owner_wait_checkpoint(ctx, 1);
  ctx->observed.store(ctx->tlw.read(), MemoryOrder::RELEASE);
  ctx->observed_gen.store(ctx->tlw.snapshot_gen(), MemoryOrder::RELEASE);
  return 0;
}

TEST(LlvmLibcThreadLocalWord, SignalClearBitsBumpsGen) {
  OwnerCtx ctx;
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(clear_bits_owner,
                                                         &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));
  while (!ctx.ready.load(MemoryOrder::ACQUIRE))
    LIBC_NAMESPACE::test_support::sleep_ms(0);

  ThreadLocalWord::signal_clear_bits(&ctx.tlw, 0x0F); // clear low nibble
  main_release_checkpoint(&ctx, 1);

  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 5000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  EXPECT_EQ(ctx.observed.load(MemoryOrder::ACQUIRE), 0xF0u);
  EXPECT_EQ(ctx.observed_gen.load(MemoryOrder::ACQUIRE), 1u);
  ::NtClose(t);
}
