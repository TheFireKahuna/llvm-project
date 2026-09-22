//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: std-at-least-c++23

#include <functional>
#include <cassert>
#include <new>
#include <utility>

#include "count_new.h"
#include "test_macros.h"

void function() {}
struct Callable {
  void operator()() {}
};
struct Large : Callable {
  char padding[128];
};

#ifndef TEST_HAS_NO_EXCEPTIONS
struct Throws : Large {
  Throws() { throw 42; }
};

void test_failure() {
  globalMemCounter.reset();
  try {
    std::move_only_function<void()> f(std::in_place_type<Throws>);
    assert(false);
  } catch (int n) {
    assert(n == 42);
    assert(globalMemCounter.checkOutstandingNewEq(0));
  }
  std::move_only_function<void()> f = function;
  globalMemCounter.throw_after      = 0;
  try {
    f = Large{};
    assert(false);
  } catch (const std::bad_alloc&) {
    assert(f);
    f();
    assert(globalMemCounter.checkOutstandingNewEq(0));
  }
  globalMemCounter.reset();
}
#endif

int main(int, char**) {
  {
    DisableAllocationGuard guard;
    std::move_only_function<void()> f = function;
    std::move_only_function<void()> g(std::in_place_type<decltype(&function)>, function);
    Callable target;
    std::move_only_function<void()> h = std::ref(target);
    std::move_only_function<void()> i(std::in_place_type<std::reference_wrapper<Callable>>, target);
    f();
    g();
    h();
    i();
  }
#ifndef TEST_HAS_NO_EXCEPTIONS
  test_failure();
#endif
  return 0;
}
