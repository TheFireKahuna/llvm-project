/* ===-------- vadefs.h ---------------------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* Only include this if we are aiming for MSVC compatibility. */
#if !defined(_MSC_VER) && !defined(_WIN32_ITANIUM)
#include_next <vadefs.h>
#else

#ifndef __clang_vadefs_h
#define __clang_vadefs_h

#ifdef _WIN32_ITANIUM
/* Windows Itanium has no Visual C++ headers. Provide what the Windows SDK and
 * the UCRT use from vadefs.h. */
#define _CRT_PACKING 8

#ifndef _UINTPTR_T_DEFINED
#define _UINTPTR_T_DEFINED
typedef __UINTPTR_TYPE__ uintptr_t;
#endif

#ifndef _VA_LIST
#define _VA_LIST
typedef __builtin_va_list va_list;
#endif

#define __crt_va_start(ap, param) __builtin_va_start(ap, param)
#define __crt_va_end(ap) __builtin_va_end(ap)
#define __crt_va_arg(ap, type) __builtin_va_arg(ap, type)
#else
#include_next <vadefs.h>
#endif

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

/* VS 2015 switched to double underscore names, which is an improvement, but now
 * we have to intercept those names too.
 */
#ifdef __crt_va_start
#undef __crt_va_start
#define __crt_va_start(ap, param) __builtin_va_start(ap, param)
#endif
#ifdef __crt_va_end
#undef __crt_va_end
#define __crt_va_end(ap)          __builtin_va_end(ap)
#endif
#ifdef __crt_va_arg
#undef __crt_va_arg
#define __crt_va_arg(ap, type)    __builtin_va_arg(ap, type)
#endif

#endif
#endif
