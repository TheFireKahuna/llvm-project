//===-- Windows unittests for pthread TLS (key/specific) ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - pthread_key_create allocates a key; pthread_key_delete frees it
//   - pthread_setspecific/pthread_getspecific roundtrip
//   - Each thread sees its own value for the same key
//   - Destructor is called with the stored value when a thread exits
//
//===----------------------------------------------------------------------===//

#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_getspecific.h"
#include "src/pthread/pthread_join.h"
#include "src/pthread/pthread_key_create.h"
#include "src/pthread/pthread_key_delete.h"
#include "src/pthread/pthread_setspecific.h"
#include "src/__support/CPP/atomic.h"
#include "test/UnitTest/Test.h"

#include <pthread.h>

// create/setspecific/getspecific roundtrip in the main thread.
TEST(LlvmLibcWindowsPthreadTlsTest, SetAndGet) {
  pthread_key_t key;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_key_create(&key, nullptr), 0);

  int value = 42;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_setspecific(key, &value), 0);
  EXPECT_EQ(LIBC_NAMESPACE::pthread_getspecific(key), static_cast<void *>(&value));

  LIBC_NAMESPACE::pthread_key_delete(key);
}

// Each thread sees its own value for the same key.
TEST(LlvmLibcWindowsPthreadTlsTest, PerThreadValues) {
  pthread_key_t key;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_key_create(&key, nullptr), 0);

  struct Args { pthread_key_t key; int tag; };
  Args a1 = {key, 1};
  Args a2 = {key, 2};

  auto worker = [](void *arg) -> void * {
    auto *a = static_cast<Args *>(arg);
    LIBC_NAMESPACE::pthread_setspecific(a->key, &a->tag);
    return LIBC_NAMESPACE::pthread_getspecific(a->key);
  };

  pthread_t t1, t2;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&t1, nullptr, worker, &a1), 0);
  ASSERT_EQ(LIBC_NAMESPACE::pthread_create(&t2, nullptr, worker, &a2), 0);

  void *r1 = nullptr, *r2 = nullptr;
  LIBC_NAMESPACE::pthread_join(t1, &r1);
  LIBC_NAMESPACE::pthread_join(t2, &r2);

  EXPECT_EQ(*static_cast<int *>(r1), 1);
  EXPECT_EQ(*static_cast<int *>(r2), 2);

  LIBC_NAMESPACE::pthread_key_delete(key);
}

// Destructor is called with the thread-local value on thread exit.
TEST(LlvmLibcWindowsPthreadTlsTest, Destructor) {
  LIBC_NAMESPACE::cpp::Atomic<int> destroy_count(0);

  pthread_key_t key;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_key_create(
      &key,
      [](void *val) {
        // val points to destroy_count — increment it.
        static_cast<LIBC_NAMESPACE::cpp::Atomic<int> *>(val)
            ->fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
      }), 0);

  struct Args { pthread_key_t key; LIBC_NAMESPACE::cpp::Atomic<int> *counter; };
  Args args = {key, &destroy_count};

  pthread_t th;
  ASSERT_EQ(LIBC_NAMESPACE::pthread_create(
      &th, nullptr,
      [](void *arg) -> void * {
        auto *a = static_cast<Args *>(arg);
        // Store the counter pointer as TLS — destructor will increment it.
        LIBC_NAMESPACE::pthread_setspecific(a->key, a->counter);
        return nullptr;
      },
      &args), 0);

  LIBC_NAMESPACE::pthread_join(th, nullptr);

  // Destructor must have fired when the thread exited.
  EXPECT_EQ(destroy_count.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE), 1);

  LIBC_NAMESPACE::pthread_key_delete(key);
}
