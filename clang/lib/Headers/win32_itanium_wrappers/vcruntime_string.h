/*===---- vcruntime_string.h - Memory and string scanning functions --------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* The UCRT's string.h and wchar.h leave these declarations to the Visual C++
 * runtime's header. On Windows Itanium the UCRT itself provides the functions.
 */

#ifndef __CLANG_VCRUNTIME_STRING_H
#define __CLANG_VCRUNTIME_STRING_H

#include <corecrt.h>

_CRT_BEGIN_C_HEADER

_VCRTIMP void _CONST_RETURN *__cdecl memchr(void const *_Buf, int _Val,
                                            size_t _MaxCount);
int __cdecl memcmp(void const *_Buf1, void const *_Buf2, size_t _Size);
void *__cdecl memcpy(void *_Dst, void const *_Src, size_t _Size);
_VCRTIMP void *__cdecl memmove(void *_Dst, void const *_Src, size_t _Size);
void *__cdecl memset(void *_Dst, int _Val, size_t _Size);

_VCRTIMP char _CONST_RETURN *__cdecl strchr(char const *_Str, int _Val);
_VCRTIMP char _CONST_RETURN *__cdecl strrchr(char const *_Str, int _Ch);
_VCRTIMP char _CONST_RETURN *__cdecl strstr(char const *_Str,
                                            char const *_SubStr);

_VCRTIMP wchar_t _CONST_RETURN *__cdecl wcschr(wchar_t const *_Str,
                                               wchar_t _Ch);
_VCRTIMP wchar_t _CONST_RETURN *__cdecl wcsrchr(wchar_t const *_Str,
                                                wchar_t _Ch);
_VCRTIMP wchar_t _CONST_RETURN *__cdecl wcsstr(wchar_t const *_Str,
                                               wchar_t const *_SubStr);

_CRT_END_C_HEADER

#endif /* __CLANG_VCRUNTIME_STRING_H */
