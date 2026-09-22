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
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>

#include "test_macros.h"

struct Qualified {
  int operator()() & noexcept { return 1; }
  int operator()() const& noexcept { return 2; }
  int operator()() && noexcept { return 3; }
  int operator()() const&& noexcept { return 4; }
};

template <class F>
void test_specialization() {
  static_assert(std::is_same_v<typename F::result_type, int>);
  static_assert(std::is_nothrow_default_constructible_v<F>);
  static_assert(std::is_nothrow_move_constructible_v<F>);
  static_assert(!std::is_copy_constructible_v<F>);
  static_assert(!std::is_copy_assignable_v<F>);
  F empty;
  F null = nullptr;
  assert(!empty && !null);
  assert(empty == nullptr && nullptr == empty);
  F f = Qualified{};
  assert(f && f != nullptr && nullptr != f);
  F g = std::move(f);
  assert(g);
  static_assert(noexcept(g.swap(null)));
  static_assert(noexcept(swap(g, null)));
  swap(g, null);
  assert(!g && null);
  assert(&(g = std::move(null)) == &g);
  assert(g);
  assert(&(g = nullptr) == &g);
  assert(!g);
  assert(&(g = Qualified{}) == &g);
  assert(g);
  null = nullptr; // A moved-from wrapper need not be empty.
  swap(empty, null);
  assert(!empty && !null);
}

template <bool Noexcept>
void test_qualifiers() {
  test_specialization<std::move_only_function<int() noexcept(Noexcept)>>();
  test_specialization<std::move_only_function<int() & noexcept(Noexcept)>>();
  test_specialization<std::move_only_function<int() && noexcept(Noexcept)>>();
  test_specialization<std::move_only_function<int() const noexcept(Noexcept)>>();
  test_specialization<std::move_only_function<int() const & noexcept(Noexcept)>>();
  test_specialization<std::move_only_function<int() const && noexcept(Noexcept)>>();
  std::move_only_function<int() noexcept(Noexcept)> a = Qualified{};
  assert(a() == 1);
  assert(std::move(a)() == 1);
  std::move_only_function<int() & noexcept(Noexcept)> b = Qualified{};
  assert(b() == 1);
  std::move_only_function<int() && noexcept(Noexcept)> c = Qualified{};
  assert(std::move(c)() == 3);
  assert(c);
  const std::move_only_function<int() const noexcept(Noexcept)> d = Qualified{};
  assert(d() == 2);
  assert(std::move(d)() == 2);
  const std::move_only_function<int() const & noexcept(Noexcept)> e = Qualified{};
  assert(e() == 2);
  const std::move_only_function<int() const && noexcept(Noexcept)> f = Qualified{};
  assert(std::move(f)() == 4);
  static_assert(noexcept(a()) == Noexcept);
  static_assert(noexcept(std::move(c)()) == Noexcept);
  static_assert(noexcept(d()) == Noexcept);
  static_assert(noexcept(std::move(f)()) == Noexcept);
}

struct Immovable {
  int value;
  explicit Immovable(int n) : value(n) {}
  Immovable(std::initializer_list<int>& values, int n) : value(n) {
    for (int v : values)
      value += v;
  }
  Immovable(std::initializer_list<int>&&, int) = delete;
  Immovable(Immovable&&)                       = delete;
  int operator()() const { return value; }
};

struct Tracked {
  int* live;
  int value;
  Tracked(int& count, int v) : live(&count), value(v) { ++*live; }
  Tracked(Tracked&& other) noexcept : Tracked(*other.live, other.value) {}
  Tracked(const Tracked&)       = delete;
  Tracked& operator=(Tracked&&) = delete;
  ~Tracked() { --*live; }
  int operator()() const { return value; }
};

void test_lifetime() {
  int live = 0;
  {
    std::move_only_function<int()> a(std::in_place_type<Tracked>, live, 42);
    assert(live == 1);
    auto b = std::move(a);
    assert(b() == 42);
    a = nullptr;
    std::move_only_function<int()> c(std::in_place_type<Tracked>, live, 17);
    assert(live == 2);
    b.swap(c);
    assert(b() == 17 && c() == 42);
    c.swap(c);
    assert(c() == 42);
    b = std::move(c);
    assert(b() == 42);
    b = nullptr;
    c = nullptr;
    a = nullptr;
    assert(live == 0);
  }
  assert(live == 0);
  std::move_only_function<int()> a(std::in_place_type<Immovable>, 42);
  std::move_only_function<int()> b(std::in_place_type<Immovable>, {1, 2, 3}, 4);
  assert(a() == 42 && b() == 10);
  swap(a, b);
  assert(a() == 10 && b() == 42);
  b = std::move(a);
  assert(b() == 10);
  auto& self = b;
  b          = std::move(self);
  // Self-move leaves a valid wrapper, with an unspecified value.
  b = nullptr;
}

int function(int n) { return n + 1; }
struct Object {
  int value = 42;
  int call(int n) const noexcept { return value + n; }
};

struct FalseCallable {
  explicit operator bool() const { return false; }
  int operator()() const { return 42; }
};

struct Initialization {
  int value;
  explicit Initialization(int n) : value(n) {}
  Initialization(std::initializer_list<int>) : value(-1) {}
  int operator()() const { return value; }
};

struct Copyable {
  int* copies;
  int* moves;
  Copyable(int& c, int& m) : copies(&c), moves(&m) {}
  Copyable(const Copyable& other) : copies(other.copies), moves(other.moves) { ++*copies; }
  Copyable(Copyable&& other) noexcept : copies(other.copies), moves(other.moves) { ++*moves; }
  int operator()() const { return 42; }
};

void test_targets() {
  std::move_only_function<int(int)> a = function;
  assert(a(41) == 42);
  a = static_cast<int (*)(int)>(nullptr);
  assert(!a && a == nullptr && nullptr == a);
  Object object;
  std::move_only_function<int(const Object&, int) noexcept> b = &Object::call;
  assert(b(object, 1) == 43);
  b = static_cast<int (Object::*)(int) const noexcept>(nullptr);
  assert(!b);
  std::move_only_function<int&(Object&)> c = &Object::value;
  assert(&c(object) == &object.value);
  c = static_cast<int Object::*>(nullptr);
  assert(!c);
  Qualified target;
  std::move_only_function<int()> d = std::ref(target);
  assert(d() == 1);
  std::move_only_function<int()> e = [p = std::make_unique<int>(42)] { return *p; };
  auto f                           = std::move(e);
  assert(f() == 42);
  std::function<void()> empty;
  std::move_only_function<void()> g = empty;
  assert(g); // An empty std::function is still a target.
  std::move_only_function<void()> h(std::in_place_type<void (*)()>, nullptr);
  assert(h); // Only the forwarding constructor filters null targets.
  std::move_only_function<void()> borrowed = std::ref(empty);
  assert(borrowed);
  std::move_only_function<int()> false_callable = FalseCallable{};
  assert(false_callable && false_callable() == 42);
  int copies = 0, moves = 0;
  Copyable source(copies, moves);
  std::move_only_function<int()> copied = source;
  assert(copied() == 42 && copies == 1 && moves == 0);
  std::move_only_function<int()> moved = std::move(source);
  assert(moved() == 42 && copies == 1 && moves == 1);
  std::move_only_function<int()> direct(std::in_place_type<Initialization>, 42);
  assert(direct() == 42); // Direct-non-list-initialization must select the int constructor.
#ifndef TEST_HAS_NO_EXCEPTIONS
  try {
    g();
    assert(false);
  } catch (const std::bad_function_call&) {
  }
#endif
}

struct Argument {
  int moves  = 0;
  Argument() = default;
  Argument(Argument&& other) : moves(other.moves + 1) {}
  Argument(const Argument&) = delete;
};
struct Result {
  Result()         = default;
  Result(Result&&) = delete;
};

void test_forwarding() {
  std::move_only_function<int(Argument)> f = [](Argument&& arg) { return arg.moves; };
  assert(f(Argument{}) == 0);
  Argument arg;
  assert(f(std::move(arg)) == 1);
  std::move_only_function<Result()> g     = [] { return Result{}; };
  [[maybe_unused]] Result result          = g();
  std::move_only_function<void()> discard = [] { return 42; };
  discard();
}

void test_conversion() {
  std::move_only_function<int() const noexcept> a = Qualified{};
  std::move_only_function<int()> b                = std::move(a);
  assert(b() == 2); // Preserve the source wrapper's invocation qualifications.
  std::move_only_function<int() &&> c = std::move(b);
  assert(std::move(c)() == 2);
  std::move_only_function<long() &&> d = std::move(c);
  assert(std::move(d)() == 2);
  std::move_only_function<int() noexcept> empty;
  std::move_only_function<int()> e = std::move(empty);
  assert(!e);
}

#ifndef TEST_HAS_NO_EXCEPTIONS
struct Throws {
  Throws() { throw 42; }
  Throws(std::initializer_list<int>&) { throw 17; }
  int operator()() noexcept { return 0; }
};
struct ThrowingCopy {
  ThrowingCopy() = default;
  ThrowingCopy(const ThrowingCopy&) { throw 17; }
  int operator()() { return 0; }
};
void test_exceptions() {
  try {
    // A nonthrowing call signature does not make target construction nonthrowing.
    std::move_only_function<int() noexcept> f(std::in_place_type<Throws>);
    assert(false);
  } catch (int n) {
    assert(n == 42);
  }
  try {
    std::move_only_function<int() noexcept> f(std::in_place_type<Throws>, {1, 2});
    assert(false);
  } catch (int n) {
    assert(n == 17);
  }
  std::move_only_function<int()> f = [] { return 42; };
  ThrowingCopy target;
  try {
    f = target;
    assert(false);
  } catch (int n) {
    assert(n == 17 && f() == 42);
  }
  f = []() -> int { throw 42; };
  try {
    f();
    assert(false);
  } catch (int n) {
    assert(n == 42);
  }
}
#endif

int main(int, char**) {
  test_qualifiers<false>();
  test_qualifiers<true>();
  test_lifetime();
  test_targets();
  test_forwarding();
  test_conversion();
#ifndef TEST_HAS_NO_EXCEPTIONS
  test_exceptions();
#endif
  return 0;
}
