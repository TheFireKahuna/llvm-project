//===-- Unnamed semaphore implementation -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/semaphore.h"
#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"

#include <limits.h> // INT_MAX

namespace LIBC_NAMESPACE_DECL {

int Semaphore::init(Semaphore *sem, bool pshared, unsigned value) {
  if (pshared)
    return ENOTSUP;
  if (value > static_cast<unsigned>(INT_MAX))
    return EINVAL;
  sem->count.init(static_cast<FutexValueType>(value));
  return 0;
}

int Semaphore::destroy(Semaphore *) { return 0; }

int Semaphore::post() {
  // Increment the count. If waiters exist, wake one.
  // ACQ_REL: RELEASE publishes the poster's stores; ACQUIRE is not strictly
  // needed here but keeps the ordering symmetric with wait().
  FutexValueType old = count.load(cpp::MemoryOrder::RELAXED);
  for (;;) {
    if (old == static_cast<FutexValueType>(INT_MAX))
      return EOVERFLOW;
    if (count.compare_exchange_weak(old, old + 1, cpp::MemoryOrder::RELEASE,
                                    cpp::MemoryOrder::RELAXED))
      break;
  }
  count.notify_one();
  return 0;
}

int Semaphore::wait() {
  for (;;) {
    FutexValueType val = count.load(cpp::MemoryOrder::ACQUIRE);
    if (val > 0) {
      if (count.compare_exchange_weak(val, val - 1,
                                      cpp::MemoryOrder::ACQ_REL,
                                      cpp::MemoryOrder::ACQUIRE))
        return 0;
      continue; // CAS failed, retry.
    }
    // Count is zero — block until a poster increments it.
    count.wait(0);
  }
}

int Semaphore::trywait() {
  FutexValueType val = count.load(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    if (val == 0)
      return EAGAIN;
    if (count.compare_exchange_weak(val, val - 1, cpp::MemoryOrder::ACQ_REL,
                                    cpp::MemoryOrder::ACQUIRE))
      return 0;
  }
}

int Semaphore::timedwait(const internal::AbsTimeout &timeout) {
  for (;;) {
    FutexValueType val = count.load(cpp::MemoryOrder::ACQUIRE);
    if (val > 0) {
      if (count.compare_exchange_weak(val, val - 1,
                                      cpp::MemoryOrder::ACQ_REL,
                                      cpp::MemoryOrder::ACQUIRE))
        return 0;
      continue;
    }
    // Block with timeout.
    int ret = count.wait(0, Futex::Timeout(timeout));
    if (ret == -ETIMEDOUT) {
      // One more try — a post may have landed between timeout and our check.
      val = count.load(cpp::MemoryOrder::ACQUIRE);
      if (val > 0 &&
          count.compare_exchange_strong(val, val - 1,
                                        cpp::MemoryOrder::ACQ_REL,
                                        cpp::MemoryOrder::ACQUIRE))
        return 0;
      return ETIMEDOUT;
    }
  }
}

int Semaphore::getvalue(int *sval) {
  *sval = static_cast<int>(count.load(cpp::MemoryOrder::RELAXED));
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
