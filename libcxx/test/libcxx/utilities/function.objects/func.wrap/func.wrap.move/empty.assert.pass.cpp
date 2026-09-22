//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: std-at-least-c++23
// REQUIRES: has-unix-headers
// REQUIRES: libcpp-hardening-mode={{extensive|debug}}
// XFAIL: libcpp-hardening-mode=debug && availability-verbose_abort-missing

#include <functional>
#include "check_assertion.h"

int main(int, char**) {
  std::move_only_function<void()> f;
  TEST_LIBCPP_ASSERT_FAILURE(f(), "move_only_function has no target");
  std::move_only_function<void() noexcept> g;
  TEST_LIBCPP_ASSERT_FAILURE(g(), "move_only_function has no target");
  return 0;
}
