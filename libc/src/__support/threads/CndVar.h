//===-- A platform independent abstraction layer for cond vars --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_CNDVAR_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_CNDVAR_H

#include "src/__support/CPP/optional.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/cndvar_queue.h" // CndWaiterQueue, CndWaiter
#include "src/__support/threads/mutex.h"        // Mutex
#include "src/__support/threads/raw_mutex.h"    // RawMutex, Futex

namespace LIBC_NAMESPACE_DECL {

class CndVar {
  CndWaiterQueue waitq;
  RawMutex qmtx;

public:
  LIBC_INLINE static int init(CndVar *cv) {
    cv->waitq = CndWaiterQueue();
    RawMutex::init(&cv->qmtx);
    return 0;
  }

  LIBC_INLINE static void destroy(CndVar *cv) {
    cv->waitq = CndWaiterQueue();
  }

  // Returns 0 on success, -1 on error.
  int wait(Mutex *m);

  // Wait with optional timeout. Returns 0 on signal, ETIMEDOUT on timeout,
  // -1 on error. When timeout is nullopt, waits indefinitely.
  int wait(Mutex *m, cpp::optional<Futex::Timeout> timeout);

  void notify_one();
  void broadcast();
};

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_CNDVAR_H
