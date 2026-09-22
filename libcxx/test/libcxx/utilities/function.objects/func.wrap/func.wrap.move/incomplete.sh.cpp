//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: std-at-least-c++23
// RUN: %{cxx} %s %{flags} %{compile_flags} -c -DPRODUCER -o %t.producer.o
// RUN: %{cxx} %s %{flags} %{compile_flags} -c -DCOMPLETE -o %t.complete.o
// RUN: %{cxx} %s %{flags} %{compile_flags} -c -DCONSUMER -o %t.consumer.o
// RUN: %{cxx} %t.producer.o %t.complete.o %t.consumer.o %{flags} %{link_flags} -o %t.exe
// RUN: %{exec} %t.exe
// RUN: %{cxx} %t.consumer.o %t.complete.o %t.producer.o %{flags} %{link_flags} -o %t.reverse.exe
// RUN: %{exec} %t.reverse.exe

#include <functional>
#include <cassert>
#include <utility>

struct Small;
using Function = std::move_only_function<int(Small) noexcept>;
Function make_incomplete();
Function make_complete();
int invoke(const Small&) noexcept;

#ifdef PRODUCER
Function make_incomplete() { return invoke; }

struct Small {
  int value;
};
int invoke(const Small& s) noexcept { return s.value; }
#elif defined(COMPLETE)
struct Small {
  int value;
};
Function make_complete() {
  return [](Small s) noexcept { return s.value + 1; };
}
#else
struct Small {
  int value;
};
int main(int, char**) {
  auto a = make_incomplete();
  assert(a(Small{42}) == 42);
  std::move_only_function<int(Small)> b = std::move(a);
  assert(b(Small{17}) == 17);
  auto c = make_complete();
  assert(c(Small{41}) == 42);
  Function d = invoke;
  assert(d(Small{42}) == 42);
  return 0;
}
#endif
