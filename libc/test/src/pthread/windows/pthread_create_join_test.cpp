//===-- Windows unittests for pthread_create and pthread_join -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - pthread_create starts a new thread; pthread_join reaps it
//   - The thread receives the argument and can return a value
//   - Multiple concurrent threads complete correctly
//   - pthread_detach allows a thread to run without join
//   - pthread_self returns a non-zero handle; pthread_equal is reflexive
//
//===----------------------------------------------------------------------===//

#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_detach.h"
#include "src/pthread/pthread_equal.h"
#include "src/pthread/pthread_join.h"
#include "src/pthread/pthread_self.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/threads/sleep.h"
#include "test/UnitTest/Test.h"

#include <pthread.h>

// Basic create + join: thread receives argument and returns a value.
TEST(LlvmLibcWindowsPthreadCreateTest, BasicCreateJoin) {
  struct Args { int input; int output; };
  Args args = {42, 0};

  pthread_t th;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_create(
      &th, nullptr,
      [](void *arg) -> void * {
        auto *a = static_cast<Args *>(arg);
        a->output = a->input * 2;
        return &a->output;
      },
      &args), 0);

  void *retval = nullptr;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_join(th, &retval), 0);
  EXPECT_EQ(args.output, 84);
  EXPECT_EQ(*static_cast<int *>(retval), 84);
}

// Eight concurrent threads each increment a shared counter.
TEST(LlvmLibcWindowsPthreadCreateTest, MultipleThreads) {
  constexpr int N = 8;
  LIBC_NAMESPACE::cpp::Atomic<int> counter(0);

  pthread_t threads[N];
  for (int i = 0; i < N; ++i) {
    ASSERT_EQ(LIBC_NAMESPACE::pthread_create(
        &threads[i], nullptr,
        [](void *arg) -> void * {
          auto *c = static_cast<LIBC_NAMESPACE::cpp::Atomic<int> *>(arg);
          c->fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);
          return nullptr;
        },
        &counter), 0);
  }

  for (int i = 0; i < N; ++i)
    LIBC_NAMESPACE::pthread_join(threads[i], nullptr);

  EXPECT_EQ(counter.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), N);
}

// pthread_detach: thread must be detachable without join.
TEST(LlvmLibcWindowsPthreadCreateTest, Detach) {
  LIBC_NAMESPACE::cpp::Atomic<int> done(0);

  pthread_t th;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_create(
      &th, nullptr,
      [](void *arg) -> void * {
        auto *d = static_cast<LIBC_NAMESPACE::cpp::Atomic<int> *>(arg);
        d->store(1, LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
        return nullptr;
      },
      &done), 0);

  EXPECT_EQ(LIBC_NAMESPACE::pthread_detach(th), 0);

  // Spin until the thread sets done — then we know it ran.
  while (done.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) == 0)
    LIBC_NAMESPACE::sleep_briefly();
}

// pthread_self is non-zero and pthread_equal is reflexive.
TEST(LlvmLibcWindowsPthreadCreateTest, SelfAndEqual) {
  pthread_t self = LIBC_NAMESPACE::pthread_self();
  EXPECT_NE(self, pthread_t{});
  EXPECT_NE(LIBC_NAMESPACE::pthread_equal(self, self), 0);
}
