//===-- Sense-reversing barrier ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lock-free sense-reversing barrier (Mellor-Crummey & Scott). One Futex
// (generation sense) + one atomic counter. Shared across all platforms.
//
// Memory ordering:
//   - Each thread's fetch_sub(ACQ_REL) on count forms a total modification
//     order N -> N-1 -> ... -> 1 -> 0. The last thread's ACQUIRE transitively
//     sees all prior threads' RELEASE stores (the chain collapses).
//   - The last thread's sense.store(RELEASE) publishes this accumulated
//     visibility. Waiting threads' sense.load(ACQUIRE) completes the
//     synchronization: after return, every thread sees every other
//     thread's pre-barrier stores.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_BARRIER_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_BARRIER_H

#include "hdr/pthread_macros.h"
#include "include/llvm-libc-types/pthread_barrier_t.h"
#include "include/llvm-libc-types/pthread_barrierattr_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/futex.h" // Futex, FutexWordType

namespace LIBC_NAMESPACE_DECL {

class Barrier {
  unsigned expected;
  Futex sense{0};
  cpp::Atomic<unsigned> count{0};

public:
  static int init(Barrier *b, const pthread_barrierattr_t *attr,
                  unsigned count);
  static int destroy(Barrier *b);
  int wait();
};

static_assert(sizeof(Barrier) <= sizeof(pthread_barrier_t),
              "The public pthread_barrier_t type cannot accommodate the "
              "internal barrier type.");

static_assert(alignof(Barrier) <= alignof(pthread_barrier_t),
              "The public pthread_barrier_t type has insufficient alignment "
              "for the internal barrier type.");

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_BARRIER_H
