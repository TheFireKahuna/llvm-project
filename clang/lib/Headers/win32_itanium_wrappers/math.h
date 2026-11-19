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

#ifdef __cplusplus
/* The C++ library owns every C++ declaration of the C library's names, and
 * the UCRT's math.h declares fpclassify and signbit overloads and isfinite,
 * isnan and similar templates for C++ that make calls ambiguous against its
 * own. Include the UCRT header as a C header, which defines those names as
 * macros that the C++ library's math.h replaces: with __cplusplus hidden, and
 * after corecrt.h, so that _CRT_BEGIN_C_HEADER keeps its extern "C" form.
 * Headers first reached from inside see C17, the C dialect whose macros match
 * C++'s, rather than no dialect at all. The C view defines complex and
 * _matherrl, which are not C++ names, so these keep the caller's definitions. */
#pragma push_macro("complex")
#pragma push_macro("_matherrl")
#include <corecrt.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__cplusplus")
#undef __cplusplus
#pragma push_macro("__STDC_VERSION__")
#define __STDC_VERSION__ 201710L
#include_next <math.h>
#pragma pop_macro("__STDC_VERSION__")
#pragma pop_macro("__cplusplus")
#pragma clang diagnostic pop
#pragma pop_macro("_matherrl")
#pragma pop_macro("complex")
#else
#include_next <math.h>
#endif

#endif /* __CLANG_MATH_H */
