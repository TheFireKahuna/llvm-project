/*===---- string.h - UCRT string.h wrapper ---------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_STRING_H
#define __CLANG_STRING_H

/* An ISO C header: the UCRT's POSIX and other non-standard names are declared
 * unless the mode is strict ISO C; see corecrt.h. */
#include <corecrt.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__STDC__")
#undef __STDC__
#define __STDC__ (!__CLANG_UCRT_NONSTDC_NAMES)
#include_next <string.h>
#pragma pop_macro("__STDC__")
#pragma clang diagnostic pop

/* C23 adds these to string.h. The UCRT declares them only with its other
 * non-standard names. */
#if !__CLANG_UCRT_NONSTDC_NAMES && defined(__STDC_VERSION__) &&                \
    __STDC_VERSION__ >= 202311L
_CRT_BEGIN_C_HEADER
_ACRTIMP void *__cdecl memccpy(void *_Dst, void const *_Src, int _Val,
                               size_t _Size);
_ACRTIMP char *__cdecl strdup(char const *_String);
_CRT_END_C_HEADER
#endif

#endif /* __CLANG_STRING_H */
