/*===---- string.h - String functions wrapper ------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_STRING_H
#define __CLANG_STRING_H

#if __STDC_HOSTED__ && __has_include_next(<string.h>)
#include_next <string.h>
#endif

/*
 * Windows Itanium: Map POSIX function names to UCRT underscore-prefixed names.
 *
 * The UCRT only exposes strdup/stricmp/strnicmp etc. when !__STDC__, but we
 * want __STDC__ for standards compliance. Provide the mappings when
 * targeting MSVCRT/UCRT.
 */
#if defined(__MSVCRT__)
/* String duplication and comparison */
#  ifndef strdup
#    define strdup _strdup
#  endif
#  ifndef strcmpi
#    define strcmpi _strcmpi
#  endif
#  ifndef stricmp
#    define stricmp _stricmp
#  endif
#  ifndef strnicmp
#    define strnicmp _strnicmp
#  endif
#  ifndef strcasecmp
#    define strcasecmp _stricmp
#  endif
#  ifndef strncasecmp
#    define strncasecmp _strnicmp
#  endif
/* String manipulation */
#  ifndef strlwr
#    define strlwr _strlwr
#  endif
#  ifndef strupr
#    define strupr _strupr
#  endif
#  ifndef strrev
#    define strrev _strrev
#  endif
#  ifndef strset
#    define strset _strset
#  endif
#  ifndef strnset
#    define strnset _strnset
#  endif
/* Wide string duplication and comparison */
#  ifndef wcsdup
#    define wcsdup _wcsdup
#  endif
#  ifndef wcsicmp
#    define wcsicmp _wcsicmp
#  endif
#  ifndef wcsnicmp
#    define wcsnicmp _wcsnicmp
#  endif
#  ifndef wcsicoll
#    define wcsicoll _wcsicoll
#  endif
/* Wide string manipulation */
#  ifndef wcslwr
#    define wcslwr _wcslwr
#  endif
#  ifndef wcsupr
#    define wcsupr _wcsupr
#  endif
#  ifndef wcsrev
#    define wcsrev _wcsrev
#  endif
#  ifndef wcsset
#    define wcsset _wcsset
#  endif
#  ifndef wcsnset
#    define wcsnset _wcsnset
#  endif
#endif /* __MSVCRT__ */

#endif /* __CLANG_STRING_H */
