//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: std-at-least-c++23

#include <functional>
#include <utility>

struct Immovable {
  Immovable()            = default;
  Immovable(Immovable&&) = delete;
  void operator()() {}
};
struct Callable {
  Callable() = default;
  Callable(std::initializer_list<int>) {}
  void operator()() const {}
};
struct ReturnsValue {
  int operator()() const;
};
struct ReturnsReference {
  int& operator()() const;
};
struct Converts {
  operator int() const;
};
struct ReturnsConvertible {
  Converts operator()() const;
};

void test() {
  std::move_only_function<void()> a = Immovable{};
  // expected-error@* {{move_only_function target must be constructible from the argument}}
  // expected-error@* 0-1 {{call to deleted constructor}}
  std::move_only_function<void()> b(std::in_place_type<const Callable>);
  // expected-error@* {{move_only_function in-place target must be a decayed type}}
  std::move_only_function<void()> c(std::in_place_type<const Callable>, {1, 2});
  // expected-error@* {{move_only_function in-place target must be a decayed type}}
  std::move_only_function<const int&()> d = ReturnsValue{};
  // expected-error@* {{Returning from invoke_r would bind a temporary object}}
  std::move_only_function<const long&()> e = ReturnsReference{};
  // expected-error@* {{Returning from invoke_r would bind a temporary object}}
  std::move_only_function<const int&()> f = ReturnsConvertible{};
  // expected-error@* {{Returning from invoke_r would bind a temporary object}}
}
