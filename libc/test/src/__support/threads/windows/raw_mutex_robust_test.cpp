//===-- Tests for robust-mutex EOWNERDEAD recovery -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The robust protocol (TID-in-word, bit 31 = OWNER_DIED) is the mutex
// layer most vulnerable to silent bugs. Standard tests pass even when
// death detection is broken, because an abandoned lock just looks like
// "held forever". These tests force the transition by having a worker
// thread exit (or being hard-terminated) while holding the lock, then
// observe the next locker from the main thread.
//
// Scenarios:
//   1. Natural exit w/ lock held → next locker observes OWNER_DEAD.
//      make_consistent + unlock returns the mutex to NONE for future use.
//
//   2. Recovery skipped (unlock without make_consistent) → mutex goes
//      permanently NOT_RECOVERABLE; subsequent lock attempts return
//      NOT_RECOVERABLE error deterministically.
//
//   3. NtTerminateThread mid-hold → identical OWNER_DEAD signal.
//      Verifies the death path doesn't depend on any cooperative libc
//      thread-exit cleanup.
//
//   4. Two dead owners in a row — recovery path is re-entrant.
//      A inherits from B, B previously inherited from the original dead.
//
//   5. try_lock against a dead-owner mutex returns OWNER_DEAD (not BUSY).
//      Guards the fast-path CAS that's easy to regress into "returns BUSY
//      whenever the word isn't exactly zero" — which would hide the death.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/threads/mutex.h"
#include "src/__support/threads/mutex_common.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::Mutex;
using LIBC_NAMESPACE::MutexError;

namespace {

struct WorkerCtx {
  Mutex *mu;
  Atomic<bool> locked{false};
  Atomic<bool> please_exit{false};
};

// Worker: acquire the robust mutex, signal "locked", then exit without
// unlocking. The thread's natural exit must trigger the registry-based
// death detection observed by the next locker.
static DWORD abandon_worker(void *arg) {
  auto *ctx = static_cast<WorkerCtx *>(arg);
  MutexError rc = ctx->mu->lock();
  // Fresh mutex — first lock on a virgin mutex must always succeed as NONE.
  if (rc != MutexError::NONE)
    return 1;
  ctx->locked.store(true, MemoryOrder::RELEASE);
  // Spin-wait for parent's go-ahead, then exit WITHOUT unlocking.
  while (!ctx->please_exit.load(MemoryOrder::ACQUIRE))
    LIBC_NAMESPACE::test_support::sleep_ms(0);
  return 0;
}

// Variant that spins indefinitely so the parent can NtTerminateThread it.
static DWORD spin_worker(void *arg) {
  auto *ctx = static_cast<WorkerCtx *>(arg);
  MutexError rc = ctx->mu->lock();
  if (rc != MutexError::NONE)
    return 1;
  ctx->locked.store(true, MemoryOrder::RELEASE);
  // Loop until forcibly terminated — never unlocks.
  for (;;)
    LIBC_NAMESPACE::test_support::sleep_ms(5);
}

// Poll: try_lock in a bounded loop; return the first non-BUSY result.
// Needed because is_owner_dead may take a few ms to propagate after the
// holder's registry slot is cleared (Windows thread-exit is asynchronous).
static MutexError poll_lock(Mutex &mu, int max_ms = 2000) {
  for (int i = 0; i < max_ms; ++i) {
    MutexError rc = mu.try_lock();
    if (rc != MutexError::BUSY)
      return rc;
    LIBC_NAMESPACE::test_support::sleep_ms(1);
  }
  return MutexError::BUSY;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Natural exit: worker holds lock, returns; main acquires → OWNER_DEAD.
//    make_consistent + unlock restores normal behavior.
// ---------------------------------------------------------------------------

TEST(LlvmLibcRobustMutex, NaturalExitSurfacesOwnerDead) {
  Mutex mu(/*timed=*/false, /*recursive=*/false,
           /*robust=*/true, /*pshared=*/false);

  WorkerCtx ctx{&mu};
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(abandon_worker, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));

  while (!ctx.locked.load(MemoryOrder::ACQUIRE))
    LIBC_NAMESPACE::test_support::sleep_ms(0);

  // Release the worker to exit while holding the lock.
  ctx.please_exit.store(true, MemoryOrder::RELEASE);
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 5000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(t);

  MutexError rc = poll_lock(mu);
  EXPECT_EQ(rc, MutexError::OWNER_DEAD);

  // Recover: mark state consistent, then unlock. Next lock/unlock round
  // must succeed as NONE with no residual OWNER_DIED bit.
  EXPECT_EQ(mu.make_consistent(), MutexError::NONE);
  EXPECT_EQ(mu.unlock(), MutexError::NONE);

  EXPECT_EQ(mu.lock(), MutexError::NONE);
  EXPECT_EQ(mu.unlock(), MutexError::NONE);
}

// ---------------------------------------------------------------------------
// 2. Skipping make_consistent poisons the mutex. Subsequent lockers see
//    NOT_RECOVERABLE deterministically.
// ---------------------------------------------------------------------------

TEST(LlvmLibcRobustMutex, UnlockWithoutMakeConsistentPoisons) {
  Mutex mu(/*timed=*/false, /*recursive=*/false,
           /*robust=*/true, /*pshared=*/false);

  WorkerCtx ctx{&mu};
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(abandon_worker, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));
  while (!ctx.locked.load(MemoryOrder::ACQUIRE))
    LIBC_NAMESPACE::test_support::sleep_ms(0);
  ctx.please_exit.store(true, MemoryOrder::RELEASE);
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 5000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(t);

  EXPECT_EQ(poll_lock(mu), MutexError::OWNER_DEAD);

  // Deliberately skip make_consistent — unlock while not_recoverable is set.
  EXPECT_EQ(mu.unlock(), MutexError::NONE);

  // Every future lock attempt must return NOT_RECOVERABLE (permanent poison).
  EXPECT_EQ(mu.lock(), MutexError::NOT_RECOVERABLE);
  EXPECT_EQ(mu.try_lock(), MutexError::NOT_RECOVERABLE);
}

// ---------------------------------------------------------------------------
// 3. NtTerminateThread mid-hold also surfaces OWNER_DEAD.
//    Proves the death detection isn't relying on cooperative cleanup.
// ---------------------------------------------------------------------------

TEST(LlvmLibcRobustMutex, HardTerminateSurfacesOwnerDead) {
  Mutex mu(/*timed=*/false, /*recursive=*/false,
           /*robust=*/true, /*pshared=*/false);

  WorkerCtx ctx{&mu};
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(spin_worker, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));
  while (!ctx.locked.load(MemoryOrder::ACQUIRE))
    LIBC_NAMESPACE::test_support::sleep_ms(0);

  // Hard-terminate the spinning worker.
  NTSTATUS st = ::NtTerminateThread(t, 0);
  EXPECT_TRUE(NT_SUCCESS(st));
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 5000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(t);

  EXPECT_EQ(poll_lock(mu), MutexError::OWNER_DEAD);
  EXPECT_EQ(mu.make_consistent(), MutexError::NONE);
  EXPECT_EQ(mu.unlock(), MutexError::NONE);
}

// ---------------------------------------------------------------------------
// 4. Chained death: A dies holding the lock. B acquires (OWNER_DEAD),
//    recovers, B dies. C acquires → OWNER_DEAD again, recovers cleanly.
//    Verifies the recovery path does not permanently corrupt the futex
//    word even across successive deaths.
// ---------------------------------------------------------------------------

TEST(LlvmLibcRobustMutex, ChainedDeathRecovery) {
  Mutex mu(/*timed=*/false, /*recursive=*/false,
           /*robust=*/true, /*pshared=*/false);

  // Round 1: worker A holds and exits.
  {
    WorkerCtx ctx{&mu};
    HANDLE t =
        LIBC_NAMESPACE::test_support::create_thread(abandon_worker, &ctx);
    ASSERT_NE(t, static_cast<HANDLE>(nullptr));
    while (!ctx.locked.load(MemoryOrder::ACQUIRE))
      LIBC_NAMESPACE::test_support::sleep_ms(0);
    ctx.please_exit.store(true, MemoryOrder::RELEASE);
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 5000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(t);
  }
  EXPECT_EQ(poll_lock(mu), MutexError::OWNER_DEAD);
  EXPECT_EQ(mu.make_consistent(), MutexError::NONE);
  // Keep holding (main is now B for round 2).

  // Round 2: main hands off to worker C that promptly exits while holding.
  // For that, main must unlock first, worker acquires, worker exits.
  EXPECT_EQ(mu.unlock(), MutexError::NONE);
  {
    WorkerCtx ctx{&mu};
    HANDLE t =
        LIBC_NAMESPACE::test_support::create_thread(abandon_worker, &ctx);
    ASSERT_NE(t, static_cast<HANDLE>(nullptr));
    while (!ctx.locked.load(MemoryOrder::ACQUIRE))
      LIBC_NAMESPACE::test_support::sleep_ms(0);
    ctx.please_exit.store(true, MemoryOrder::RELEASE);
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 5000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(t);
  }
  EXPECT_EQ(poll_lock(mu), MutexError::OWNER_DEAD);
  EXPECT_EQ(mu.make_consistent(), MutexError::NONE);
  EXPECT_EQ(mu.unlock(), MutexError::NONE);

  // Final sanity — mutex is fully usable.
  EXPECT_EQ(mu.lock(), MutexError::NONE);
  EXPECT_EQ(mu.unlock(), MutexError::NONE);
}

// ---------------------------------------------------------------------------
// 5. try_lock after death returns OWNER_DEAD, not BUSY.
// ---------------------------------------------------------------------------

TEST(LlvmLibcRobustMutex, TryLockAfterDeathReturnsOwnerDead) {
  Mutex mu(/*timed=*/false, /*recursive=*/false,
           /*robust=*/true, /*pshared=*/false);

  WorkerCtx ctx{&mu};
  HANDLE t = LIBC_NAMESPACE::test_support::create_thread(abandon_worker, &ctx);
  ASSERT_NE(t, static_cast<HANDLE>(nullptr));
  while (!ctx.locked.load(MemoryOrder::ACQUIRE))
    LIBC_NAMESPACE::test_support::sleep_ms(0);
  ctx.please_exit.store(true, MemoryOrder::RELEASE);
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(t, 5000),
            static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
  ::NtClose(t);

  // poll_lock uses try_lock internally — the first non-BUSY result is what
  // a direct try_lock would return once death propagates.
  EXPECT_EQ(poll_lock(mu), MutexError::OWNER_DEAD);
  EXPECT_EQ(mu.make_consistent(), MutexError::NONE);
  EXPECT_EQ(mu.unlock(), MutexError::NONE);
}
