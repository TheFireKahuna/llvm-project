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
#include_next <math.h>
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
