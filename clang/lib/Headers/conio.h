/*===---- conio.h - Console I/O wrapper ------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_CONIO_H
#define __CLANG_CONIO_H

#if __STDC_HOSTED__ && __has_include_next(<conio.h>)
#include_next <conio.h>
#endif

/*
 * Windows Itanium: Map POSIX function names to UCRT underscore-prefixed names.
 *
 * The UCRT only exposes getch/kbhit/putch etc. when !__STDC__, but we want
 * __STDC__ for standards compliance. Provide the mappings when targeting
 * MSVCRT/UCRT.
 */
#if defined(__MSVCRT__)
/* Character I/O */
#  ifndef getch
#    define getch _getch
#  endif
#  ifndef getche
#    define getche _getche
#  endif
#  ifndef putch
#    define putch _putch
#  endif
#  ifndef ungetch
#    define ungetch _ungetch
#  endif
#  ifndef kbhit
#    define kbhit _kbhit
#  endif
/* String I/O */
#  ifndef cgets
#    define cgets _cgets
#  endif
#  ifndef cputs
#    define cputs _cputs
#  endif
/* Formatted I/O */
#  ifndef cprintf
#    define cprintf _cprintf
#  endif
#  ifndef cscanf
#    define cscanf _cscanf
#  endif
#endif /* __MSVCRT__ */

#endif /* __CLANG_CONIO_H */
