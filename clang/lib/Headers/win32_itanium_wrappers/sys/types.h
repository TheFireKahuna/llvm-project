/*===---- sys/types.h - UCRT sys/types.h wrapper ---------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* Not a header that ISO C defines, so it declares the UCRT's POSIX and other
 * non-standard names in every mode, also where a header that ISO C defines is
 * the first to include it; see corecrt.h. */
#include <corecrt.h>
#pragma push_macro("__CLANG_UCRT_NONSTDC_NAMES")
#undef __CLANG_UCRT_NONSTDC_NAMES
#define __CLANG_UCRT_NONSTDC_NAMES 1
#include_next <sys/types.h>

/* The UCRT's off_t has no ssize_t beside it. _SSIZE_T_DEFINED is MinGW-w64's
 * name for the typedef, which portable code tests before defining its own. */
#if _CRT_DECLARE_NONSTDC_NAMES && !defined(_SSIZE_T_DEFINED)
#define _SSIZE_T_DEFINED
typedef __PTRDIFF_TYPE__ ssize_t;
#endif
#pragma pop_macro("__CLANG_UCRT_NONSTDC_NAMES")
