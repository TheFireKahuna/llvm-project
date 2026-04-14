//===-- Unnamed semaphore for Windows ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Futex-based counting semaphore. The futex value IS the count — sem_post
// increments and notifies, sem_wait spins then blocks until it can decrement.
// Same algorithm on Linux and Windows; only the Futex backend differs.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_SEMAPHORE_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_SEMAPHORE_H

#include "hdr/errno_macros.h"
#include "include/llvm-libc-types/sem_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/optional.h"
#include "src/__support/macros/config.h"
#include "src/__support/time/abs_timeout.h"
#include "futex_utils.h"

namespace LIBC_NAMESPACE_DECL {

// Kind discriminator stored in sem_t.__data[0].
inline constexpr unsigned char SEM_KIND_UNNAMED = 0;
inline constexpr unsigned char SEM_KIND_NAMED = 1;

class Semaphore {
  Futex count;

public:
  static int init(Semaphore *sem, bool pshared, unsigned value);
  static int destroy(Semaphore *sem);

  int post();
  int wait();
  int trywait();
  int timedwait(const internal::AbsTimeout &timeout);
  int getvalue(int *sval);
};

static_assert(sizeof(unsigned char) + sizeof(Semaphore) <= sizeof(sem_t),
              "sem_t cannot accommodate Semaphore + kind byte.");
static_assert(alignof(Semaphore) <= alignof(sem_t),
              "sem_t has insufficient alignment for Semaphore.");

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_SEMAPHORE_H
