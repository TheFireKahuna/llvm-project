/*===---- sys/stat.h - UCRT sys/stat.h wrapper -----------------------------===
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
#include_next <sys/stat.h>
#pragma pop_macro("__CLANG_UCRT_NONSTDC_NAMES")
