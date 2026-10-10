/*===---- search.h - UCRT search.h wrapper ---------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_SEARCH_H
#define __CLANG_SEARCH_H

/* Not an ISO C header, so the UCRT's POSIX and other non-standard names are
 * declared in every mode; see corecrt.h. */
#include <corecrt.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__STDC__")
#undef __STDC__
#define __STDC__ (!__CLANG_UCRT_POSIX_HEADER_NAMES)
#include_next <search.h>
#pragma pop_macro("__STDC__")
#pragma clang diagnostic pop

/* stdlib.h, in a mode that hides these names, may have included the UCRT
 * header that declares them first. */
#if !__CLANG_UCRT_NONSTDC_NAMES && __CLANG_UCRT_POSIX_HEADER_NAMES
_CRT_BEGIN_C_HEADER
_ACRTIMP void *__cdecl lfind(void const *_Key, void const *_Base,
                             unsigned int *_NumOfElements,
                             unsigned int _SizeOfElements,
                             int(__cdecl *_PtFuncCompare)(void const *,
                                                          void const *));
_ACRTIMP void *__cdecl lsearch(void const *_Key, void *_Base,
                               unsigned int *_NumOfElements,
                               unsigned int _SizeOfElements,
                               int(__cdecl *_PtFuncCompare)(void const *,
                                                            void const *));
_CRT_END_C_HEADER
#endif

#endif /* __CLANG_SEARCH_H */
