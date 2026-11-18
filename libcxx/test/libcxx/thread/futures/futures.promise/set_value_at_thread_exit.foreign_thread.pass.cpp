//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// UNSUPPORTED: no-threads
// UNSUPPORTED: c++03
// UNSUPPORTED: using-built-library-before-llvm-24

// <future>

// Test that set_value_at_thread_exit and set_exception_at_thread_exit work on a
// thread that was not created by std::thread. See https://llvm.org/PR30202 for
// notify_all_at_thread_exit.

#include <cassert>
#include <exception>
#include <future>
#include <thread>

#include "test_macros.h"

template <class F>
void run_on_foreign_thread(F f) {
  std::__libcpp_thread_t t;
  int ec = std::__libcpp_thread_create(
      &t,
      [](void* arg) -> void* {
        (*static_cast<F*>(arg))();
        return nullptr;
      },
      &f);
  assert(ec == 0);
  ec = std::__libcpp_thread_join(&t);
  assert(ec == 0);
}

int main(int, char**) {
  {
    std::promise<int> p;
    std::future<int> f = p.get_future();
    run_on_foreign_thread([&] { p.set_value_at_thread_exit(42); });
    assert(f.get() == 42);
  }
#ifndef TEST_HAS_NO_EXCEPTIONS
  {
    std::promise<int> p;
    std::future<int> f = p.get_future();
    run_on_foreign_thread([&] { p.set_exception_at_thread_exit(std::make_exception_ptr(7)); });
    try {
      (void)f.get();
      assert(false);
    } catch (int e) {
      assert(e == 7);
    }
  }
#endif

  return 0;
}
