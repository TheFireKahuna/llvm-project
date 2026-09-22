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
#include <type_traits>
#include <utility>

struct Qualified {
  int operator()() & noexcept { return 1; }
  int operator()() const& noexcept { return 2; }
  int operator()() && noexcept { return 3; }
  int operator()() const&& noexcept { return 4; }
};

template <class F>
int invoke(F& f) {
  if constexpr (std::is_invocable_v<F&>)
    return f();
  else
    return std::move(f)();
}

template <class Source, class Destination, bool Const, int Ref, bool Noexcept>
void test_conversion() {
  using CV   = std::conditional_t<Const, const Source, Source>;
  using Inv  = std::conditional_t<Ref == 2, CV&&, CV&>;
  using Qual = std::conditional_t<Ref == 0, CV, Inv>;
  constexpr bool callable =
      Noexcept ? std::is_nothrow_invocable_r_v<int, Qual> && std::is_nothrow_invocable_r_v<int, Inv>
               : std::is_invocable_r_v<int, Qual> && std::is_invocable_r_v<int, Inv>;
  static_assert(std::is_constructible_v<Destination, Source> == (std::is_same_v<Source, Destination> || callable));
  if constexpr (std::is_constructible_v<Destination, Source>) {
    Source source           = Qualified{};
    int expected            = invoke(source);
    Destination destination = std::move(source);
    assert(invoke(destination) == expected);
    Source empty;
    Destination from_empty = std::move(empty);
    assert(!from_empty);
  }
}

template <class F>
void for_each_signature(F test) {
  test.template operator()<std::move_only_function<int()>, false, 0, false>();
  test.template operator()<std::move_only_function<int() &>, false, 1, false>();
  test.template operator()<std::move_only_function<int() &&>, false, 2, false>();
  test.template operator()<std::move_only_function<int() const>, true, 0, false>();
  test.template operator()<std::move_only_function<int() const&>, true, 1, false>();
  test.template operator()<std::move_only_function<int() const&&>, true, 2, false>();
  test.template operator()<std::move_only_function<int() noexcept>, false, 0, true>();
  test.template operator()<std::move_only_function<int() & noexcept>, false, 1, true>();
  test.template operator()<std::move_only_function<int() && noexcept>, false, 2, true>();
  test.template operator()<std::move_only_function<int() const noexcept>, true, 0, true>();
  test.template operator()<std::move_only_function<int() const & noexcept>, true, 1, true>();
  test.template operator()<std::move_only_function<int() const && noexcept>, true, 2, true>();
}

int main(int, char**) {
  for_each_signature([]<class Source, bool, int, bool> {
    for_each_signature([]<class Destination, bool Const, int Ref, bool Noexcept> {
      test_conversion<Source, Destination, Const, Ref, Noexcept>();
    });
  });
  return 0;
}
