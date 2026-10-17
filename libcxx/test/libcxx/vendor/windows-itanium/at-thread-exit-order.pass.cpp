//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: no-threads, c++03

// [futures.promise] and [thread.condition.nonmember]: set_value_at_thread_exit
// and notify_all_at_thread_exit take effect after all of the thread's objects
// of thread storage duration have been destroyed. Windows runs libc++'s
// thread-exit callback before the ones that destroy thread_local objects, so
// check the order on a std::thread and on a thread libc++ did not create.

#include <windows.h>

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

static std::atomic<int> destroyed{0};

struct Local {
  ~Local() { destroyed.fetch_add(1); }
};

static thread_local Local local;

static std::promise<int>* promise;
static std::mutex mutex;
static std::condition_variable cv;
static bool ready = false;

static void body() {
  (void)&local; // constructs it
  promise->set_value_at_thread_exit(42);
  std::unique_lock<std::mutex> lock(mutex);
  ready = true;
  std::notify_all_at_thread_exit(cv, std::move(lock));
}

static DWORD WINAPI foreign_body(void*) {
  body();
  return 0;
}

static void check(int expected_destroyed) {
  std::unique_lock<std::mutex> lock(mutex);
  cv.wait(lock, [] { return ready; });
  assert(destroyed.load() == expected_destroyed);
  ready = false;
}

int main(int, char**) {
  {
    std::promise<int> p;
    promise            = &p;
    std::future<int> f = p.get_future();
    HANDLE h           = CreateThread(nullptr, 0, foreign_body, nullptr, 0, nullptr);
    assert(h != nullptr);
    assert(f.get() == 42);
    assert(destroyed.load() == 1);
    check(1);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
  }
  {
    std::promise<int> p;
    promise            = &p;
    std::future<int> f = p.get_future();
    std::thread t(body);
    assert(f.get() == 42);
    assert(destroyed.load() == 2);
    check(2);
    t.join();
  }
  return 0;
}
