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

struct Small {
  int x, y;
};
struct Odd {
  char bytes[3];
};
struct Large {
  void* pointers[3];
};
struct Volatile {
  volatile int x;
};
struct Move {
  int value = 42;
  Move()    = default;
  Move(Move&& other) : value(other.value + 1) {}
};
template <class T>
using Adjusted = typename std::__function::__small_forward<T, std::__function::__can_use_small_value<T, true>()>::type;
static_assert(std::is_same_v<Adjusted<Odd>, Odd&&>);
static_assert(std::is_same_v<Adjusted<Large>, Large&&>);
static_assert(std::is_same_v<Adjusted<Volatile>, Volatile&&>);
static_assert(std::is_same_v<Adjusted<Move>, Move&&>);
static_assert(std::is_same_v<Adjusted<Small&>, Small&>);
static_assert(std::is_same_v<Adjusted<const Small&>, const Small&>);
static_assert(std::is_same_v<Adjusted<int>, int>);
#if __has_builtin(__builtin_is_bitwise_relocatable)
static_assert(std::is_same_v<Adjusted<Small>, std::conditional_t<sizeof(Small) <= sizeof(void*), Small, Small&&>>);
#endif

int main(int, char**) {
  std::move_only_function<int(Small) noexcept> f = [](Small&& x) noexcept { return x.x + x.y; };
  assert(f(Small{17, 25}) == 42);
  std::move_only_function<int(Small)> g = std::move(f);
  assert(g(Small{1, 2}) == 3);
  Small s{1, 2};
  std::move_only_function<Small&(Small&)> h = [](Small& x) -> Small& { return x; };
  assert(&h(s) == &s);
  std::move_only_function<int(Move)> m = [](Move&& x) { return x.value; };
  assert(m(Move{}) == 42);
  Move x;
  assert(m(std::move(x)) == 43);
  std::move_only_function<int(Volatile)> v = [](Volatile&& x) { return x.x; };
  assert(v(Volatile{42}) == 42);
  return 0;
}
