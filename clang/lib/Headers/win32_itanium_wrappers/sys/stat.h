/*===---- sys/stat.h - File status wrapper ---------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_SYS_STAT_H
#define __CLANG_SYS_STAT_H

#if __STDC_HOSTED__ && __has_include_next(<sys/stat.h>)
#include_next <sys/stat.h>
#endif

/*
 * Windows Itanium: Map POSIX names to UCRT underscore-prefixed names,
 * and provide S_IS* macros not present in UCRT.
 * Matches libc++ posix_compat.h.
 */
#if defined(__MSVCRT__) || defined(_UCRT)
/* File type flags - map to UCRT */
#  ifndef S_IFMT
#    define S_IFMT _S_IFMT
#  endif
#  ifndef S_IFDIR
#    define S_IFDIR _S_IFDIR
#  endif
#  ifndef S_IFCHR
#    define S_IFCHR _S_IFCHR
#  endif
#  ifndef S_IFREG
#    define S_IFREG _S_IFREG
#  endif
#  ifndef S_IFIFO
#    define S_IFIFO _S_IFIFO
#  endif

/* File types not in UCRT - define values matching libc++ posix_compat.h */
#  ifndef _S_IFBLK
#    define _S_IFBLK 0x6000
#  endif
#  ifndef _S_IFLNK
#    define _S_IFLNK 0xA000
#  endif
#  ifndef _S_IFSOCK
#    define _S_IFSOCK 0xC000
#  endif
#  ifndef S_IFBLK
#    define S_IFBLK _S_IFBLK
#  endif
#  ifndef S_IFLNK
#    define S_IFLNK _S_IFLNK
#  endif
#  ifndef S_IFSOCK
#    define S_IFSOCK _S_IFSOCK
#  endif

/* Permission flags */
#  ifndef S_IREAD
#    define S_IREAD _S_IREAD
#  endif
#  ifndef S_IWRITE
#    define S_IWRITE _S_IWRITE
#  endif
#  ifndef S_IEXEC
#    define S_IEXEC _S_IEXEC
#  endif

/* S_IS* macros - not in UCRT, from libc++ posix_compat.h */
#  ifndef S_ISDIR
#    define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#  endif
#  ifndef S_ISCHR
#    define S_ISCHR(m) (((m) & _S_IFMT) == _S_IFCHR)
#  endif
#  ifndef S_ISFIFO
#    define S_ISFIFO(m) (((m) & _S_IFMT) == _S_IFIFO)
#  endif
#  ifndef S_ISREG
#    define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#  endif
#  ifndef S_ISBLK
#    define S_ISBLK(m) (((m) & _S_IFMT) == _S_IFBLK)
#  endif
#  ifndef S_ISLNK
#    define S_ISLNK(m) (((m) & _S_IFMT) == _S_IFLNK)
#  endif
#  ifndef S_ISSOCK
#    define S_ISSOCK(m) (((m) & _S_IFMT) == _S_IFSOCK)
#  endif

/* Functions - map to the time_t-appropriate variants via _stat/_fstat macros */
#  ifndef stat
#    define stat _stat
#  endif
#  ifndef fstat
#    define fstat _fstat
#  endif
#endif /* __MSVCRT__ || _UCRT */

#endif /* __CLANG_SYS_STAT_H */
