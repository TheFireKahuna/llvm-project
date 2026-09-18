/* ===-------- vadefs.h ---------------------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __clang_vadefs_h
#define __clang_vadefs_h

/* Only chain to a real <vadefs.h> for plain clang-cl / MSVC-compat builds.
 * _WIN32_ITANIUM and __NTPOSIX__ ship no Visual Studio headers, so there is
 * nothing to chain to — they use the self-contained definitions below. */
#if !defined(_MSC_VER) && !defined(_WIN32_ITANIUM) && !defined(__NTPOSIX__)
#include_next <vadefs.h>
#else

#ifndef _VA_LIST
#define _VA_LIST
typedef __builtin_va_list va_list;
#endif

#define _CRT_PACKING 8
#pragma pack(push, _CRT_PACKING)

#if !defined _W64
#define _W64
#endif

#ifndef _UINTPTR_T_DEFINED
    #define _UINTPTR_T_DEFINED
    #ifdef _WIN64
        typedef unsigned long long uintptr_t;
    #else
        typedef unsigned int uintptr_t;
    #endif
#endif

#if defined(_WIN32_ITANIUM) || defined(__NTPOSIX__)
/* Define CRT symbols unconditionally for the no-VS-headers targets. */
#undef __crt_va_start
#define __crt_va_start(ap, param) __builtin_va_start(ap, param)
#undef __crt_va_end
#define __crt_va_end(ap)          __builtin_va_end(ap)
#undef __crt_va_arg
#define __crt_va_arg(ap, type)    __builtin_va_arg(ap, type)
#else
/* Override macros from vadefs.h with definitions that work with Clang. */
#ifdef _crt_va_start
#undef _crt_va_start
#define _crt_va_start(ap, param) __builtin_va_start(ap, param)
#endif
#ifdef _crt_va_end
#undef _crt_va_end
#define _crt_va_end(ap)          __builtin_va_end(ap)
#endif
#ifdef _crt_va_arg
#undef _crt_va_arg
#define _crt_va_arg(ap, type)    __builtin_va_arg(ap, type)
#endif
#endif

#pragma pack(pop)

#endif
#endif
