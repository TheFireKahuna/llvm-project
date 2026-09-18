/*===---- math.h - Math functions wrapper ----------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_MATH_H
#define __CLANG_MATH_H

#if __STDC_HOSTED__ && __has_include_next(<math.h>)
#if defined(__cplusplus) && defined(LLVM_CRT_UCRT) && !defined(_MSC_VER)
/*
 * Present the UCRT as a plain C library: libc++ owns every C++-visible
 * declaration. UCRT's __cplusplus block in corecrt_math.h declares
 * fpclassify/signbit overloads and isfinite/isnan/... function templates
 * that make every call ambiguous against libc++'s definitions, so include
 * the header with __cplusplus hidden. The C face defines those names as
 * macros instead, which libc++'s <math.h> removes before declaring its own.
 * corecrt.h is included first, while __cplusplus is still visible, so
 * _CRT_BEGIN_C_HEADER is permanently pinned to its extern "C" form and every
 * UCRT declaration keeps C linkage.
 *
 * While __cplusplus is hidden the TU must still identify as some language:
 * headers reached from here run version-gated content once, under their
 * include guard, and would otherwise lock in the no-dialect subset for the
 * whole TU. Claim C17 so they take their modern-C paths, whose results match
 * the C++ ones.
 */
#include <corecrt.h>
/* UCRT assumes every C++ dialect supports noexcept. Keep its non-throwing
 * declarations valid when libc++ uses the frozen C++03 headers. */
#if __cplusplus < 201103L
#undef _CRT_NOEXCEPT
#define _CRT_NOEXCEPT throw()
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__cplusplus")
#undef __cplusplus
#pragma push_macro("__STDC_VERSION__")
#define __STDC_VERSION__ 201710L
#include_next <math.h>
#pragma pop_macro("__STDC_VERSION__")
#pragma pop_macro("__cplusplus")
#pragma clang diagnostic pop
#else
#include_next <math.h>
#endif
#endif

/*
 * Windows Itanium: Map POSIX function names to UCRT underscore-prefixed names.
 *
 * The UCRT only exposes Bessel functions (j0/j1/jn/y0/y1/yn) when !__STDC__,
 * but we want __STDC__ for standards compliance. Provide the mappings when
 * targeting MSVCRT/UCRT.
 */
#if defined(__MSVCRT__)
/* Bessel functions of the first kind */
#  ifndef j0
#    define j0 _j0
#  endif
#  ifndef j1
#    define j1 _j1
#  endif
#  ifndef jn
#    define jn _jn
#  endif
/* Bessel functions of the second kind */
#  ifndef y0
#    define y0 _y0
#  endif
#  ifndef y1
#    define y1 _y1
#  endif
#  ifndef yn
#    define yn _yn
#  endif
#endif /* __MSVCRT__ */

#endif /* __CLANG_MATH_H */
