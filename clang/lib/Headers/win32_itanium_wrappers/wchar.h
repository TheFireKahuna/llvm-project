/*===---- wchar.h - UCRT wchar.h wrapper ------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_WCHAR_H
#define __CLANG_WCHAR_H

/* The UCRT's POSIX and other non-standard names are declared unless the mode
 * is strict ISO C; see corecrt.h. The UCRT's wchar.h also includes sys/stat.h,
 * sys/types.h and corecrt_share.h, whose names are declared in every mode. */
#include <corecrt.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__STDC__")
#undef __STDC__
#define __STDC__ (!__CLANG_UCRT_NONSTDC_NAMES)
#include_next <wchar.h>
#pragma pop_macro("__STDC__")
#pragma clang diagnostic pop

#endif /* __CLANG_WCHAR_H */
