/*===---- ctype.h - UCRT ctype.h wrapper -----------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* A header that ISO C defines, which leaves the UCRT's POSIX and other
 * non-standard names out in strict ISO C; see corecrt.h. */
#include <corecrt.h>
#pragma push_macro("__CLANG_UCRT_NONSTDC_NAMES")
#undef __CLANG_UCRT_NONSTDC_NAMES
#define __CLANG_UCRT_NONSTDC_NAMES __CLANG_UCRT_ISO_C_NAMES
#include_next <ctype.h>
#pragma pop_macro("__CLANG_UCRT_NONSTDC_NAMES")
