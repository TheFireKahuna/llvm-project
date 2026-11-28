/*===---- stdlib.h - UCRT stdlib.h wrapper ---------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_STDLIB_H
#define __CLANG_STDLIB_H

/* A header that ISO C defines, which leaves the UCRT's POSIX and other
 * non-standard names out in strict ISO C; see corecrt.h. glibc's stdlib.h
 * defines none of the UCRT's min, max, environ, sys_errlist and sys_nerr
 * macros, so these keep the caller's definitions. */
#pragma push_macro("min")
#pragma push_macro("max")
#pragma push_macro("environ")
#pragma push_macro("sys_errlist")
#pragma push_macro("sys_nerr")
#include <corecrt.h>
#pragma push_macro("__CLANG_UCRT_NONSTDC_NAMES")
#undef __CLANG_UCRT_NONSTDC_NAMES
#define __CLANG_UCRT_NONSTDC_NAMES __CLANG_UCRT_ISO_C_NAMES
#ifdef __cplusplus
/* The C++ library owns every C++ declaration of the C library's names, and
 * the UCRT's stdlib.h declares abs and div overloads for C++ that collide with
 * its own. Include the UCRT header as a C header, as math.h does. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__cplusplus")
#undef __cplusplus
#pragma push_macro("__STDC_VERSION__")
#define __STDC_VERSION__ 201710L
#include_next <stdlib.h>
#pragma pop_macro("__STDC_VERSION__")
#pragma pop_macro("__cplusplus")
#pragma clang diagnostic pop
#else
#include_next <stdlib.h>
#endif
#pragma pop_macro("__CLANG_UCRT_NONSTDC_NAMES")
#pragma pop_macro("sys_nerr")
#pragma pop_macro("sys_errlist")
#pragma pop_macro("environ")
#pragma pop_macro("max")
#pragma pop_macro("min")

/* C11's aligned_alloc and POSIX's posix_memalign, which the UCRT neither
 * declares nor exports and the start-up library provides. Their memory is
 * released with free. As with glibc, a strict ISO C mode sees aligned_alloc
 * only from C11 on and posix_memalign only for POSIX, so that a program may
 * use the names itself. */
#ifdef __cplusplus
extern "C" {
#endif
#if defined(__cplusplus) || !defined(__STRICT_ANSI__) ||                       \
    (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) ||              \
    defined(_ISOC11_SOURCE) || defined(_DEFAULT_SOURCE) ||                     \
    defined(_GNU_SOURCE)
void *__cdecl aligned_alloc(size_t _Alignment, size_t _Size);
#endif
#if defined(__cplusplus) || !defined(__STRICT_ANSI__) ||                       \
    (defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L) ||                \
    (defined(_XOPEN_SOURCE) && _XOPEN_SOURCE >= 600) ||                        \
    defined(_DEFAULT_SOURCE) || defined(_GNU_SOURCE)
int __cdecl posix_memalign(void **_Memory, size_t _Alignment, size_t _Size);
#endif
#ifdef __cplusplus
}
#endif

#endif /* __CLANG_STDLIB_H */
