// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -fsyntax-only -verify %t/test.c
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -fsyntax-only -verify %t/test.c
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -fsyntax-only -verify -x c++ %t/test.c
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -fsyntax-only -verify -x c++ %t/test.c
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -fsyntax-only -verify -x c++ %t/test.c \
// RUN:   -D_CRT_DECLARE_NONSTDC_NAMES=1
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -fsyntax-only -verify -x c++ %t/test.c \
// RUN:   -D_CRT_DECLARE_NONSTDC_NAMES=1 -DCALLER_COMPLEX
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -fsyntax-only -verify %t/test.c \
// RUN:   -D_CRT_STDIO_LEGACY_WIDE_SPECIFIERS

// Windows Itanium compiles against the Universal CRT and the Windows SDK
// without Visual C++. The wrapper directory stands in for the Visual C++
// headers and adapts the UCRT and SDK headers below, which model the parts the
// wrappers depend on.

//--- corecrt.h
#pragma once
#include <vcruntime.h>
#define _ACRTIMP __declspec(dllimport)
#define _CONST_RETURN
_CRT_BEGIN_C_HEADER
struct __crt_packed { char c; __int128 i; };
int __crt_c_linkage(void);
_CRT_END_C_HEADER

//--- corecrt_stdio_config.h
#if defined(_CRT_STDIO_ISO_WIDE_SPECIFIERS) ==                                 \
    defined(_CRT_STDIO_LEGACY_WIDE_SPECIFIERS)
#error "wide format specifiers are not ISO C's unless legacy ones are asked for"
#endif

//--- stdlib.h
#include <corecrt.h>
#include <stddef.h>
#ifdef __cplusplus
#error "the C++ library owns the C++ declarations"
#endif
#ifndef __cplusplus
#define min(a, b) a
#define max(a, b) b
#endif

//--- math.h
#include <corecrt.h>
#ifdef __cplusplus
#error "the C++ library owns the C++ declarations"
#endif
#ifndef __cplusplus
#if _CRT_DECLARE_NONSTDC_NAMES
#define complex _complex
#endif
#define _matherrl _matherr
#endif

//--- new.h
#ifdef _MSC_EXTENSIONS
#error "the C++ library owns std::new_handler"
#endif

//--- new
#pragma once

//--- sal.h
#define __null

//--- ntdef.h
#ifndef DECLSPEC_ALIGN
#define DECLSPEC_ALIGN(x)
#endif
typedef struct DECLSPEC_ALIGN(16) _M128A { long long Low, High; } M128A;

//--- winnt.h
#include <ntdef.h>
#define DEFAULT_UNREACHABLE

//--- sdkddkver.h

//--- wrl/def.h
#if _MSC_VER < 1600
#error "Visual C++ 2010 or later is required"
#endif

//--- test.c
// expected-no-diagnostics
#ifdef __cplusplus
#define _Static_assert static_assert
#define _Alignof alignof
#define max 2
#ifdef CALLER_COMPLEX
#define complex 3
#endif
#endif

#include <stdlib.h>
#include <math.h>
#include <stdnoreturn.h>
#include <setjmp.h>
#include <excpt.h>
#include <vcruntime_startup.h>
#include <vcruntime_string.h>
#include <corecrt_stdio_config.h>
#include <sal.h>
#include <winnt.h>

// The UCRT's declarations are packed as Visual C++ packs them, with C linkage
// in C++.
_Static_assert(_CRT_PACKING == 8, "");
_Static_assert(sizeof(struct __crt_packed) == 24, "");
#ifdef __cplusplus
extern "C" int __crt_c_linkage(void);
#endif
_Static_assert(sizeof(intptr_t) == sizeof(void *), "");
_Static_assert(sizeof(uintptr_t) == sizeof(void *), "");
_VCRTIMP int __crt_imported;
static char __crt_array[5];
_Static_assert(__crt_countof(__crt_array) == 5, "");

// The C view of stdlib.h leaves C++'s min and max as they were.
#ifdef __cplusplus
#ifdef min
#error "min leaks out of stdlib.h"
#endif
_Static_assert(max == 2, "");
#endif

// The C view of math.h leaves C++'s complex and _matherrl as they were.
#ifdef __cplusplus
#ifdef _matherrl
#error "_matherrl leaks out of math.h"
#endif
#ifdef CALLER_COMPLEX
_Static_assert(complex == 3, "");
#elif defined(complex)
#error "complex leaks out of math.h"
#endif
#endif

// The UCRT's stdlib.h and math.h are C headers to C++, and their NULL and
// wchar_t are C++'s.
int *null_pointer = NULL;

#ifdef __x86_64__
_Static_assert(sizeof(jmp_buf) == 256, "");
#else
_Static_assert(sizeof(jmp_buf) == 192, "");
#endif

EXCEPTION_DISPOSITION disposition = ExceptionContinueSearch;
_crt_argv_mode argv_mode = _crt_argv_no_arguments;
const void *scan(const void *p) { return memchr(p, 0, 1); }

// The SDK's sal.h leaves clang's __null alone.
#ifdef __cplusplus
int *null_keyword = __null;
#endif

// winnt.h and ntdef.h see the declaration specifiers clang supports.
_Static_assert(_Alignof(M128A) == 16, "");
int unreachable(int i) {
  switch (i) {
  case 0:
    return 1;
    DEFAULT_UNREACHABLE;
  }
}

#ifdef __cplusplus
#include <new.h>
#include <vcruntime_new_debug.h>
#include <wrl/def.h>
#ifdef _MSC_VER
#error "_MSC_VER leaks out of wrl/def.h"
#endif
int *debug_new() { return new (1, "file", 2) int; }
#else
#include <complex.h>
_Dcomplex complex_value(void) { return CMPLX(1.0, 2.0); }
double real_part(_Dcomplex z) { return creal(z); }
_Static_assert(_Generic(&cpowf, float _Complex (*)(float _Complex,
                                                    float _Complex): 1), "");
_Static_assert(_Generic(&_LCbuild, long double _Complex (*)(long double,
                                                            long double): 1),
               "");
#endif
