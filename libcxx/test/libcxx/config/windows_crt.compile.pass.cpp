//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: windows

// CRT identity must be available even when libc++ is included before the CRT.
#if __cplusplus < 201103L && defined(_LIBCPP_USE_FROZEN_CXX03_HEADERS)
#  include <__cxx03/__config>
#else
#  include <__config>
#endif

#if defined(__NTPOSIX__)
#  if defined(_LIBCPP_MSVCRT_LIKE) || defined(_LIBCPP_MSVCRT) || defined(_LIBCPP_UCRT)
#    error NT-POSIX does not use the Microsoft CRT.
#  endif
#else
#  if !defined(_LIBCPP_MSVCRT_LIKE)
#    error Microsoft CRT interfaces must be available independently of the C++ ABI.
#  endif
#  if defined(_LIBCPP_MSVCRT) == defined(_LIBCPP_UCRT)
#    error Select exactly one Microsoft CRT generation.
#  endif

#  include <stdlib.h>
#  if defined(_LIBCPP_UCRT) != defined(_UCRT)
#    error CRT identity must not depend on header inclusion order.
#  endif

#  if defined(_WIN32_ITANIUM)
#    if !defined(_LIBCPP_UCRT) || !defined(_LIBCPP_ABI_ITANIUM) || defined(_LIBCPP_ABI_VCRUNTIME)
#      error Windows Itanium uses UCRT with its own C++ ABI runtime.
#    endif
#  endif
#endif

// CRT generation must not suppress declarations supplied by libc++'s wrappers
// or duplicate overloads supplied by the underlying headers.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <fstream>

void test_cpp_overloads() {
  (void)::isfinite(1);
  (void)::isgreater(1, 2.0);
  (void)::abs(1L);
  (void)::div(5L, 2L);
  const char* (*narrow)(const char*, int)         = &::strchr;
  const wchar_t* (*wide)(const wchar_t*, wchar_t) = &::wcschr;
  (void)narrow;
  (void)wide;
}
