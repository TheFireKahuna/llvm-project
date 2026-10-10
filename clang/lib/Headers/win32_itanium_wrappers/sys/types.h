/*===---- sys/types.h - UCRT sys/types.h wrapper ---------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_SYS_TYPES_H
#define __CLANG_SYS_TYPES_H

/* Not an ISO C header, so the UCRT's POSIX and other non-standard names are
 * declared in every mode; see corecrt.h. */
#include <corecrt.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__STDC__")
#undef __STDC__
#define __STDC__ (!__CLANG_UCRT_POSIX_HEADER_NAMES)
#include_next <sys/types.h>
#pragma pop_macro("__STDC__")
#pragma clang diagnostic pop

/* The UCRT's off_t has no ssize_t beside it. _SSIZE_T_DEFINED is MinGW-w64's
 * name for the typedef, which portable code tests before defining its own. */
#if __CLANG_UCRT_POSIX_HEADER_NAMES && !defined(_SSIZE_T_DEFINED)
#define _SSIZE_T_DEFINED
typedef __PTRDIFF_TYPE__ ssize_t;
#endif

#endif /* __CLANG_SYS_TYPES_H */
