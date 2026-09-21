/*===---- sys/types.h - System types wrapper -------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_SYS_TYPES_H
#define __CLANG_SYS_TYPES_H

#if __STDC_HOSTED__ && __has_include_next(<sys/types.h>)
#include_next <sys/types.h>
#endif

/*
 * Windows Itanium: Provide POSIX type aliases.
 * UCRT defines _ino_t etc. but only defines ino_t when !__STDC__.
 * We want __STDC__ for compliance, so provide the aliases here.
 */
#if defined(__MSVCRT__)
/* _ino_t defined by UCRT, we just add the POSIX alias */
#  ifndef __CLANG_INO_T_DEFINED
#    define __CLANG_INO_T_DEFINED
     typedef _ino_t ino_t;
#  endif

/* _dev_t defined by UCRT, we just add the POSIX alias */
#  ifndef __CLANG_DEV_T_DEFINED
#    define __CLANG_DEV_T_DEFINED
     typedef _dev_t dev_t;
#  endif

/* off_t - use 64-bit for large file support (matches libc++ posix_compat.h) */
#  ifndef __CLANG_OFF_T_DEFINED
#    define __CLANG_OFF_T_DEFINED
     typedef __INT64_TYPE__ off_t;
#  endif

/* ssize_t - not in UCRT at all */
#  ifndef __CLANG_SSIZE_T_DEFINED
#    define __CLANG_SSIZE_T_DEFINED
#    ifdef _WIN64
       typedef long long ssize_t;
#    else
       typedef long ssize_t;
#    endif
#  endif
#endif /* __MSVCRT__ */

#endif /* __CLANG_SYS_TYPES_H */
