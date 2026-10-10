// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -fsyntax-only -verify -std=c++03 %s
// expected-no-diagnostics

// C++ sees the UCRT's stdlib.h and math.h as C headers: without their C++
// declarations, whose names the C++ library owns, but with C linkage and C17's
// view of any header they include first. The C-only min and max macros do not
// escape, and NULL is still C++'s.

#define min(a, b) caller_min
#include <math.h>
#include <stdlib.h>

extern "C" int __ucrt_stdlib_function(void) throw();
extern "C" double __ucrt_math_function(double) throw();

#if !__UCRT_STDLIB_SAW_C17
#error "headers included from stdlib.h see C17"
#endif
#if !defined(min) || defined(max)
#error "min and max are the caller's"
#endif

int *null_pointer = NULL;
