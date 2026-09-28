/*===---- tchar.h - UCRT tchar.h wrapper ------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_TCHAR_H
#define __CLANG_TCHAR_H

#ifndef __cplusplus
/* Unless __STDC__ is defined, as it is not under -fms-compatibility, the UCRT
 * defines the generic-text functions __inline without extern, and does not
 * export them. Outside the Microsoft C++ ABI such a C definition is only an
 * inline definition, so a call that is not inlined would refer to a function
 * that no library defines. Give them internal linkage instead. wchar.h, which
 * tchar.h includes for _UNICODE, comes first, since its wrapper includes
 * headers that must keep their own __inline. */
#include <corecrt.h>
#ifdef _UNICODE
#include <wchar.h>
#endif
#pragma push_macro("__inline")
#define __inline static __inline
#include_next <tchar.h>
#pragma pop_macro("__inline")
#else
#include_next <tchar.h>
#endif

#endif /* __CLANG_TCHAR_H */
