/*===---- math.h - UCRT math.h wrapper -------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_MATH_H
#define __CLANG_MATH_H

/* The UCRT's POSIX and other non-standard names are declared unless the mode
 * is strict ISO C; see corecrt.h. glibc's math.h defines none of the UCRT's
 * complex, matherr and error type macros, and complex would break complex.h,
 * so these stay undefined. */
#pragma push_macro("complex")
#pragma push_macro("matherr")
#pragma push_macro("DOMAIN")
#pragma push_macro("SING")
#pragma push_macro("OVERFLOW")
#pragma push_macro("UNDERFLOW")
#pragma push_macro("TLOSS")
#pragma push_macro("PLOSS")
#include <corecrt.h>
/* glibc's math.h defines M_PI and the other constants in the same modes, and
 * the UCRT's defines them for _USE_MATH_DEFINES. */
#pragma push_macro("_USE_MATH_DEFINES")
#if __CLANG_UCRT_NONSTDC_NAMES && !defined(_USE_MATH_DEFINES)
#define _USE_MATH_DEFINES
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__STDC__")
#undef __STDC__
#define __STDC__ (!__CLANG_UCRT_NONSTDC_NAMES)
#ifdef __cplusplus
/* The C++ library owns every C++ declaration of the C library's names, and
 * the UCRT's math.h declares fpclassify and signbit overloads and isfinite,
 * isnan and similar templates for C++ that make calls ambiguous against its
 * own. Include the UCRT header as a C header, which defines those names as
 * macros that the C++ library's math.h replaces: with __cplusplus hidden, and
 * after corecrt.h, so that _CRT_BEGIN_C_HEADER keeps its extern "C" form.
 * Headers first reached from inside see C17, the C dialect whose macros match
 * C++'s, rather than no dialect at all. */
#pragma push_macro("__cplusplus")
#undef __cplusplus
#pragma push_macro("__STDC_VERSION__")
#define __STDC_VERSION__ 201710L
#include_next <math.h>
#pragma pop_macro("__STDC_VERSION__")
#pragma pop_macro("__cplusplus")
#else
/* The UCRT defines float and long double forms of some functions __inline
 * without extern, as wrappers of the double ones. Outside the Microsoft C++
 * ABI such a C definition is only an inline definition, so a call that is not
 * inlined would refer to a function that no library defines. Give them
 * internal linkage instead. */
#pragma push_macro("__inline")
#define __inline static __inline
#include_next <math.h>
#pragma pop_macro("__inline")
#endif
#pragma pop_macro("__STDC__")
#pragma clang diagnostic pop
#pragma pop_macro("_USE_MATH_DEFINES")
#pragma pop_macro("PLOSS")
#pragma pop_macro("TLOSS")
#pragma pop_macro("UNDERFLOW")
#pragma pop_macro("OVERFLOW")
#pragma pop_macro("SING")
#pragma pop_macro("DOMAIN")
#pragma pop_macro("matherr")
#pragma pop_macro("complex")

#endif /* __CLANG_MATH_H */
