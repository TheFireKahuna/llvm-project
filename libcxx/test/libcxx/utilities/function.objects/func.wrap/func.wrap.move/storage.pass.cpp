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
#include <memory>
#include <type_traits>
#include <utility>

#include "count_new.h"
#include "test_macros.h"

struct Small {
  int* moves;
  explicit Small(int& n) : moves(&n) {}
  Small(Small&& other) noexcept : moves(other.moves) { ++*moves; }
  Small& operator=(Small&&) = delete;
  void operator()() const noexcept {}
};
struct Large : Small {
  char padding[128];
  using Small::Small;
};
struct alignas(64) Aligned : Small {
  using Small::Small;
};
struct ThrowingMove : Small {
  using Small::Small;
  ThrowingMove(ThrowingMove&& other) noexcept(false) : Small(std::move(other)) {}
};
struct Immovable {
  Immovable()            = default;
  Immovable(Immovable&&) = delete;
  void operator()() const noexcept {}
};

template <class T>
void test_external() {
  int moves = 0;
  globalMemCounter.reset();
  {
    std::move_only_function<void()> f(std::in_place_type<T>, moves);
    DoNotOptimize(f);
    assert(globalMemCounter.checkOutstandingNewEq(1));
    DisableAllocationGuard guard;
    auto g                               = std::move(f);
    std::move_only_function<void() &&> h = std::move(g);
    h.swap(h);
    assert(moves == 0);
    std::move(h)();
  }
  assert(globalMemCounter.checkOutstandingNewEq(0));
}

int main(int, char**) {
  static_assert(sizeof(std::move_only_function<void()>) == 4 * sizeof(void*));
  static_assert(!std::is_trivially_copyable_v<std::move_only_function<void()>>);
  int moves = 0;
  {
    DisableAllocationGuard guard;
    std::move_only_function<void() const noexcept> f(std::in_place_type<Small>, moves);
    std::move_only_function<void()> g = std::move(f);
    assert(moves == 1);
    assert(!f);
    auto h = std::move(g);
    assert(moves == 2);
    assert(!g);
    h();
  }
  {
    auto owner = std::make_unique<int>(42);
    DisableAllocationGuard guard;
    std::move_only_function<int()> f = [p = std::move(owner)] { return *p; };
    auto g                           = std::move(f);
    assert(g() == 42);
  }
  test_external<Large>();
#ifndef TEST_HAS_NO_ALIGNED_ALLOCATION
  test_external<Aligned>();
#endif
  test_external<ThrowingMove>();
  {
    std::move_only_function<void()> f(std::in_place_type<Immovable>);
    DisableAllocationGuard guard;
    auto g = std::move(f);
    g();
  }
  return 0;
}
