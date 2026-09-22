//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: std-at-least-c++23

// RUN: %{cxx} %s %{flags} %{compile_flags} -c -DPRODUCER -o %t.producer.o
// RUN: %{cxx} %s %{flags} %{compile_flags} -c -DOPAQUE -o %t.opaque.o
// RUN: %{cxx} %s %{flags} %{compile_flags} -c -DCONSUMER -o %t.consumer.o
// RUN: %{cxx} %t.producer.o %t.opaque.o %t.consumer.o %{flags} %{link_flags} -o %t.exe
// RUN: %{exec} %t.exe

#include <__functional/move_only_function_dispatch.h>
#include <cassert>
#include <type_traits>
#include <utility>

using Base = std::__move_only_function::__dispatch_base;
using Mode = std::__move_only_function::__argument_passing;
template <class Signature, Mode Arguments>
using Table = std::__move_only_function::__dispatch_table<Signature, Arguments>;

// Deliberately requires a complete value type: the test must fail if argument
// classification is moved into the handle's layout or member declarations.
template <class T>
using Argument = std::conditional_t<(sizeof(T) <= sizeof(void*)) && std::is_trivially_copyable_v<T>, T, T&&>;

template <class Signature>
struct Handle;

template <class R, class... Args, bool Noexcept>
struct Handle<R(Args...) noexcept(Noexcept)> {
  void* storage     = nullptr;
  const Base* table = nullptr;

  R operator()(Args... args) const noexcept(Noexcept) {
    using ReferenceInvoker = R(void*, Args&&...) noexcept(Noexcept);
    using ValueInvoker     = R(void*, Argument<Args>...) noexcept(Noexcept);
    return std::__move_only_function::__dispatch_call<ReferenceInvoker, ValueInvoker>::__call(
        table, storage, std::forward<Args>(args)...);
  }

  void destroy() {
    if (table != nullptr) {
      table->__manage_(nullptr, storage);
      table = nullptr;
    }
  }
};

struct Small;
struct Result;
using Throwing         = Handle<int(Small)>;
using Nothrow          = Handle<int(Small) noexcept>;
using IncompleteResult = Handle<Result()>;

Nothrow make_handle(int*);
Nothrow make_incomplete_handle(int*);
Throwing weaken(Nothrow);
void destroy_handle(Throwing&);
void exercise_incomplete_result();

#if defined(OPAQUE) || defined(CONSUMER)
// These instantiations precede the type definitions. The opaque TU never sees
// either definition, but can still copy metadata, weaken noexcept, and destroy.
static_assert(sizeof(Throwing) == sizeof(Nothrow));
static_assert(sizeof(IncompleteResult) == sizeof(Throwing));
#endif

#if defined(PRODUCER) || defined(CONSUMER)
struct Small {
  int value;
};
struct Result {
  int value;
};
#endif

#if defined(PRODUCER)
int invoke(void* storage, Small arg) noexcept { return *static_cast<int*>(storage) + arg.value; }
void manage(void*, void* storage) noexcept { ++*static_cast<int*>(storage); }

Nothrow make_handle(int* storage) {
  using TypedTable = Table<int(void*, Argument<Small>) noexcept, Mode::__value>;
  static constexpr TypedTable table{manage, invoke};
  return {storage, &table};
}
#elif defined(OPAQUE)
int invoke_incomplete(void* storage, Small&&) noexcept { return *static_cast<int*>(storage); }
void manage_incomplete(void*, void* storage) noexcept { ++*static_cast<int*>(storage); }

Nothrow make_incomplete_handle(int* storage) {
  using TypedTable = Table<int(void*, Small&&) noexcept, Mode::__reference>;
  static constexpr TypedTable table{manage_incomplete, invoke_incomplete};
  return {storage, &table};
}

Throwing weaken(Nothrow source) { return {source.storage, source.table}; }
void destroy_handle(Throwing& handle) { handle.destroy(); }
void exercise_incomplete_result() {
  IncompleteResult handle;
  handle.destroy();
}
#elif defined(CONSUMER)
int main(int, char**) {
  int value      = 40;
  Nothrow source = make_handle(&value);
  assert(source(Small{2}) == 42);
  Throwing destination              = weaken(source);
  const Base* volatile opaque_table = destination.table;
  destination.table                 = opaque_table;
  assert(destination(Small{3}) == 43);
  assert(destination.table == source.table);
  destroy_handle(destination);
  assert(value == 41);
  assert(destination.table == nullptr);

  source = make_incomplete_handle(&value);
  assert(source(Small{2}) == 41);
  destination       = weaken(source);
  opaque_table      = destination.table;
  destination.table = opaque_table;
  assert(destination(Small{3}) == 41);
  assert(destination.table == source.table);
  assert(destination.table->__arguments_ == Mode::__reference);
  destroy_handle(destination);
  assert(value == 42);
  exercise_incomplete_result();
  return 0;
}
#endif
