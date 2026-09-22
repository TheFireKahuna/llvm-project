//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: std-at-least-c++23

#include <__functional/move_only_function.h>

#if __has_builtin(__builtin_is_bitwise_relocatable)
#  if __has_feature(ptrauth_intrinsics)

struct [[clang::trivially_relocatable(true)]] Authenticated {
  int* __ptrauth(1, 1, 42) pointer;
  int* moves;

  Authenticated(int* p, int* n) : pointer(p), moves(n) {}
  Authenticated(Authenticated&& other) noexcept : pointer(other.pointer), moves(other.moves) {
    other.pointer = nullptr;
    ++*moves;
  }
  ~Authenticated() {}
  int operator()() const noexcept { return pointer ? *pointer : 0; }
};

static_assert(!__builtin_is_bitwise_relocatable(Authenticated));
static_assert(!__builtin_is_cpp_trivially_relocatable(Authenticated));
static_assert(requires(Authenticated* p) { __builtin_trivially_relocate(p, p, 0); });

struct [[clang::trivially_relocatable(false)]] OptOut : Authenticated {
  using Authenticated::Authenticated;
};
template <class T>
constexpr bool CanRelocate = requires(T* p) { __builtin_trivially_relocate(p, p, 0); };
static_assert(!CanRelocate<OptOut>);

void test_authenticated(int* p) {
  int moves = 0;
  std::move_only_function<int() const noexcept> f(std::in_place_type<Authenticated>, p, &moves);
  auto g = std::move(f);
  if (g() != (p ? *p : 0) || moves != 0)
    __builtin_abort();

  std::move_only_function<int() const noexcept> a(std::in_place_type<OptOut>, p, &moves);
  auto b = std::move(a);
  if (b() != (p ? *p : 0) || moves != 1)
    __builtin_abort();
}
#  endif
#endif

int main(int, char**) {
#if __has_builtin(__builtin_is_bitwise_relocatable)
#  if __has_feature(ptrauth_intrinsics)
  int value = 42;
  test_authenticated(&value);
  test_authenticated(nullptr);
#  endif
#endif
  return 0;
}
