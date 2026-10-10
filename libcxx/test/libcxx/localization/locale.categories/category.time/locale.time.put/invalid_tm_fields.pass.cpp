//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// UNSUPPORTED: no-localization

// MSVC's runtime keeps its default invalid-parameter handler, which terminates
// the process.
// XFAIL: msvc

// <locale>

// A tm field outside its range must not terminate the process. A conversion
// that does not use the field formats normally; one that does produces
// unspecified characters, which is nothing on Windows.

#include <cassert>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "test_macros.h"

template <class CharT>
std::basic_string<CharT> format(const std::tm& t, const CharT* fmt) {
  std::basic_ostringstream<CharT> os;
  os << std::put_time(&t, fmt);
  return os.str();
}

int main(int, char**) {
  std::tm t = {};
  t.tm_hour = 12;
  t.tm_min  = 34;
  // tm_mday stays 0, outside strftime's range of [1, 31].
  assert(format(t, "%H:%M") == "12:34");
#ifdef _WIN32
  assert(format(t, "%d") == "");
#endif
#ifndef TEST_HAS_NO_WIDE_CHARACTERS
  assert(format(t, L"%H:%M") == L"12:34");
#endif

  return 0;
}
