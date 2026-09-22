/*===---- direct.h - Directory handling wrapper ----------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_DIRECT_H
#define __CLANG_DIRECT_H

#if __STDC_HOSTED__ && __has_include_next(<direct.h>)
#include_next <direct.h>
#endif

/*
 * Windows Itanium: Map POSIX function names to UCRT underscore-prefixed names.
 */
#if defined(__MSVCRT__) || defined(_UCRT)
#  ifndef getcwd
#    define getcwd _getcwd
#  endif
#  ifndef chdir
#    define chdir _chdir
#  endif
#  ifndef mkdir
#    define mkdir _mkdir
#  endif
#  ifndef rmdir
#    define rmdir _rmdir
#  endif

/*
 * Windows Itanium with LLVM libc: Map UCRT underscore-prefixed directory
 * functions to POSIX equivalents.
 */
#elif defined(_WIN32_ITANIUM)
#include <unistd.h>
#include <sys/stat.h>

#define _getcwd getcwd
#define _chdir  chdir
#define _rmdir  rmdir

/* _mkdir takes one arg (no mode); POSIX mkdir takes two. */
static __inline int _mkdir(const char *path) {
  return mkdir(path, 0777);
}

#endif /* __MSVCRT__ || _UCRT / _WIN32_ITANIUM */

#endif /* __CLANG_DIRECT_H */
