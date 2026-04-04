//===--- Implementation of the RawMutex class -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_RAW_MUTEX_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_RAW_MUTEX_H

#include "hdr/errno_macros.h"
#include "src/__support/CPP/optional.h"
#include "src/__support/common.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/sleep.h"
#include "src/__support/time/abs_timeout.h"

#include <stdio.h>

#include "src/__support/macros/properties/runtime.h"

#if defined(__linux__)
#include "src/__support/threads/linux/futex_utils.h"
#elif defined(__APPLE__)
#include "src/__support/threads/darwin/futex_utils.h"
#elif defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
#include "src/__support/threads/windows/futex_utils.h"
#endif

#ifndef LIBC_COPT_TIMEOUT_ENSURE_MONOTONICITY
#define LIBC_COPT_TIMEOUT_ENSURE_MONOTONICITY 1
#endif

// TODO(bojle): check this for darwin impl
#if LIBC_COPT_TIMEOUT_ENSURE_MONOTONICITY
#include "src/__support/time/monotonicity.h"
#endif

#ifndef LIBC_COPT_RAW_MUTEX_DEFAULT_SPIN_COUNT
#define LIBC_COPT_RAW_MUTEX_DEFAULT_SPIN_COUNT 100
#endif

namespace LIBC_NAMESPACE_DECL {
// Lock is a simple timable lock for internal usage.
// This is separated from Mutex because this one does not need to consider
// robustness and reentrancy. Also, this one has spin optimization for shorter
// critical sections.
class RawMutex {
protected:
  Futex futex;
  LIBC_INLINE_VAR static constexpr FutexWordType UNLOCKED = 0b00;
  LIBC_INLINE_VAR static constexpr FutexWordType LOCKED = 0b01;
  LIBC_INLINE_VAR static constexpr FutexWordType IN_CONTENTION = 0b10;
#if defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
  // Tri-state handoff transition marker. Publisher writes this before
  // publishing the HANDOFF state on slot.link (fold:
  // SIGNALED_HANDOFF_{CLEAN,ORPHAN}); recipient CASes it back to
  // IN_CONTENTION to claim. New acquirers that observe this state
  // must park via wait() rather than exchange-to-IN_CONTENTION
  // (which would clobber the marker).
  LIBC_INLINE_VAR static constexpr FutexWordType HANDOFF_TRANSIT = 0b11;
#endif

private:
  LIBC_INLINE FutexWordType spin(unsigned spin_count) {
    FutexWordType result;
    for (;;) {
      result = futex.load(cpp::MemoryOrder::RELAXED);
      // spin until one of the following conditions is met:
      // - the mutex is unlocked
      // - the mutex is in contention
      // - the spin count reaches 0
      if (result != LOCKED || spin_count == 0u)
        return result;
      // Pause the pipeline to avoid extraneous memory operations due to
      // speculation.
      sleep_briefly();
      spin_count--;
    };
  }

  // Return true if the lock is acquired. Return false if timeout happens before
  // the lock is acquired.
  LIBC_INLINE bool lock_slow(cpp::optional<Futex::Timeout> timeout,
                             bool is_pshared, unsigned spin_count) {
    FutexWordType state = spin(spin_count);
    // Before go into contention state, try to grab the lock.
    if (state == UNLOCKED &&
        futex.compare_exchange_strong(state, LOCKED, cpp::MemoryOrder::ACQUIRE,
                                      cpp::MemoryOrder::RELAXED))
      return true;
#if LIBC_COPT_TIMEOUT_ENSURE_MONOTONICITY
    /* ADL should kick in */
    if (timeout)
      ensure_monotonicity(*timeout);
#endif
    for (;;) {
#if defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
      // CAS-based state machine — required because an unconditional
      // exchange(IN_CONTENTION) would clobber a concurrent publisher's
      // HANDOFF_TRANSIT marker (bad: breaks the post-publish rollback
      // CAS and leaves recipient waiting on a marker that no longer
      // exists → deadlock).
      //
      //   UNLOCKED         → CAS to IN_CONTENTION, own lock.
      //   LOCKED           → CAS to IN_CONTENTION, mark contention, wait.
      //   IN_CONTENTION    → wait on IN_CONTENTION.
      //   HANDOFF_TRANSIT  → wait on HANDOFF_TRANSIT; don't touch.
      if (state == UNLOCKED) {
        FutexWordType exp = UNLOCKED;
        if (futex.compare_exchange_strong(exp, IN_CONTENTION,
                                          cpp::MemoryOrder::ACQUIRE,
                                          cpp::MemoryOrder::RELAXED))
          return true;
        state = exp;
        continue;
      }
      if (state == LOCKED) {
        FutexWordType exp = LOCKED;
        if (!futex.compare_exchange_strong(exp, IN_CONTENTION,
                                           cpp::MemoryOrder::ACQUIRE,
                                           cpp::MemoryOrder::RELAXED)) {
          state = exp;
          continue;
        }
        state = IN_CONTENTION;
      }
      // state ∈ {IN_CONTENTION, HANDOFF_TRANSIT} — park on it.
      long wait_ret = futex.wait(state, timeout, is_pshared);
      if (wait_ret == 1) {
        // Handoff received — claim by transitioning the marker back
        // to IN_CONTENTION. CAS failure means the publisher already
        // rolled back (ghost detected) or another racing path
        // resolved the transition; fall through and retry.
        FutexWordType transit = HANDOFF_TRANSIT;
        if (futex.compare_exchange_strong(transit, IN_CONTENTION,
                                          cpp::MemoryOrder::ACQUIRE,
                                          cpp::MemoryOrder::RELAXED))
          return true;
      }
#else
      // Try to grab the lock if it is unlocked. Mark the contention flag if it
      // is locked.
      if (state != IN_CONTENTION &&
          futex.exchange(IN_CONTENTION, cpp::MemoryOrder::ACQUIRE) == UNLOCKED)
        return true;
      // Contention persists. Park the thread and wait for further notification.
      long wait_ret = futex.wait(IN_CONTENTION, timeout, is_pshared);
#endif
      if (ETIMEDOUT == -wait_ret)
        return false;
      // Invalidation: the mutex was destroyed/recycled while we slept.
      // Bail out — the futex address no longer belongs to our mutex.
      if (EINVAL == -wait_ret)
        return false;
#if defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
      // Any other negative return (e.g. -ENOMEM from wait-slot pool
      // exhaustion) is a fatal inability to park. Looping would either
      // spin-burn or re-fail identically — bail out as "couldn't
      // acquire." Callers treat the return identically to a timed-out
      // trylock. Only -EINTR is handled by the implicit re-loop; the
      // windows Futex::wait<false> path absorbs APC wakes internally,
      // so -EINTR never surfaces here for the default Interruptible=
      // false caller.
      if (wait_ret < 0 && wait_ret != -EINTR)
        return false;
#endif
#if defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
      // Refresh state after wake. Claim CAS may have failed because
      // publisher rolled back (value now UNLOCKED), so a fresh load
      // is required — we can't assume LOCKED. If now UNLOCKED, the
      // top-of-loop exchange(IN_CONTENTION) will acquire.
      state = futex.load(cpp::MemoryOrder::RELAXED);
#else
      // Continue to spin after waking up.
      state = spin(spin_count);
#endif
    }
  }

  LIBC_INLINE void wake(bool is_pshared) { futex.notify_one(is_pshared); }

public:
  LIBC_INLINE static void init(RawMutex *mutex) {
#if defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
    // init() atomically sets both value_ AND stack_ to clean state.
    // Do NOT use reset() here — reset() calls drain_waiters() which
    // walks the Treiber stack. For first-time init of opaque storage
    // (pthread_mutex_t, pthread_cond_t), the stack may contain garbage
    // from uninitialized memory, causing pool[garbage_index] accesses
    // on uncommitted VA → access violation.
    mutex->futex.init(UNLOCKED);
#else
    mutex->futex = UNLOCKED;
#endif
  }
  LIBC_INLINE constexpr RawMutex() : futex(UNLOCKED) {}
  [[nodiscard]] LIBC_INLINE bool try_lock() {
    FutexWordType expected = UNLOCKED;
    // Use strong version since this is a one-time operation.
    return futex.compare_exchange_strong(
        expected, LOCKED, cpp::MemoryOrder::ACQUIRE, cpp::MemoryOrder::RELAXED);
  }
  LIBC_INLINE bool
  lock(cpp::optional<Futex::Timeout> timeout = cpp::nullopt,
       bool is_shared = false,
       unsigned spin_count = LIBC_COPT_RAW_MUTEX_DEFAULT_SPIN_COUNT) {
    // Timeout will not be checked if immediate lock is possible.
    if (LIBC_LIKELY(try_lock()))
      return true;
    return lock_slow(timeout, is_shared, spin_count);
  }
  LIBC_INLINE bool unlock(bool is_pshared = false) {
#if defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
    // Handoff unlock: transfer ownership directly to one parked waiter
    // without publishing UNLOCKED. Spinners never see UNLOCKED, so no
    // thundering-herd CAS stampede.
    //
    // Fast path: CAS LOCKED→UNLOCKED (uncontended — no waiters).
    FutexWordType expected = LOCKED;
    if (LIBC_LIKELY(futex.compare_exchange_strong(
            expected, UNLOCKED, cpp::MemoryOrder::RELEASE,
            cpp::MemoryOrder::RELAXED)))
      return true;
    if (LIBC_UNLIKELY(expected == UNLOCKED))
      return false;
    // Contended: tri-state handoff. Publisher stores HANDOFF_TRANSIT
    // before publishing the HANDOFF bit; recipient CASes the marker
    // back to IN_CONTENTION to claim. Any ghost (slot cycled past
    // pre-mark OR slot migrated to another Futex via recycling) is
    // caught by the post-publish CAS rollback — race-free because
    // only the recipient and the publisher are authorized to move
    // the value off the transit marker, and CAS outcomes
    // unambiguously distinguish the two.
    futex.unlock_notify(UNLOCKED, HANDOFF_TRANSIT, is_pshared);
    return true;
#else
    FutexWordType prev = futex.exchange(UNLOCKED, cpp::MemoryOrder::RELEASE);
    // if there is someone waiting, wake them up
    if (LIBC_UNLIKELY(prev == IN_CONTENTION))
      wake(is_pshared);
    // Detect invalid unlock operation.
    return prev != UNLOCKED;
#endif
  }
  LIBC_INLINE void static destroy([[maybe_unused]] RawMutex *lock) {
    LIBC_ASSERT(lock->futex.load(cpp::MemoryOrder::RELAXED) == UNLOCKED &&
                "Mutex destroyed while used.");
#if defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
    // reset() drains stale waiters (clears their wait_address so they
    // detect invalidation on wake) then zeroes value + stack atomically.
    lock->futex.reset(UNLOCKED);
#endif
  }
  LIBC_INLINE Futex &get_raw_futex() { return futex; }
  LIBC_INLINE void reset() { futex.reset(UNLOCKED); }
  LIBC_INLINE void reset_for_fork() { futex.reset_for_fork(UNLOCKED); }
};
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_RAW_MUTEX_H
