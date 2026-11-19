/*===---- vcruntime.h - Definitions the UCRT headers expect ----------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* Windows Itanium uses the UCRT without the Visual C++ runtime. Every UCRT
 * header includes vcruntime.h first, through corecrt.h, for the types and
 * macros it shares with that runtime; this header provides them. */

#ifndef __CLANG_VCRUNTIME_H
#define __CLANG_VCRUNTIME_H

#include <sal.h>
#include <vadefs.h>

#define __need_size_t
#define __need_ptrdiff_t
#define __need_wchar_t
#include <stddef.h>
#define _SIZE_T_DEFINED
#define _PTRDIFF_T_DEFINED

#ifndef _INTPTR_T_DEFINED
#define _INTPTR_T_DEFINED
typedef __INTPTR_TYPE__ intptr_t;
#endif

/* The UCRT declares its interface between these, with Visual C++'s structure
 * packing and C linkage. */
#ifdef __cplusplus
#define _CRT_BEGIN_C_HEADER _Pragma("pack(push, _CRT_PACKING)") extern "C" {
#define _CRT_END_C_HEADER                                                      \
  }                                                                            \
  _Pragma("pack(pop)")
#else
#define _CRT_BEGIN_C_HEADER _Pragma("pack(push, _CRT_PACKING)")
#define _CRT_END_C_HEADER _Pragma("pack(pop)")
#endif

#define _CRT_STRINGIZE_(x) #x
#define _CRT_STRINGIZE(x) _CRT_STRINGIZE_(x)
#define _CRT_WIDE_(s) L##s
#define _CRT_WIDE(s) _CRT_WIDE_(s)

/* The UCRT's stdlib.h defines _countof as this. In C++ it accepts only
 * arrays, as Visual C++'s does. */
#ifdef __cplusplus
extern "C++" {
template <typename _CountofType, size_t _SizeOfArray>
char (*__countof_helper(_CountofType (&_Array)[_SizeOfArray]))[_SizeOfArray];
}
#define __crt_countof(_Array) (sizeof(*__countof_helper(_Array)) + 0)
#else
#define __crt_countof(_Array) (sizeof(_Array) / sizeof(_Array[0]))
#endif

/* The UCRT's DLL exports the functions that the Visual C++ runtime's would, so
 * under _DLL they are imported, as the UCRT's own are. */
#ifndef _VCRTIMP
#ifdef _DLL
#define _VCRTIMP __declspec(dllimport)
#else
#define _VCRTIMP
#endif
#endif
#define __CRTDECL __cdecl

/* The UCRT's static inline functions cannot be exported from C++ modules. */
#if defined(__cplusplus) && !defined(_STATIC_INLINE_UCRT_FUNCTIONS)
#define _STATIC_INLINE_UCRT_FUNCTIONS 0
#endif

/* The UCRT assumes that every C++ dialect has noexcept. */
#if defined(__cplusplus) && __cplusplus < 201103L && !defined(_CRT_NOEXCEPT)
#define _CRT_NOEXCEPT throw()
#endif

#define _CRT_DEPRECATE_TEXT(_Text) __declspec(deprecated(_Text))

/* The ISO C functions that the UCRT marks as insecure are not deprecated. */
#define _CRT_INSECURE_DEPRECATE(_Replacement)
#define _CRT_INSECURE_DEPRECATE_MEMORY(_Replacement)

#endif /* __CLANG_VCRUNTIME_H */
