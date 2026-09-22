/*===---- search.h - Search functions wrapper ------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_SEARCH_H
#define __CLANG_SEARCH_H

#if __STDC_HOSTED__ && __has_include_next(<search.h>)
#include_next <search.h>
#endif

/*
 * Windows Itanium: Map POSIX function names to UCRT underscore-prefixed names.
 *
 * The UCRT only exposes lfind/lsearch when !__STDC__, but we want __STDC__
 * for standards compliance. Provide the mappings when targeting MSVCRT/UCRT.
 */
#if defined(__MSVCRT__) || defined(_UCRT)
#  ifndef lfind
#    define lfind _lfind
#  endif
#  ifndef lsearch
#    define lsearch _lsearch
#  endif
#endif /* __MSVCRT__ || _UCRT */

#endif /* __CLANG_SEARCH_H */
