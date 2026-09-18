//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// UNSUPPORTED: no-exceptions

// RUN: %{cxx} %{flags} %{compile_flags} -DABI_FIRST %s %{link_flags} -o %t.abi
// RUN: %{exec} %t.abi
// RUN: %{cxx} %{flags} %{compile_flags} %s %{link_flags} -o %t.cxx
// RUN: %{exec} %t.cxx
// RUN: %{cxx} %{flags} %{compile_flags} -std=c++03 -D_LIBCPP_USE_FROZEN_CXX03_HEADERS -DABI_FIRST -fsyntax-only %s
// RUN: %{cxx} %{flags} %{compile_flags} -std=c++03 -D_LIBCPP_USE_FROZEN_CXX03_HEADERS -fsyntax-only %s

// clang-format off
#ifdef ABI_FIRST
#include <cxxabi.h>
#include <exception>
#else
#include <exception>
#include <cxxabi.h>
#endif
// clang-format on

#include <cassert>
#include "test_macros.h"

int main(int, char**) {
#if TEST_STD_VER >= 11
  std::exception_ptr saved = std::make_exception_ptr(42);
  try {
    std::rethrow_exception(saved);
  } catch (int value) {
    assert(value == 42);
    return 0;
  }
  assert(false);
#endif
  return 0;
}
