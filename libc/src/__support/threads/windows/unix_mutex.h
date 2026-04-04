//===--- Windows POSIX-compatible mutex implementation -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Full POSIX mutex (NORMAL, ERRORCHECK, RECURSIVE, robust) for Windows.
// Included from mutex.h when targeting NTPOSIX (POSIX runtime on Windows).
//
// This file is the Windows counterpart of the shared unix_mutex.h. It provides
// the same Mutex class API but with Windows-specific robust mutex support
// (death detection via thread registry, per-thread robust list).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_UNIX_MUTEX_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_UNIX_MUTEX_H

#include "hdr/errno_macros.h"
#include "hdr/types/pid_t.h"
#include "src/__support/CPP/optional.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/identifier.h"
#include "src/__support/threads/mutex_common.h"
#include "src/__support/threads/raw_mutex.h"
#include "src/__support/threads/windows/mutex.h"
#include "src/__support/threads/windows/thread_lifecycle.h"

// Mutex type constants matching POSIX. Defined here for use in the Mutex class
// without pulling in pthread headers (C11 mtx_init also needs these).
#ifndef PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_NORMAL 0
#endif
#ifndef PTHREAD_MUTEX_ERRORCHECK
#define PTHREAD_MUTEX_ERRORCHECK 1
#endif
#ifndef PTHREAD_MUTEX_RECURSIVE
#define PTHREAD_MUTEX_RECURSIVE 2
#endif

namespace LIBC_NAMESPACE_DECL {

// Futex word encoding for robust mutexes. The owner TID is stored directly
// in the futex word, enabling death detection without per-mutex handles.
//
// Bits 0-29:  owner TID (0 = unlocked)
// Bit 30:     WAITERS (threads waiting in futex)
// Bit 31:     OWNER_DIED (previous owner terminated without unlock)
inline constexpr FutexValueType ROBUST_TID_MASK = 0x3FFFFFFFu;
inline constexpr FutexValueType ROBUST_WAITERS = 0x40000000u;
inline constexpr FutexValueType ROBUST_OWNER_DIED = 0x80000000u;

// Stop / filter predicate for robust_lock_slow, consumed by
// Futex::wait_on_predicate. Returns true iff the mutex is acquirable
// — TID bits zero (unlocked) or OWNER_DIED set (previous owner
// crashed; a fresh acquirer recovers via make_consistent).
//
// Notify contract: every transition that flips the predicate
// FALSE → TRUE must be paired with a notify on the futex. Checked:
//   (a) robust_unlock — CAS(self_tid [| ROBUST_WAITERS] → 0)
//       followed by notify_all, or store_and_notify_all for the
//       not_recoverable path (publishes ROBUST_OWNER_DIED).
//   (b) robust_lock_slow death-detect — CAS(old → OWNER_DIED |
//       WAITERS) followed by notify_all.
// Both FALSE → TRUE edges notify; the contract holds.
[[clang::always_inline]] LIBC_INLINE bool
robust_is_acquirable(uint32_t v, uint32_t /*arg*/) noexcept {
  return (v & ROBUST_TID_MASK) == 0 || (v & ROBUST_OWNER_DIED) != 0;
}

class Mutex final : private RawMutex {
  unsigned char timed;
  unsigned char type; // PTHREAD_MUTEX_{NORMAL,ERRORCHECK,RECURSIVE}
  unsigned char robust;
  unsigned char pshared;
  cpp::Atomic<unsigned char> not_recoverable;

  cpp::Atomic<pid_t> owner;
  cpp::Atomic<unsigned int> lock_count;

  // Opaque handle to a lifecycle-owned robust record.
  RobustRecord *robust_record;

  LIBC_INLINE bool tracks_owner() const {
    return type != PTHREAD_MUTEX_NORMAL || robust;
  }

  // --- Platform helpers (thin wrappers around windows/mutex.h "syscalls") ---

  LIBC_INLINE pid_t get_self_tid() const {
    return static_cast<pid_t>(internal::gettid());
  }

  LIBC_INLINE FutexValueType get_self_futex_tid() const {
    return robust_mutex::get_robust_owner_id() & ROBUST_TID_MASK;
  }

  LIBC_INLINE void set_owner_locked(pid_t self) {
    owner.store(self, cpp::MemoryOrder::RELAXED);
    lock_count.store(1, cpp::MemoryOrder::RELAXED);
    if (robust)
      robust_mutex::robust_list_add(&futex, &robust_record);
  }

  LIBC_INLINE void clear_robust_record() {
    robust_mutex::robust_list_remove(&robust_record);
  }

  // --- Robust mutex protocol (TID-in-futex-word) ---

  LIBC_INLINE MutexError robust_lock(cpp::optional<Futex::Timeout> timeout) {
    FutexValueType self_tid = get_self_futex_tid();
    pid_t self = static_cast<pid_t>(self_tid);

    // Fast path: CAS(0 -> self_tid).
    FutexValueType expected = 0;
    if (LIBC_LIKELY(futex.compare_exchange_strong(
            expected, self_tid, cpp::MemoryOrder::ACQUIRE,
            cpp::MemoryOrder::RELAXED))) {
      set_owner_locked(self);
      return MutexError::NONE;
    }

    return robust_lock_slow(timeout, self, self_tid);
  }

  LIBC_INLINE MutexError
  robust_lock_slow(cpp::optional<Futex::Timeout> timeout, pid_t self,
                   FutexValueType self_tid) {
    for (;;) {
      FutexValueType word = futex.load(cpp::MemoryOrder::RELAXED);
      FutexValueType owner_tid = word & ROBUST_TID_MASK;

      // Unlocked — try to grab.
      if (owner_tid == 0 && !(word & ROBUST_OWNER_DIED)) {
        FutexValueType desired = self_tid | (word & ROBUST_WAITERS);
        if (futex.compare_exchange_strong(word, desired,
                                          cpp::MemoryOrder::ACQUIRE,
                                          cpp::MemoryOrder::RELAXED)) {
          set_owner_locked(self);
          return MutexError::NONE;
        }
        continue;
      }

      // Previous owner died — recoverable if caller calls make_consistent.
      if (word & ROBUST_OWNER_DIED) {
        FutexValueType desired = self_tid | (word & ROBUST_WAITERS);
        if (futex.compare_exchange_strong(word, desired,
                                          cpp::MemoryOrder::ACQUIRE,
                                          cpp::MemoryOrder::RELAXED)) {
          set_owner_locked(self);
          not_recoverable.store(1, cpp::MemoryOrder::RELAXED);
          return MutexError::OWNER_DEAD;
        }
        continue;
      }

      // Owner present — check if dead via platform death detection.
      if (robust_mutex::is_owner_dead(owner_tid)) {
        FutexValueType old = word;
        FutexValueType died = ROBUST_OWNER_DIED | ROBUST_WAITERS;
        futex.compare_exchange_strong(old, died, cpp::MemoryOrder::RELEASE,
                                      cpp::MemoryOrder::RELAXED);
        futex.notify_all(pshared);
        continue;
      }

      // Set WAITERS bit if not already set.
      if (!(word & ROBUST_WAITERS)) {
        FutexValueType with_waiters = word | ROBUST_WAITERS;
        futex.compare_exchange_strong(word, with_waiters,
                                      cpp::MemoryOrder::RELAXED,
                                      cpp::MemoryOrder::RELAXED);
      }

      // Park.
      //
      // TIMEOUT and any other negative-non-EINTR result (e.g. -ENOMEM
      // from wait-slot pool exhaustion, -EINVAL from mutex destruction)
      // are all "cannot make progress" conditions; callers treat them
      // the same way — abort the acquire attempt. Mapping everything
      // except a signalled wake to TIMEOUT keeps the public API stable
      // (MutexError is a fixed enum) without silently spin-failing on
      // an unacquirable robust mutex.
      //
      // Predicate-driven wait: ret == 0 ⇒ robust_is_acquirable held at
      // some point during the wait (unlocked OR OWNER_DIED). The
      // predicate doubles as the waker-side filter, so wakes on TID
      // rollover (e.g. any other owner's unlock→re-acquire cycle that
      // leaves value_ locked by a fresh TID) don't alert us. Caller's
      // outer for(;;) is a barger-retry loop, not a wake-absorb loop —
      // the CAS may still lose to a fresh acquirer that beat us to
      // ownership; that's a retry, not a re-wait.
      long wait_ret = futex.wait_on_predicate(&robust_is_acquirable, 0,
                                               timeout, pshared);
      if (wait_ret == -ETIMEDOUT)
        return MutexError::TIMEOUT;
      if (wait_ret < 0 && wait_ret != -EINTR)
        return MutexError::TIMEOUT;
    }
  }

  LIBC_INLINE MutexError robust_try_lock() {
    FutexValueType self_tid = get_self_futex_tid();
    pid_t self = static_cast<pid_t>(self_tid);

    FutexValueType expected = 0;
    if (futex.compare_exchange_strong(expected, self_tid,
                                      cpp::MemoryOrder::ACQUIRE,
                                      cpp::MemoryOrder::RELAXED)) {
      set_owner_locked(self);
      return MutexError::NONE;
    }

    // Also try to claim if OWNER_DIED.
    if (expected & ROBUST_OWNER_DIED) {
      FutexValueType desired = self_tid | (expected & ROBUST_WAITERS);
      if (futex.compare_exchange_strong(expected, desired,
                                        cpp::MemoryOrder::ACQUIRE,
                                        cpp::MemoryOrder::RELAXED)) {
        set_owner_locked(self);
        not_recoverable.store(1, cpp::MemoryOrder::RELAXED);
        return MutexError::OWNER_DEAD;
      }
    }

    return MutexError::BUSY;
  }

  LIBC_INLINE MutexError robust_unlock() {
    FutexValueType self_tid = get_self_futex_tid();

    // If not_recoverable, poison the mutex permanently.
    if (LIBC_UNLIKELY(not_recoverable.load(cpp::MemoryOrder::RELAXED) != 0)) {
      clear_robust_record();
      // store_and_notify_all, NOT store(RELEASE)+notify_all. On x86 the
      // split form reorders, letting a concurrent waiter push under the
      // old value and park while the waker's notify reads stack_=empty.
      // store_and_notify_all uses SEQ_CST which drains the store buffer.
      futex.store_and_notify_all(ROBUST_OWNER_DIED, pshared);
      return MutexError::NONE;
    }

    // Fast path: CAS(self_tid -> 0) when no waiters.
    FutexValueType expected = self_tid;
    if (LIBC_LIKELY(futex.compare_exchange_strong(
            expected, 0, cpp::MemoryOrder::RELEASE,
            cpp::MemoryOrder::RELAXED))) {
      clear_robust_record();
      return MutexError::NONE;
    }

    // Had WAITERS bit — clear and wake.
    expected = self_tid | ROBUST_WAITERS;
    if (futex.compare_exchange_strong(expected, 0, cpp::MemoryOrder::RELEASE,
                                      cpp::MemoryOrder::RELAXED)) {
      clear_robust_record();
      futex.notify_all(pshared);
      return MutexError::NONE;
    }

    return MutexError::UNLOCK_WITHOUT_LOCK;
  }

public:
  // Constructor matches the upstream Mutex(bool, bool, bool, bool) signature
  // so that all existing callers (TSSKeyMgr, etc.) work unchanged.
  LIBC_INLINE constexpr Mutex(bool is_timed, bool is_recursive, bool is_robust,
                              bool is_pshared)
      : RawMutex(), timed(is_timed),
        type(is_recursive ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL),
        robust(is_robust), pshared(is_pshared), not_recoverable(0), owner(0),
        lock_count(0), robust_record(nullptr) {}

  // Extended constructor accepting POSIX mutex type constants directly.
  LIBC_INLINE constexpr Mutex(bool is_timed, int mutex_type, bool is_robust,
                              bool is_pshared)
      : RawMutex(), timed(is_timed),
        type(static_cast<unsigned char>(mutex_type)), robust(is_robust),
        pshared(is_pshared), not_recoverable(0), owner(0), lock_count(0),
        robust_record(nullptr) {}

  LIBC_INLINE static MutexError init(Mutex *mutex, bool is_timed, bool isrecur,
                                     bool isrobust, bool is_pshared) {
    RawMutex::init(mutex);
    mutex->timed = is_timed;
    mutex->type = isrecur ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL;
    mutex->robust = isrobust;
    mutex->pshared = is_pshared;
    mutex->not_recoverable.store(0, cpp::MemoryOrder::RELAXED);
    mutex->owner.store(0, cpp::MemoryOrder::RELAXED);
    mutex->lock_count.store(0, cpp::MemoryOrder::RELAXED);
    mutex->robust_record = nullptr;
    return MutexError::NONE;
  }

  // Extended init accepting POSIX mutex type constants directly.
  LIBC_INLINE static MutexError init(Mutex *mutex, bool is_timed,
                                     int mutex_type, bool isrobust,
                                     bool is_pshared) {
    RawMutex::init(mutex);
    mutex->timed = is_timed;
    mutex->type = static_cast<unsigned char>(mutex_type);
    mutex->robust = isrobust;
    mutex->pshared = is_pshared;
    mutex->not_recoverable.store(0, cpp::MemoryOrder::RELAXED);
    mutex->owner.store(0, cpp::MemoryOrder::RELAXED);
    mutex->lock_count.store(0, cpp::MemoryOrder::RELAXED);
    mutex->robust_record = nullptr;
    return MutexError::NONE;
  }

  LIBC_INLINE static MutexError destroy(Mutex *lock) {
    if (lock->owner.load(cpp::MemoryOrder::RELAXED) != 0 ||
        lock->lock_count.load(cpp::MemoryOrder::RELAXED) != 0)
      return MutexError::BUSY;
    RawMutex::destroy(lock);
    return MutexError::NONE;
  }

  LIBC_INLINE MutexError lock() {
    if (LIBC_UNLIKELY(not_recoverable.load(cpp::MemoryOrder::ACQUIRE) != 0))
      return MutexError::NOT_RECOVERABLE;

    if (LIBC_UNLIKELY(tracks_owner())) {
      pid_t self = get_self_tid();
      if (owner.load(cpp::MemoryOrder::RELAXED) == self) {
        if (type == PTHREAD_MUTEX_RECURSIVE) {
          lock_count.fetch_add(1, cpp::MemoryOrder::RELAXED);
          return MutexError::NONE;
        }
        return MutexError::BAD_LOCK_STATE;
      }

      if (LIBC_UNLIKELY(robust != 0))
        return robust_lock(cpp::nullopt);

      this->RawMutex::lock(cpp::nullopt, this->pshared);
      owner.store(self, cpp::MemoryOrder::RELAXED);
      lock_count.store(1, cpp::MemoryOrder::RELAXED);
      return MutexError::NONE;
    }

    this->RawMutex::lock(cpp::nullopt, this->pshared);
    return MutexError::NONE;
  }

  LIBC_INLINE MutexError timed_lock(internal::AbsTimeout abs_time) {
    if (LIBC_UNLIKELY(not_recoverable.load(cpp::MemoryOrder::ACQUIRE) != 0))
      return MutexError::NOT_RECOVERABLE;

    if (LIBC_UNLIKELY(tracks_owner())) {
      pid_t self = get_self_tid();
      if (owner.load(cpp::MemoryOrder::RELAXED) == self) {
        if (type == PTHREAD_MUTEX_RECURSIVE) {
          lock_count.fetch_add(1, cpp::MemoryOrder::RELAXED);
          return MutexError::NONE;
        }
        return MutexError::BAD_LOCK_STATE;
      }

      if (LIBC_UNLIKELY(robust != 0))
        return robust_lock(cpp::optional<Futex::Timeout>{abs_time});

      if (this->RawMutex::lock(cpp::optional<Futex::Timeout>{abs_time},
                               this->pshared)) {
        owner.store(self, cpp::MemoryOrder::RELAXED);
        lock_count.store(1, cpp::MemoryOrder::RELAXED);
        return MutexError::NONE;
      }
      return MutexError::TIMEOUT;
    }

    if (this->RawMutex::lock(cpp::optional<Futex::Timeout>{abs_time},
                             this->pshared))
      return MutexError::NONE;
    return MutexError::TIMEOUT;
  }

  LIBC_INLINE MutexError unlock() {
    if (LIBC_UNLIKELY(tracks_owner())) {
      if (owner.load(cpp::MemoryOrder::RELAXED) != get_self_tid())
        return MutexError::UNLOCK_WITHOUT_LOCK;

      if (type == PTHREAD_MUTEX_RECURSIVE &&
          lock_count.load(cpp::MemoryOrder::RELAXED) > 1) {
        lock_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
        return MutexError::NONE;
      }

      owner.store(0, cpp::MemoryOrder::RELAXED);
      lock_count.store(0, cpp::MemoryOrder::RELAXED);

      if (LIBC_UNLIKELY(robust != 0))
        return robust_unlock();

      if (this->RawMutex::unlock(this->pshared))
        return MutexError::NONE;
      return MutexError::UNLOCK_WITHOUT_LOCK;
    }

    if (this->RawMutex::unlock(this->pshared))
      return MutexError::NONE;
    return MutexError::UNLOCK_WITHOUT_LOCK;
  }

  LIBC_INLINE MutexError try_lock() {
    if (LIBC_UNLIKELY(not_recoverable.load(cpp::MemoryOrder::ACQUIRE) != 0))
      return MutexError::NOT_RECOVERABLE;

    if (LIBC_UNLIKELY(tracks_owner())) {
      pid_t self = get_self_tid();
      if (owner.load(cpp::MemoryOrder::RELAXED) == self) {
        if (type == PTHREAD_MUTEX_RECURSIVE) {
          lock_count.fetch_add(1, cpp::MemoryOrder::RELAXED);
          return MutexError::NONE;
        }
        return MutexError::BUSY;
      }

      if (LIBC_UNLIKELY(robust != 0))
        return robust_try_lock();

      if (this->RawMutex::try_lock()) {
        owner.store(self, cpp::MemoryOrder::RELAXED);
        lock_count.store(1, cpp::MemoryOrder::RELAXED);
        return MutexError::NONE;
      }
      return MutexError::BUSY;
    }

    if (this->RawMutex::try_lock())
      return MutexError::NONE;
    return MutexError::BUSY;
  }

  LIBC_INLINE MutexError make_consistent() {
    if (!robust)
      return MutexError::BAD_LOCK_STATE;
    if (owner.load(cpp::MemoryOrder::RELAXED) != get_self_tid())
      return MutexError::UNLOCK_WITHOUT_LOCK;
    not_recoverable.store(0, cpp::MemoryOrder::RELAXED);
    return MutexError::NONE;
  }

  LIBC_INLINE void mark_not_recoverable() {
    not_recoverable.store(1, cpp::MemoryOrder::RELAXED);
  }
};

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_UNIX_MUTEX_H
