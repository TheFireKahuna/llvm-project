//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// UNSUPPORTED: no-threads
// UNSUPPORTED: c++03

// <future>

// class promise<R>

// void promise::set_value_at_thread_exit(const R& r);
// void promise<void>::set_value_at_thread_exit();

// Test that these work on a thread that was not created by std::thread, which
// has no thread-exit data until the call allocates it.

#include <cassert>
#include <future>
#include <thread>

#include "test_macros.h"

void* set_int(void* arg) {
  static_cast<std::promise<int>*>(arg)->set_value_at_thread_exit(42);
  return nullptr;
}

void* set_void(void* arg) {
  static_cast<std::promise<void>*>(arg)->set_value_at_thread_exit();
  return nullptr;
}

int main(int, char**) {
  {
    std::promise<int> p;
    std::future<int> f = p.get_future();
    std::__libcpp_thread_t t;
    assert(std::__libcpp_thread_create(&t, &set_int, &p) == 0);
    assert(std::__libcpp_thread_join(&t) == 0);
    assert(f.get() == 42);
  }
  {
    std::promise<void> p;
    std::future<void> f = p.get_future();
    std::__libcpp_thread_t t;
    assert(std::__libcpp_thread_create(&t, &set_void, &p) == 0);
    assert(std::__libcpp_thread_join(&t) == 0);
    f.get();
  }

  return 0;
}
