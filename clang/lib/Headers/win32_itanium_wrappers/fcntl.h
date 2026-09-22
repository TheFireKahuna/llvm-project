/*===---- fcntl.h - File control options wrapper ---------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_FCNTL_H
#define __CLANG_FCNTL_H

#if __STDC_HOSTED__ && __has_include_next(<fcntl.h>)
#include_next <fcntl.h>
#endif

/*
 * Windows Itanium: Map POSIX names to UCRT underscore-prefixed names.
 *
 * The UCRT only exposes O_RDONLY etc. when !__STDC__, but we want __STDC__
 * for standards compliance. Provide the mappings when targeting MSVCRT/UCRT.
 */
#if defined(__MSVCRT__) || defined(_UCRT)
#  ifndef O_RDONLY
#    define O_RDONLY _O_RDONLY
#  endif
#  ifndef O_WRONLY
#    define O_WRONLY _O_WRONLY
#  endif
#  ifndef O_RDWR
#    define O_RDWR _O_RDWR
#  endif
#  ifndef O_APPEND
#    define O_APPEND _O_APPEND
#  endif
#  ifndef O_CREAT
#    define O_CREAT _O_CREAT
#  endif
#  ifndef O_TRUNC
#    define O_TRUNC _O_TRUNC
#  endif
#  ifndef O_EXCL
#    define O_EXCL _O_EXCL
#  endif
#  ifndef O_TEXT
#    define O_TEXT _O_TEXT
#  endif
#  ifndef O_BINARY
#    define O_BINARY _O_BINARY
#  endif
#  ifndef O_RAW
#    define O_RAW _O_BINARY
#  endif
#  ifndef O_TEMPORARY
#    define O_TEMPORARY _O_TEMPORARY
#  endif
#  ifndef O_NOINHERIT
#    define O_NOINHERIT _O_NOINHERIT
#  endif
#  ifndef O_SEQUENTIAL
#    define O_SEQUENTIAL _O_SEQUENTIAL
#  endif
#  ifndef O_RANDOM
#    define O_RANDOM _O_RANDOM
#  endif

/* O_NONBLOCK - not in UCRT, no-op on Windows (from libc++ posix_compat.h) */
#  ifndef O_NONBLOCK
#    define O_NONBLOCK 0
#  endif
/*
 * Windows Itanium with LLVM libc: Map UCRT _O_* flags to POSIX O_* flags.
 * Third-party code uses _O_BINARY, _O_RDONLY etc. under _WIN32 guards.
 */
#endif

#endif /* __CLANG_FCNTL_H */
