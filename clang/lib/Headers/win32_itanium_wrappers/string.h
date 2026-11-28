/*===---- string.h - UCRT string.h wrapper ---------------------------------===
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
#include_next <string.h>
#pragma pop_macro("__CLANG_UCRT_NONSTDC_NAMES")

/* C23 adds these to string.h. The UCRT declares them only with its other
 * non-standard names. */
#if !__CLANG_UCRT_ISO_C_NAMES && defined(__STDC_VERSION__) &&                  \
    __STDC_VERSION__ >= 202311L
_CRT_BEGIN_C_HEADER
_ACRTIMP void *__cdecl memccpy(void *_Dst, void const *_Src, int _Val,
                               size_t _Size);
_ACRTIMP char *__cdecl strdup(char const *_String);
_CRT_END_C_HEADER
#endif
