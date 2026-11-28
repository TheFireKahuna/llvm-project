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
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -fsyntax-only -verify -x c++ %t/test.c \
// RUN:   -DDELAYIMP_INSECURE_WRITABLE_HOOKS \
// RUN:   -DDELAYIMP_WRITABLE_DLOAD_SECTION_PROTECTION_POLICY
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -fsyntax-only -verify=strict %t/test.c -std=c99
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -fsyntax-only -verify %t/test.c -std=c11 \
// RUN:   -D_POSIX_C_SOURCE=200112L
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -emit-llvm -o - %t/test.c \
// RUN:   | FileCheck %t/test.c --implicit-check-not=@_aligned_
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -emit-llvm -o - %t/test.c \
// RUN:   -D_CRT_STDIO_INLINE=__inline | FileCheck %t/test.c --check-prefix=INLINE
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -fsyntax-only -verify %t/test.c \
// RUN:   -D_CRT_DECLARE_NONSTDC_NAMES=0
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:   -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:   -internal-isystem %t -fsyntax-only -verify %t/test.c -std=c23

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
#if (defined _CRT_DECLARE_NONSTDC_NAMES && _CRT_DECLARE_NONSTDC_NAMES) ||      \
    (!defined _CRT_DECLARE_NONSTDC_NAMES && !__STDC__)
#define _CRT_INTERNAL_NONSTDC_NAMES 1
#else
#define _CRT_INTERNAL_NONSTDC_NAMES 0
#endif

//--- corecrt_stdio_config.h
#pragma once
#if defined(_CRT_STDIO_ISO_WIDE_SPECIFIERS) ==                                 \
    defined(_CRT_STDIO_LEGACY_WIDE_SPECIFIERS)
#error "wide format specifiers are not ISO C's unless legacy ones are asked for"
#endif
#ifdef _NO_CRT_STDIO_INLINE
#undef _CRT_STDIO_INLINE
#define _CRT_STDIO_INLINE
int printf(const char *, ...);
#else
_CRT_STDIO_INLINE int printf(const char *_Format, ...) { return 0; }
#endif

//--- stdio.h
#pragma once
#include <corecrt_stdio_config.h>
#if _CRT_INTERNAL_NONSTDC_NAMES
int fileno(void *);
#endif

//--- string.h
#pragma once
#include <corecrt.h>
#if _CRT_INTERNAL_NONSTDC_NAMES
char *strdup(char const *);
#endif

//--- wchar.h
#pragma once
#include <sys/stat.h>

//--- sys/stat.h
#pragma once
#include <sys/types.h>
#if _CRT_INTERNAL_NONSTDC_NAMES
struct stat { off_t st_size; };
#endif

//--- sys/types.h
#pragma once
#if (defined _CRT_DECLARE_NONSTDC_NAMES && _CRT_DECLARE_NONSTDC_NAMES) ||      \
    (!defined _CRT_DECLARE_NONSTDC_NAMES && !__STDC__)
typedef long off_t;
#endif

//--- io.h
#pragma once
#include <corecrt.h>
#if _CRT_INTERNAL_NONSTDC_NAMES
int open(char const *, int, ...);
#endif

//--- direct.h
//--- process.h

//--- stdlib.h
#pragma once
#include <corecrt.h>
#include <stddef.h>
#ifdef __cplusplus
#error "the C++ library owns the C++ declarations"
#endif
#ifndef __cplusplus
#define min(a, b) a
#define max(a, b) b
#endif
#if _CRT_INTERNAL_NONSTDC_NAMES
#define environ _environ
#endif
_CRT_BEGIN_C_HEADER
void *malloc(size_t);
void free(void *);
_CRT_END_C_HEADER

//--- malloc.h
#define _mm_free(a) _aligned_free(a)
#define _mm_malloc(a, b) _aligned_malloc(a, b)

//--- math.h
#include <corecrt.h>
#ifdef __cplusplus
#error "the C++ library owns the C++ declarations"
#endif
#ifndef __cplusplus
#if _CRT_INTERNAL_NONSTDC_NAMES
#define complex _complex
#define DOMAIN _DOMAIN
double j0(double);
#endif
#define _matherrl _matherr
#endif
#ifdef _USE_MATH_DEFINES
#define M_PI 3.14
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

//--- windows.h
#include <winnt.h>
#define WINAPI
#define FACILITY_VISUALCPP 109
typedef int BOOL;
typedef unsigned long DWORD;
typedef long HRESULT;
typedef const char *LPCSTR;
typedef struct HINSTANCE__ *HMODULE;
typedef long long (WINAPI *FARPROC)(void);
typedef struct { unsigned long long Function; } IMAGE_THUNK_DATA;

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
#include <stdio.h>
#include <sal.h>
#include <winnt.h>
#include <delayimp.h>

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
int default_unreachable(int i) {
  switch (i) {
  case 0:
    return 1;
    DEFAULT_UNREACHABLE;
  }
}

// stdlib.h declares aligned_alloc and posix_memalign, but in strict ISO C,
// as with glibc, aligned_alloc only from C11 on and posix_memalign only for
// POSIX, so that a program may use the names itself.
#if defined(__STRICT_ANSI__) && __STDC_VERSION__ >= 202311L
// mm_malloc.h declares posix_memalign itself.
#include <mm_malloc.h>
void *aligned(void) { return _mm_malloc(64, aligned_alloc(64, 64) ? 64 : 8); }
#elif defined(__STRICT_ANSI__) && !defined(_POSIX_C_SOURCE)
int posix_memalign;
// strict-error@+2 {{call to undeclared library function 'aligned_alloc'}}
// strict-note@+1 {{explicitly provide a declaration for 'aligned_alloc'}}
void *undeclared(void) { return aligned_alloc(16, 16); }
#else
void *(*aligned_alloc_pointer)(size_t, size_t) = aligned_alloc;
int (*posix_memalign_pointer)(void **, size_t, size_t) = posix_memalign;
#endif

// _mm_malloc allocates with posix_memalign, whose memory free releases, in
// either order with the UCRT's malloc.h, which defines it as a macro for
// _aligned_malloc, and malloc.h alone provides it.
// CHECK-LABEL: define {{.*}}@allocate(
// CHECK:       call i32 @posix_memalign(
#if !defined(__STRICT_ANSI__) || defined(_POSIX_C_SOURCE)
#ifdef __cplusplus
#include <mm_malloc.h>
#endif
#include <malloc.h>
#ifdef __cplusplus
extern "C"
#endif
void allocate(void) { _mm_free(_mm_malloc(64, 64)); }
#endif

// The UCRT's formatted I/O, which its headers define inline, is only declared,
// since the start-up library defines it, unless the unit defines
// _CRT_STDIO_INLINE.
// CHECK:  declare {{.*}}i32 @printf(
// INLINE: define {{.*}}i32 @printf(
int print(void) { return printf(""); }

// As with glibc, every header declares the UCRT's POSIX and other non-standard
// names, in sys/types.h ssize_t too, but a header that ISO C defines leaves
// them out in strict ISO C without a feature test macro, so that a program may
// use them itself, and math.h defines M_PI where it declares them. C23's
// string.h has strdup. A program's own _CRT_DECLARE_NONSTDC_NAMES decides for
// every header. wchar.h comes first, so that sys/stat.h and sys/types.h are
// first read from a header that ISO C defines.
#include <wchar.h>
#include <string.h>
#include <unistd.h>
#if _CRT_DECLARE_NONSTDC_NAMES
int (*open_pointer)(char const *, int, ...) = open;
struct stat status;
ssize_t signed_size = STDIN_FILENO - 1;
_Static_assert(sizeof(ssize_t) == sizeof(size_t) && sizeof(off_t) == 4, "");
#else
int open, off_t, ssize_t;
#endif
#if !_CRT_DECLARE_NONSTDC_NAMES ||                                             \
    (defined(__STRICT_ANSI__) && !defined(_POSIX_C_SOURCE))
int fileno;
#ifndef __cplusplus
int j0;
#endif
#if _CRT_DECLARE_NONSTDC_NAMES && __STDC_VERSION__ >= 202311L
char *(*strdup_pointer)(char const *) = strdup;
#endif
#ifdef M_PI
#error "M_PI is defined in strict ISO C"
#endif
#else
int (*fileno_pointer)(void *) = fileno;
char *(*strdup_pointer)(char const *) = strdup;
double pi = M_PI;
#endif
// glibc defines none of these, and complex would break complex.h.
#if !defined(__cplusplus) &&                                                   \
    (defined(min) || defined(max) || defined(environ) || defined(complex) ||   \
     defined(DOMAIN) || defined(_USE_MATH_DEFINES))
#error "the UCRT's macros and _USE_MATH_DEFINES stay as the program left them"
#endif

// delayimp.h has Visual C++'s layout, and a program's hooks and its choice of
// section-wide protection are constant unless it asks for writable ones.
_Static_assert(sizeof(ImgDelayDescr) == 32, "");
_Static_assert(sizeof(DelayLoadInfo) == 72, "");
_Static_assert(__builtin_offsetof(DelayLoadInfo, dlp.szProcName) == 40, "");
_Static_assert(dliFailGetProc == 4 && dliNoteEndProcessing == 5, "");
_Static_assert(VcppException(0xC0000000, 127) == 0xC06D007F, "");
static FARPROC WINAPI hook(unsigned Stage, PDelayLoadInfo Info) {
  return Stage == dliFailGetProc ? Info->pfnCur : 0;
}
#ifndef DELAYIMP_INSECURE_WRITABLE_HOOKS
const
#endif
PfnDliHook __pfnDliNotifyHook2 = hook, __pfnDliFailureHook2 = hook;
#ifndef DELAYIMP_WRITABLE_DLOAD_SECTION_PROTECTION_POLICY
const
#endif
BOOL __bChangeProtectionOfWholeDloadSection = 1;
BOOL unload(void) { return __FUnloadDelayLoadedDLL2("a.dll"); }
HRESULT load_all(void) { return __HrLoadAllImportsForDll("a.dll"); }

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
