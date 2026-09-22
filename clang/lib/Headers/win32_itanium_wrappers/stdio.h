/*===---- stdio.h - Standard I/O wrapper -----------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_STDIO_H
#define __CLANG_STDIO_H

#if __STDC_HOSTED__ && __has_include_next(<stdio.h>)

/*
 * UCRT stdio.h emits sprintf/_snprintf/vsprintf etc. as __inline definitions.
 * In clang's C mode __inline takes GNU89 semantics: every TU that includes
 * <stdio.h> emits a strong external definition, and any two such objects
 * collide at link time. Pin the definitions to internal linkage. C++ inline
 * semantics (COMDAT) are already correct, so C only. Another UCRT header may
 * already have defined _CRT_STDIO_INLINE; override it only for this include.
 */
#if defined(_WIN32_ITANIUM) && !defined(__cplusplus)
#include <corecrt_stdio_config.h>
#pragma push_macro("_CRT_STDIO_INLINE")
#undef _CRT_STDIO_INLINE
#define _CRT_STDIO_INLINE static __inline
#endif

#include_next <stdio.h>
#if defined(_WIN32_ITANIUM) && !defined(__cplusplus)
#pragma pop_macro("_CRT_STDIO_INLINE")
#endif
#endif

/*
 * Windows Itanium: Map POSIX function names to UCRT underscore-prefixed names.
 *
 * The UCRT only exposes fileno/fdopen/popen/pclose/tempnam etc. when !__STDC__,
 * but we want __STDC__ for standards compliance. Provide the mappings when
 * _WIN32_ITANIUM is defined (set by clang for windows-itanium targets).
 */
#if defined(__MSVCRT__) || defined(_UCRT)

#  ifndef fileno
#    define fileno _fileno
#  endif
#  ifndef fdopen
#    define fdopen _fdopen
#  endif
#  ifndef popen
#    define popen _popen
#  endif
#  ifndef pclose
#    define pclose _pclose
#  endif
#  ifndef tempnam
#    define tempnam _tempnam
#  endif
#  ifndef fcloseall
#    define fcloseall _fcloseall
#  endif
#  ifndef fgetchar
#    define fgetchar _fgetchar
#  endif
#  ifndef flushall
#    define flushall _flushall
#  endif
#  ifndef fputchar
#    define fputchar _fputchar
#  endif
#  ifndef getw
#    define getw _getw
#  endif
#  ifndef putw
#    define putw _putw
#  endif
#  ifndef rmtmp
#    define rmtmp _rmtmp
#  endif
/*
 * Windows Itanium with LLVM libc: Map UCRT underscore-prefixed stdio
 * functions to POSIX/C99 equivalents.
 */
#endif

#endif /* __CLANG_STDIO_H */
