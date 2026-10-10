/*===---- memory.h - UCRT memory.h wrapper ---------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_MEMORY_H
#define __CLANG_MEMORY_H

/* Not an ISO C header, so the UCRT's POSIX and other non-standard names are
 * declared in every mode; see corecrt.h. */
#include <corecrt.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__STDC__")
#undef __STDC__
#define __STDC__ (!__CLANG_UCRT_POSIX_HEADER_NAMES)
#include_next <memory.h>
#pragma pop_macro("__STDC__")
#pragma clang diagnostic pop

/* string.h, in a mode that hides these names, may have included the UCRT
 * header that declares them first. */
#if !__CLANG_UCRT_NONSTDC_NAMES && __CLANG_UCRT_POSIX_HEADER_NAMES
_CRT_BEGIN_C_HEADER
_ACRTIMP void *__cdecl memccpy(void *_Dst, void const *_Src, int _Val,
                               size_t _Size);
_ACRTIMP int __cdecl memicmp(void const *_Buf1, void const *_Buf2,
                             size_t _Size);
_CRT_END_C_HEADER
#endif

#endif /* __CLANG_MEMORY_H */
