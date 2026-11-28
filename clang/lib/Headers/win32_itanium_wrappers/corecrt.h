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
 * open, strdup and off_t, only where __STDC__ is not true, which with clang
 * it always is, or where the program defines _CRT_DECLARE_NONSTDC_NAMES as
 * true. Choose as glibc does: every header declares the names, except that
 * the headers ISO C defines leave them out in strict ISO C without a feature
 * test macro that asks for them, so that a program may use them itself. The
 * UCRT's headers read the macro where they declare the names, so the wrappers
 * of the ISO C headers set what it expands to around the UCRT's header. A
 * program's own definition decides for every header, as with Microsoft's
 * compiler. */
#ifdef _CRT_DECLARE_NONSTDC_NAMES
#define __CLANG_UCRT_ISO_C_NAMES _CRT_DECLARE_NONSTDC_NAMES
#else
#define _CRT_DECLARE_NONSTDC_NAMES __CLANG_UCRT_NONSTDC_NAMES
#define __CLANG_UCRT_NONSTDC_NAMES 1
#if defined(__cplusplus) || !defined(__STRICT_ANSI__) ||                       \
    defined(_POSIX_SOURCE) || defined(_POSIX_C_SOURCE) ||                      \
    defined(_XOPEN_SOURCE) || defined(_DEFAULT_SOURCE) || defined(_GNU_SOURCE)
#define __CLANG_UCRT_ISO_C_NAMES 1
#else
#define __CLANG_UCRT_ISO_C_NAMES 0
#endif
#endif

/* glibc does not deprecate these names, so neither does the UCRT unless the
 * program configures its deprecation. */
#if !defined(_CRT_NONSTDC_DEPRECATE) && !defined(_CRT_NONSTDC_NO_WARNINGS) &&  \
    !defined(_CRT_NONSTDC_NO_DEPRECATE)
#define _CRT_NONSTDC_NO_WARNINGS
#endif

#include_next <corecrt.h>

/* corecrt.h has decided once, for the headers that test its result. They
 * decide where they test it instead. */
#undef _CRT_INTERNAL_NONSTDC_NAMES
#define _CRT_INTERNAL_NONSTDC_NAMES _CRT_DECLARE_NONSTDC_NAMES

#endif /* __CLANG_CORECRT_H */
