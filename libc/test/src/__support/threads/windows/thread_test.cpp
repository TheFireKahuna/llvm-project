//===-- Unittests for Windows Thread create/join/detach -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/mutex.h"
#include "src/__support/threads/sleep.h"
#include "src/__support/threads/thread.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"

static void *return_arg(void *arg) { return arg; }

// Thread::run + Thread::join (POSIX style).
TEST(LlvmLibcWindowsThreadTest, CreateJoinPosix) {
  LIBC_NAMESPACE::Thread th;
  int sentinel = 0xBEEF;
  ASSERT_EQ(th.run(return_arg, &sentinel), 0);
  void *retval;
  ASSERT_EQ(th.join(&retval), 0);
  EXPECT_EQ(retval, static_cast<void *>(&sentinel));
}

static int stdc_func(void *arg) {
  return *static_cast<int *>(arg);
}

// Thread::run + Thread::join (STDC style).
TEST(LlvmLibcWindowsThreadTest, CreateJoinStdc) {
  LIBC_NAMESPACE::Thread th;
  int retcode = 42;
  ASSERT_EQ(th.run(stdc_func, &retcode), 0);
  int val;
  ASSERT_EQ(th.join(&val), 0);
  EXPECT_EQ(val, 42);
}

// Multiple threads run concurrently and all join successfully.
TEST(LlvmLibcWindowsThreadTest, MultipleThreads) {
  constexpr int N = 8;
  LIBC_NAMESPACE::Thread threads[N];
  static LIBC_NAMESPACE::cpp::Atomic<int> counter{0};
  counter.store(0, LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);

  auto func = +[](void *) -> void * {
    counter.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
    return nullptr;
  };

  for (int i = 0; i < N; ++i)
    ASSERT_EQ(threads[i].run(func, nullptr), 0);
  for (int i = 0; i < N; ++i)
    ASSERT_EQ(threads[i].join(static_cast<void **>(nullptr)), 0);

  EXPECT_EQ(counter.load(LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED), N);
}

// Detach a thread that is still running (simple detach — resource cleanup
// deferred to thread exit).
static LIBC_NAMESPACE::Mutex *detach_mutex;

static void *detach_func(void *) {
  // Hold the mutex briefly so the main thread can detach before we exit.
  detach_mutex->lock();
  detach_mutex->unlock();
  return nullptr;
}

TEST(LlvmLibcWindowsThreadTest, DetachSimple) {
  LIBC_NAMESPACE::Mutex mtx(false, false, false, false);
  detach_mutex = &mtx;

  mtx.lock(); // Block the thread so we detach before it exits.

  LIBC_NAMESPACE::Thread th;
  ASSERT_EQ(th.run(detach_func, nullptr), 0);

  // Thread is running (blocked on mutex) — detach should be SIMPLE.
  ASSERT_EQ(th.detach(), static_cast<int>(LIBC_NAMESPACE::DetachType::SIMPLE));

  mtx.unlock(); // Let the thread finish and self-cleanup.
}

// Detach a thread that has already exited (cleanup detach).
TEST(LlvmLibcWindowsThreadTest, DetachCleanup) {
  LIBC_NAMESPACE::Mutex mtx(false, false, false, false);
  detach_mutex = &mtx;

  mtx.lock();
  LIBC_NAMESPACE::Thread th;
  ASSERT_EQ(th.run(detach_func, nullptr), 0);
  mtx.unlock();

  // Wait for the thread to finish without joining it.
  th.wait();

  // Thread is done — detach should perform full cleanup.
  ASSERT_EQ(th.detach(), static_cast<int>(LIBC_NAMESPACE::DetachType::CLEANUP));
}

// Thread-local storage: key create/set/get/delete.
TEST(LlvmLibcWindowsThreadTest, ThreadLocalStorage) {
  int dtor_called = 0;
  auto dtor = [](void *p) { (*static_cast<int *>(p))++; };

  auto key_opt = LIBC_NAMESPACE::new_tss_key(dtor);
  ASSERT_TRUE(key_opt.has_value());
  unsigned int key = *key_opt;

  // Initially null.
  EXPECT_EQ(LIBC_NAMESPACE::get_tss_value(key), static_cast<void *>(nullptr));

  // Set and get.
  ASSERT_TRUE(LIBC_NAMESPACE::set_tss_value(key, &dtor_called));
  EXPECT_EQ(LIBC_NAMESPACE::get_tss_value(key),
            static_cast<void *>(&dtor_called));

  // Delete key.
  ASSERT_TRUE(LIBC_NAMESPACE::tss_key_delete(key));
}
