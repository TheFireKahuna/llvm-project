//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium

// Windows Itanium's C library is the Universal CRT, which declares the
// const-correct C++ overloads of strchr, wcschr and the like itself. libc++
// uses them instead of adding its own.

#include <string.h>
#include <wchar.h>

#include "test_macros.h"

#if !defined(_LIBCPP_STRING_H_HAS_CONST_OVERLOADS)
#  error "libc++ should use the Universal CRT's overloads in <string.h>"
#endif
#if _LIBCPP_HAS_WIDE_CHARACTERS && !defined(_LIBCPP_WCHAR_H_HAS_CONST_OVERLOADS)
#  error "libc++ should use the Universal CRT's overloads in <wchar.h>"
#endif

const char* (*narrow_const)(const char*, int)         = &::strchr;
char* (*narrow)(char*, int)                           = &::strchr;
const wchar_t* (*wide_const)(const wchar_t*, wchar_t) = &::wcschr;
wchar_t* (*wide)(wchar_t*, wchar_t)                   = &::wcschr;
