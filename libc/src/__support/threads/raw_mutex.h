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
      // Try to grab the lock if it is unlocked. Mark the contention flag if it
      // is locked.
      if (state != IN_CONTENTION &&
          futex.exchange(IN_CONTENTION, cpp::MemoryOrder::ACQUIRE) == UNLOCKED)
        return true;
      // Contention persists. Park the thread and wait for further notification.
      long wait_ret = futex.wait(IN_CONTENTION, timeout, is_pshared);
#if defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
      // Handoff: the unlocker transferred ownership directly to us
      // without changing the futex value. We own the lock — the value
      // stays IN_CONTENTION, which is correct (other waiters may exist).
      if (wait_ret == 1)
        return true;
#endif
      if (ETIMEDOUT == -wait_ret)
        return false;
      // Invalidation: the mutex was destroyed/recycled while we slept.
      // Bail out — the futex address no longer belongs to our mutex.
      if (EINVAL == -wait_ret)
        return false;
#if defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
      // Skip post-wake spin: try exchange immediately. At high thread
      // counts, spinning after wake steals CPU from the lock holder
      // (threads > cores) and the lock is typically re-grabbed before
      // our spin completes. Setting state to LOCKED forces one exchange
      // attempt at the top of the loop without 100 wasted PAUSE iters.
      state = LOCKED;
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
    // Contended: single scan tries handoff (WAITING) first, then
    // Dekker-safe wake (IN_KERNEL), then bare store (empty list).
    futex.unlock_notify(UNLOCKED, is_pshared);
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
