//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: std-at-least-c++23

#include <functional>
#include <type_traits>
#include <utility>

using F = std::move_only_function<int(int)>;
static_assert(std::is_same_v<F::result_type, int>);
static_assert(std::is_default_constructible_v<F>);
static_assert(std::is_nothrow_default_constructible_v<F>);
static_assert(std::is_nothrow_constructible_v<F, std::nullptr_t>);
static_assert(std::is_nothrow_move_constructible_v<F>);
static_assert(std::is_move_assignable_v<F>);
static_assert(!std::is_copy_constructible_v<F>);
static_assert(!std::is_copy_assignable_v<F>);
static_assert(std::is_nothrow_destructible_v<F>);
static_assert(std::is_nothrow_swappable_v<F>);
static_assert(std::is_assignable_v<F&, std::nullptr_t>);
static_assert(!std::is_convertible_v<F, bool>);
static_assert(std::is_constructible_v<bool, F>);

struct Lvalue {
  int operator()() & noexcept;
};
struct Rvalue {
  int operator()() && noexcept;
};
struct Const {
  int operator()() const noexcept;
};
struct Throws {
  int operator()();
};

static_assert(!std::is_constructible_v<std::move_only_function<int()>, Lvalue>);
static_assert(std::is_constructible_v<std::move_only_function<int() &>, Lvalue>);
static_assert(!std::is_constructible_v<std::move_only_function<int() &&>, Lvalue>);
static_assert(!std::is_constructible_v<std::move_only_function<int() const&>, Lvalue>);
static_assert(!std::is_constructible_v<std::move_only_function<int()>, Rvalue>);
static_assert(!std::is_constructible_v<std::move_only_function<int() &>, Rvalue>);
static_assert(std::is_constructible_v<std::move_only_function<int() &&>, Rvalue>);
static_assert(std::is_constructible_v<std::move_only_function<int() const noexcept>, Const>);
static_assert(!std::is_constructible_v<std::move_only_function<int() noexcept>, Throws>);
static_assert(std::is_constructible_v<std::move_only_function<int()>, Throws>);
static_assert(!std::is_invocable_v<std::move_only_function<int() &>>);
static_assert(std::is_invocable_v<std::move_only_function<int() &>&>);
static_assert(!std::is_invocable_v<std::move_only_function<int() &&>&>);
static_assert(std::is_invocable_v<std::move_only_function<int() &&>>);
static_assert(!std::is_invocable_v<const std::move_only_function<int()>&>);
static_assert(std::is_invocable_v<const std::move_only_function<int() const>&>);
static_assert(std::is_constructible_v<std::move_only_function<int()>, std::move_only_function<int() noexcept>>);
static_assert(!std::is_constructible_v<std::move_only_function<int() noexcept>, std::move_only_function<int()>>);

struct Immovable {
  explicit Immovable(int);
  Immovable(const Immovable&) = delete;
  Immovable(Immovable&&)      = delete;
  void operator()();
};
using V = std::move_only_function<void()>;
static_assert(std::is_constructible_v<V, std::in_place_type_t<Immovable>, int>);
static_assert(!std::is_convertible_v<std::in_place_type_t<Const>, V>);
static_assert(!std::is_constructible_v<V, std::in_place_type_t<Immovable>>);
// Target construction is a mandate for the forwarding constructor, not a constraint.
static_assert(std::is_constructible_v<V, Immovable>);

struct ThrowingConversion {
  operator int() noexcept(false);
};
struct ReturnsConversion {
  ThrowingConversion operator()() noexcept;
};
static_assert(std::is_constructible_v<std::move_only_function<int()>, ReturnsConversion>);
static_assert(!std::is_constructible_v<std::move_only_function<int() noexcept>, ReturnsConversion>);

struct ThrowsOnLvalue {
  void operator()() &;
  void operator()() && noexcept;
};
struct ThrowsOnRvalue {
  void operator()() & noexcept;
  void operator()() &&;
};
static_assert(!std::is_constructible_v<std::move_only_function<void() noexcept>, ThrowsOnLvalue>);
static_assert(!std::is_constructible_v<std::move_only_function<void() noexcept>, ThrowsOnRvalue>);
static_assert(std::is_constructible_v<std::move_only_function<void() && noexcept>, ThrowsOnLvalue>);
static_assert(std::is_constructible_v<std::move_only_function<void() & noexcept>, ThrowsOnRvalue>);

struct List {
  List(std::initializer_list<int>&, int);
  List(std::initializer_list<int>&&, int) = delete;
  void operator()();
};
static_assert(std::is_constructible_v<V, std::in_place_type_t<List>, std::initializer_list<int>, int>);
static_assert(!std::is_constructible_v<V, std::in_place_type_t<List>, std::initializer_list<int>>);
static_assert(!std::is_constructible_v<V, std::in_place_type_t<List>, std::initializer_list<void*>, int>);
static_assert(!std::is_constructible_v<std::move_only_function<void() noexcept>,
                                       std::in_place_type_t<List>,
                                       std::initializer_list<int>,
                                       int>);

struct Incomplete;
using IncompleteFunction = std::move_only_function<Incomplete(Incomplete)>;
static_assert(sizeof(IncompleteFunction) > 0);
static_assert(std::is_nothrow_move_constructible_v<IncompleteFunction>);
static_assert(std::is_nothrow_destructible_v<IncompleteFunction>);
void move_incomplete(IncompleteFunction& f) {
  auto g = std::move(f);
  g      = nullptr;
}

struct CopyOnly {
  CopyOnly(const CopyOnly&) noexcept;
  void operator()() const;
};
static_assert(std::is_constructible_v<V, const CopyOnly&>);
struct HasBool {
  operator bool() const;
  void operator()() const;
};
static_assert(std::is_constructible_v<V, HasBool>);
template <class T>
concept Complete = requires { sizeof(T); };
static_assert(!Complete<std::move_only_function<void() volatile>>);
static_assert(!Complete<std::move_only_function<void() const volatile>>);
static_assert(!Complete<std::move_only_function<int>>);
static_assert(!Complete<std::move_only_function<>>);

#define CHECK_SIGNATURE(CV, REF, N)                                                                                    \
  static_assert(std::is_same_v<decltype(&std::move_only_function<int(int) CV REF noexcept(N)>::operator()),            \
                               int (std::move_only_function<int(int) CV REF noexcept(N)>::*)(int) CV REF noexcept(N)>)
CHECK_SIGNATURE(, , false);
CHECK_SIGNATURE(, &, false);
CHECK_SIGNATURE(, &&, false);
CHECK_SIGNATURE(const, , false);
CHECK_SIGNATURE(const, &, false);
CHECK_SIGNATURE(const, &&, false);
CHECK_SIGNATURE(, , true);
CHECK_SIGNATURE(, &, true);
CHECK_SIGNATURE(, &&, true);
CHECK_SIGNATURE(const, , true);
CHECK_SIGNATURE(const, &, true);
CHECK_SIGNATURE(const, &&, true);
#undef CHECK_SIGNATURE
