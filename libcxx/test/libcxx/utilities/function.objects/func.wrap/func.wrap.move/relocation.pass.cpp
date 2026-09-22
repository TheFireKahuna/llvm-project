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
#include <utility>

#include "test_macros.h"

struct Deleter {
  int* deletions;
  void operator()(int* p) const {
    ++*deletions;
    delete p;
  }
};

struct ObservableDeleter {
  int* moves;
  explicit ObservableDeleter(int& n) : moves(&n) {}
  ObservableDeleter(ObservableDeleter&& other) noexcept : moves(other.moves) { ++*moves; }
  void operator()(int* p) const { delete p; }
};

int main(int, char**) {
#if __has_builtin(__builtin_is_bitwise_relocatable)
  static_assert(__builtin_is_bitwise_relocatable(std::unique_ptr<int>));
  static_assert(__builtin_is_bitwise_relocatable(std::unique_ptr<int[]>));
  static_assert(__builtin_is_bitwise_relocatable(std::unique_ptr<int, Deleter>));
#  if !TEST_HAS_FEATURE(address_sanitizer)
  // Sanitizer field padding can disqualify an otherwise relocatable representation.
  static_assert(__builtin_is_bitwise_relocatable(std::unique_ptr<int, ObservableDeleter&>));
#  endif
  static_assert(!__builtin_is_bitwise_relocatable(std::unique_ptr<int, ObservableDeleter>));
#endif
  int deletions = 0;
  {
    auto target = [p = std::unique_ptr<int, Deleter>(new int(42), Deleter{&deletions})] { return *p; };
#if __has_builtin(__builtin_is_bitwise_relocatable)
    static_assert(__builtin_is_bitwise_relocatable(decltype(target)));
#endif
    std::move_only_function<int()> a = std::move(target);
    auto b                           = std::move(a);
    auto c                           = std::move(b);
    assert(c() == 42 && deletions == 0);
  }
  assert(deletions == 1);
  int moves = 0;
  {
    auto target = [p = std::unique_ptr<int, ObservableDeleter>(new int(42), ObservableDeleter(moves))] { return *p; };
#if __has_builtin(__builtin_is_bitwise_relocatable)
    static_assert(!__builtin_is_bitwise_relocatable(decltype(target)));
#endif
    std::move_only_function<int()> a = std::move(target);
    int before                       = moves;
    auto b                           = std::move(a);
    assert(moves == before + 1);
    assert(b() == 42);
  }
  return 0;
}
