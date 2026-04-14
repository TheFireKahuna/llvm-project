//===--- Implementation of a Unix mutex class -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_UNIX_MUTEX_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_UNIX_MUTEX_H

#include "hdr/types/pid_t.h"
#include "src/__support/CPP/optional.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/identifier.h"
#include "src/__support/threads/mutex_common.h"
#include "src/__support/threads/raw_mutex.h"

// Mutex type constants matching POSIX. Defined here so the Mutex class
// can be used from both pthread and C11 code without pulling in pthread.h.
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

class Mutex final : private RawMutex {
  unsigned char timed;
  unsigned char type; // PTHREAD_MUTEX_{NORMAL,ERRORCHECK,RECURSIVE}
  unsigned char robust;
  unsigned char pshared;

  pid_t owner;
  unsigned long long lock_count;

  LIBC_INLINE bool tracks_owner() const {
    return type != PTHREAD_MUTEX_NORMAL || robust;
  }

  LIBC_INLINE pid_t get_self_tid() const {
    return static_cast<pid_t>(internal::gettid());
  }

public:
  LIBC_INLINE constexpr Mutex(bool is_timed, bool is_recursive, bool is_robust,
                              bool is_pshared)
      : RawMutex(), timed(is_timed),
        type(is_recursive ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL),
        robust(is_robust), pshared(is_pshared), owner(0), lock_count(0) {}

  LIBC_INLINE constexpr Mutex(bool is_timed, int mutex_type, bool is_robust,
                              bool is_pshared)
      : RawMutex(), timed(is_timed),
        type(static_cast<unsigned char>(mutex_type)), robust(is_robust),
        pshared(is_pshared), owner(0), lock_count(0) {}

  LIBC_INLINE static MutexError init(Mutex *mutex, bool is_timed, bool isrecur,
                                     bool isrobust, bool is_pshared) {
    RawMutex::init(mutex);
    mutex->timed = is_timed;
    mutex->type = isrecur ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL;
    mutex->robust = isrobust;
    mutex->pshared = is_pshared;
    mutex->owner = 0;
    mutex->lock_count = 0;
    return MutexError::NONE;
  }

  LIBC_INLINE static MutexError init(Mutex *mutex, bool is_timed,
                                     int mutex_type, bool isrobust,
                                     bool is_pshared) {
    RawMutex::init(mutex);
    mutex->timed = is_timed;
    mutex->type = static_cast<unsigned char>(mutex_type);
    mutex->robust = isrobust;
    mutex->pshared = is_pshared;
    mutex->owner = 0;
    mutex->lock_count = 0;
    return MutexError::NONE;
  }

  LIBC_INLINE static MutexError destroy(Mutex *lock) {
    if (lock->owner != 0 || lock->lock_count != 0)
      return MutexError::BUSY;
    RawMutex::destroy(lock);
    return MutexError::NONE;
  }

  LIBC_INLINE MutexError lock() {
    if (LIBC_UNLIKELY(tracks_owner())) {
      pid_t self = get_self_tid();
      if (owner == self) {
        if (type == PTHREAD_MUTEX_RECURSIVE) {
          lock_count++;
          return MutexError::NONE;
        }
        // ERRORCHECK: deadlock detected.
        return MutexError::BAD_LOCK_STATE;
      }

      this->RawMutex::lock(cpp::nullopt, this->pshared);
      owner = self;
      lock_count = 1;
      return MutexError::NONE;
    }

    this->RawMutex::lock(cpp::nullopt, this->pshared);
    return MutexError::NONE;
  }

  LIBC_INLINE MutexError timed_lock(internal::AbsTimeout abs_time) {
    if (LIBC_UNLIKELY(tracks_owner())) {
      pid_t self = get_self_tid();
      if (owner == self) {
        if (type == PTHREAD_MUTEX_RECURSIVE) {
          lock_count++;
          return MutexError::NONE;
        }
        return MutexError::BAD_LOCK_STATE;
      }

      if (this->RawMutex::lock(cpp::optional<Futex::Timeout>{abs_time},
                               this->pshared)) {
        owner = self;
        lock_count = 1;
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
      if (owner != get_self_tid())
        return MutexError::UNLOCK_WITHOUT_LOCK;

      if (type == PTHREAD_MUTEX_RECURSIVE && lock_count > 1) {
        lock_count--;
        return MutexError::NONE;
      }

      owner = 0;
      lock_count = 0;

      if (this->RawMutex::unlock(this->pshared))
        return MutexError::NONE;
      return MutexError::UNLOCK_WITHOUT_LOCK;
    }

    if (this->RawMutex::unlock(this->pshared))
      return MutexError::NONE;
    return MutexError::UNLOCK_WITHOUT_LOCK;
  }

  LIBC_INLINE MutexError try_lock() {
    if (LIBC_UNLIKELY(tracks_owner())) {
      pid_t self = get_self_tid();
      if (owner == self) {
        if (type == PTHREAD_MUTEX_RECURSIVE) {
          lock_count++;
          return MutexError::NONE;
        }
        return MutexError::BUSY;
      }

      if (this->RawMutex::try_lock()) {
        owner = self;
        lock_count = 1;
        return MutexError::NONE;
      }
      return MutexError::BUSY;
    }

    if (this->RawMutex::try_lock())
      return MutexError::NONE;
    return MutexError::BUSY;
  }

  LIBC_INLINE MutexError make_consistent() {
    // Robust mutexes require platform-specific death detection. On platforms
    // without robust support, this is always an error.
    return MutexError::BAD_LOCK_STATE;
  }
};

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_UNIX_MUTEX_H
