/*===---- corecrt.h - UCRT corecrt.h wrapper -------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_CORECRT_H
#define __CLANG_CORECRT_H

/* The UCRT declares its POSIX and other non-standard names, such as fileno,
 * open and off_t, only where __STDC__ is not true, which it always is for this
 * target. The wrappers of the UCRT's headers define __STDC__ around the UCRT
 * header to choose, and choose as glibc does: a header that ISO C defines
 * declares the names unless the mode is strict ISO C without a feature test
 * macro that asks for them, and any other header always declares them. A
 * program's own _CRT_DECLARE_NONSTDC_NAMES decides for every header, as it
 * does with the Microsoft compiler. glibc does not deprecate these names, so
 * neither does the UCRT unless the program configures its deprecation. */
#if defined(_CRT_DECLARE_NONSTDC_NAMES)
#define __CLANG_UCRT_NONSTDC_NAMES _CRT_DECLARE_NONSTDC_NAMES
#define __CLANG_UCRT_POSIX_HEADER_NAMES _CRT_DECLARE_NONSTDC_NAMES
#else
#if defined(__cplusplus) || !defined(__STRICT_ANSI__) || !defined(__STDC__) || \
    defined(_POSIX_SOURCE) || defined(_POSIX_C_SOURCE) ||                      \
    defined(_XOPEN_SOURCE) || defined(_DEFAULT_SOURCE) || defined(_GNU_SOURCE)
#define __CLANG_UCRT_NONSTDC_NAMES 1
#else
#define __CLANG_UCRT_NONSTDC_NAMES 0
#endif
#define __CLANG_UCRT_POSIX_HEADER_NAMES 1
#endif

#if !defined(_CRT_NONSTDC_DEPRECATE) && !defined(_CRT_NONSTDC_NO_WARNINGS) &&  \
    !defined(_CRT_NONSTDC_NO_DEPRECATE)
#define _CRT_NONSTDC_NO_DEPRECATE
#endif

#include_next <corecrt.h>

/* Recent UCRTs decide once, in corecrt.h, and test the result instead of
 * __STDC__. Decide again wherever a wrapper chooses, and elsewhere as an ISO C
 * header does. */
#undef _CRT_INTERNAL_NONSTDC_NAMES
#define _CRT_INTERNAL_NONSTDC_NAMES (!__STDC__ || __CLANG_UCRT_NONSTDC_NAMES)

#endif /* __CLANG_CORECRT_H */
