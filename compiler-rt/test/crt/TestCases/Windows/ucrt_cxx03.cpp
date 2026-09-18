// RUN: %clangxx_crt_main -std=c++03 -D_LIBCPP_USE_FROZEN_CXX03_HEADERS -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe
// RUN: %clangxx_crt_main -std=c++03 -D_LIBCPP_USE_FROZEN_CXX03_HEADERS -DUCRT_FIRST -UNDEBUG %s -o %t.first.exe
// RUN: %run %t.first.exe
// RUN: %clangxx_crt_main -std=c++03 -D_LIBCPP_USE_FROZEN_CXX03_HEADERS -DSDK_FIRST -UNDEBUG %s -o %t.sdk.exe
// RUN: %run %t.sdk.exe
// REQUIRES: windows, crt

// UCRT configuration may already have been included before libc++.
#ifdef UCRT_FIRST
#  include <corecrt.h>
#endif
#ifdef SDK_FIRST
#  include <windows.h>
#endif

#include <cassert>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
  assert(std::abs(-7L) == 7L);
  assert(std::div(7, 3).quot == 2);
  std::vector<std::string> values(2, "C++03");
  assert(values.at(1) == "C++03");
  try {
    (void)values.at(2);
  } catch (const std::out_of_range &error) {
    assert(error.what()[0]);
    return 0;
  }
  assert(false);
}
