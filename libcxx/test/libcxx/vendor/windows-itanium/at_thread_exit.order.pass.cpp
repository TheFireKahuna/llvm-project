//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: no-threads, c++03

// [futures.promise]: set_value_at_thread_exit makes the state ready after all
// of the thread's objects of thread storage duration have been destroyed.
// Windows runs libc++'s thread-exit callback before the callbacks that destroy
// those objects.

#include <cassert>
#include <chrono>
#include <future>
#include <thread>

static std::future<int> future;
static bool ready_before_destruction = false;

struct Local {
  ~Local() { ready_before_destruction = future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
};

static thread_local Local local;

int main(int, char**) {
  std::promise<int> promise;
  future = promise.get_future();
  std::thread t([&] {
    (void)&local;
    promise.set_value_at_thread_exit(1);
  });
  t.join();
  assert(future.get() == 1);
  assert(!ready_before_destruction);
  return 0;
}
