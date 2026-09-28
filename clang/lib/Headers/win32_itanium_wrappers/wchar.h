/*===---- wchar.h - UCRT wchar.h wrapper ------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_WCHAR_H
#define __CLANG_WCHAR_H

#ifndef __cplusplus
/* The UCRT defines fwide, mbsinit and the wmem functions __inline without
 * extern, and does not export them. Outside the Microsoft C++ ABI such a C
 * definition is only an inline definition, so a call that is not inlined would
 * refer to a function that no library defines. Give them internal linkage
 * instead. The intrinsic headers wchar.h includes come first, since their
 * definitions are static already. */
#include <corecrt.h>
#include <intrin.h>
#if defined(_M_ARM64) || defined(_M_ARM64EC)
#include <arm_neon.h>
#endif
#pragma push_macro("__inline")
#define __inline static __inline
#include_next <wchar.h>
#pragma pop_macro("__inline")
#else
#include_next <wchar.h>
#endif

#endif /* __CLANG_WCHAR_H */
