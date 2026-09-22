//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: std-at-least-c++23

#include <__functional/move_only_function_dispatch.h>
#include <cassert>
#include <type_traits>
#include <utility>

#include "test_macros.h"

using Base = std::__move_only_function::__dispatch_base;
using Mode = std::__move_only_function::__argument_passing;
template <class Signature, Mode Arguments = Mode::__reference>
using Table = std::__move_only_function::__dispatch_table<Signature, Arguments>;
template <class Signature>
using View = std::__move_only_function::__dispatch_view<Signature>;
template <class ReferenceInvoker, class ValueInvoker>
using Call = std::__move_only_function::__dispatch_call<ReferenceInvoker, ValueInvoker>;

struct Small {
  int value;
};

int invoke(void* storage, Small arg) noexcept { return *static_cast<int*>(storage) + arg.value; }

void manage(void* destination, void* source) noexcept {
  if (destination != nullptr)
    *static_cast<int*>(destination) = *static_cast<int*>(source);
  *static_cast<int*>(source) = 0;
}

using ThrowingTable = Table<int(void*, Small), Mode::__value>;
using NothrowTable  = Table<int(void*, Small) noexcept, Mode::__value>;
using ThrowingView  = View<int(void*, Small)>;
using NothrowView   = View<int(void*, Small) noexcept>;

constexpr ThrowingTable throwing_table{manage, invoke};
constexpr NothrowTable nothrow_table{manage, invoke};

static_assert(std::is_same_v<decltype(throwing_table.__invoke_), int (*)(void*, Small)>);
static_assert(std::is_same_v<decltype(nothrow_table.__invoke_), int (*)(void*, Small) noexcept>);
static_assert(!noexcept(throwing_table.__invoke_(nullptr, Small{})));
static_assert(noexcept(nothrow_table.__invoke_(nullptr, Small{})));
static_assert(std::is_convertible_v<const NothrowTable*, const ThrowingView*>);
static_assert(!std::is_convertible_v<const ThrowingTable*, const NothrowView*>);
static_assert(std::is_trivially_copyable_v<ThrowingTable>);
static_assert(std::is_trivially_copyable_v<NothrowTable>);
static_assert(throwing_table.__arguments_ == Mode::__value);
static_assert(nothrow_table.__arguments_ == Mode::__value);
static_assert(!std::is_constructible_v<NothrowTable, Base::__manager, int (*)(void*, Small)>);

struct NonTrivial {
  int moves                     = 0;
  NonTrivial()                  = default;
  NonTrivial(const NonTrivial&) = delete;
  NonTrivial(NonTrivial&& other) : moves(other.moves + 1) {}
};

NonTrivial& forward_argument(void*, NonTrivial&& arg) noexcept { return static_cast<NonTrivial&>(arg); }

struct Incomplete;
Incomplete& forward_incomplete(void*, Incomplete& arg) noexcept { return arg; }

constexpr Table<Incomplete&(void*, Incomplete&) noexcept> incomplete_table{manage, forward_incomplete};
static_assert(incomplete_table.__arguments_ == Mode::__reference);

void test() {
  int value = 40;

  // Keep indirect calls opaque even when the test is compiled with LTO and CFI.
  const Base* volatile base = &throwing_table;
  assert(static_cast<const ThrowingView*>(base)->__invoke_(&value, Small{2}) == 42);

  base = &nothrow_table;
  assert(static_cast<const NothrowView*>(base)->__invoke_(&value, Small{3}) == 43);
  // A weakened signature recovers the actual throwing-table base subobject.
  assert(static_cast<const ThrowingView*>(base)->__invoke_(&value, Small{4}) == 44);
  assert(static_cast<const ThrowingView*>(base)->__invoke_ == nothrow_table.__invoke_);

  int destination = 0;
  base->__manage_(&destination, &value);
  assert(destination == 40);
  assert(value == 0);
  base->__manage_(nullptr, &destination);
  assert(destination == 0);

  constexpr Table<NonTrivial&(void*, NonTrivial&&) noexcept> reference_table{manage, forward_argument};
  NonTrivial arg;
  base         = &reference_table;
  auto& result = static_cast<const decltype(reference_table)*>(base)->__invoke_(nullptr, std::move(arg));
  assert(&result == &arg);
  assert(arg.moves == 0);

  using ReferenceInvoker = NonTrivial&(void*, Small&&, NonTrivial&&) noexcept;
  using ValueInvoker     = NonTrivial&(void*, Small, NonTrivial&&) noexcept;
  constexpr Table<ReferenceInvoker> mixed_reference{
      manage, +[](void*, Small&& small, NonTrivial&& object) noexcept -> NonTrivial& {
        assert(small.value == 42);
        return static_cast<NonTrivial&>(object);
      }};
  constexpr Table<ValueInvoker, Mode::__value> mixed_value{
      manage, +[](void*, Small small, NonTrivial&& object) noexcept -> NonTrivial& {
        assert(small.value == 42);
        return static_cast<NonTrivial&>(object);
      }};
  using MixedCall = Call<ReferenceInvoker, ValueInvoker>;
  assert(&MixedCall::__call(&mixed_reference, nullptr, Small{42}, std::move(arg)) == &arg);
  assert(&MixedCall::__call(&mixed_value, nullptr, Small{42}, std::move(arg)) == &arg);
  assert(arg.moves == 0);

  // Reference parameters and results require no definition of the referred-to type.
  base = &incomplete_table;
  assert(static_cast<const decltype(incomplete_table)*>(base)->__invoke_ == forward_incomplete);

  // Identical invoker types need no mode branch, including after noexcept weakening.
  using VoidInvoker = void(void*, int&) noexcept;
  constexpr Table<VoidInvoker> void_reference{manage, +[](void*, int& arg) noexcept { ++arg; }};
  constexpr Table<VoidInvoker, Mode::__value> void_value{manage, +[](void*, int& arg) noexcept { ++arg; }};
  Call<VoidInvoker, VoidInvoker>::__call(&void_reference, nullptr, value);
  Call<VoidInvoker, VoidInvoker>::__call(&void_value, nullptr, value);
  using ThrowingVoidInvoker = void(void*, int&);
  Call<ThrowingVoidInvoker, ThrowingVoidInvoker>::__call(&void_value, nullptr, value);
  assert(value == 3);
}

struct Immovable {
  int value;
  explicit Immovable(int v) : value(v) {}
  Immovable(const Immovable&) = delete;
  Immovable(Immovable&&)      = delete;
};

void test_result_elision() {
  using ReferenceInvoker = Immovable(void*, Small&&);
  using ValueInvoker     = Immovable(void*, Small);
  constexpr Table<ReferenceInvoker> reference{manage, +[](void*, Small&& arg) { return Immovable(arg.value); }};
  constexpr Table<ValueInvoker, Mode::__value> value{manage, +[](void*, Small arg) { return Immovable(arg.value); }};
  using Dispatch = Call<ReferenceInvoker, ValueInvoker>;
  assert(Dispatch::__call(&reference, nullptr, Small{42}).value == 42);
  assert(Dispatch::__call(&value, nullptr, Small{43}).value == 43);
}

#ifndef TEST_HAS_NO_EXCEPTIONS
int throw_from_target(void*, Small) { throw 42; }

void test_exception() {
  constexpr ThrowingTable table{manage, throw_from_target};
  constexpr Table<int(void*, Small&&)> reference{manage, +[](void*, Small&&) -> int { throw 42; }};
  using Dispatch       = Call<int(void*, Small&&), int(void*, Small)>;
  const Base* tables[] = {&table, &reference};
  for (const Base* base : tables) {
    try {
      Dispatch::__call(base, nullptr, Small{});
      assert(false);
    } catch (int value) {
      assert(value == 42);
    }
  }
}
#endif

int main(int, char**) {
  test();
  test_result_elision();
#ifndef TEST_HAS_NO_EXCEPTIONS
  test_exception();
#endif
  return 0;
}
