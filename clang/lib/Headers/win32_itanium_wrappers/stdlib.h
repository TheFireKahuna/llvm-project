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

#ifdef __cplusplus
/* The C++ library owns every C++ declaration of the C library's names, and
 * the UCRT's stdlib.h declares abs and div overloads for C++ that collide with
 * its own. Include the UCRT header as a C header: with __cplusplus hidden, and
 * after corecrt.h, so that _CRT_BEGIN_C_HEADER keeps its extern "C" form.
 * Headers first reached from inside see C17, the C dialect whose macros match
 * C++'s, rather than no dialect at all. Hiding __cplusplus can define min and
 * max, which are not C++ names. */
#pragma push_macro("min")
#pragma push_macro("max")
#include <corecrt.h>
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
#pragma pop_macro("max")
#pragma pop_macro("min")
#else
#include_next <stdlib.h>
#endif

#endif /* __CLANG_STDLIB_H */
