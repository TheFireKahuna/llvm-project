/*===---- memory.h - Memory functions wrapper ------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_MEMORY_H
#define __CLANG_MEMORY_H

#if __STDC_HOSTED__ && __has_include_next(<memory.h>)
#include_next <memory.h>
#endif

/*
 * Windows Itanium: Map POSIX function names to UCRT underscore-prefixed names.
 *
 * The UCRT only exposes memccpy/memicmp when !__STDC__, but we want __STDC__
 * for standards compliance. Provide the mappings when targeting MSVCRT/UCRT.
 */
#if defined(__MSVCRT__)
#  ifndef memccpy
#    define memccpy _memccpy
#  endif
#  ifndef memicmp
#    define memicmp _memicmp
#  endif
#endif /* __MSVCRT__ */

#endif /* __CLANG_MEMORY_H */
