/*===---- malloc.h - UCRT malloc.h wrapper ---------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_MALLOC_H
#define __CLANG_MALLOC_H

#ifndef __cplusplus
/* The UCRT defines the _malloca helpers and _freea __inline without extern.
 * Outside the Microsoft C++ ABI such a C definition is only an inline
 * definition, so a call that is not inlined would refer to a function that no
 * library defines. Give them internal linkage instead. */
#include <corecrt.h>
#pragma push_macro("__inline")
#define __inline static __inline
#include_next <malloc.h>
#pragma pop_macro("__inline")
#else
#include_next <malloc.h>
#endif

#endif /* __CLANG_MALLOC_H */
