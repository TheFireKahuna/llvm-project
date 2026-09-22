/*===---- process.h - Process control wrapper ------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_PROCESS_H
#define __CLANG_PROCESS_H

#if __STDC_HOSTED__ && __has_include_next(<process.h>)
#include_next <process.h>
#endif

/*
 * Windows Itanium: Map POSIX function names to UCRT underscore-prefixed names.
 *
 * The UCRT only exposes getpid/execv/spawnv etc. when !__STDC__, but we want
 * __STDC__ for standards compliance. Provide the mappings when targeting
 * MSVCRT/UCRT.
 */
#if defined(__MSVCRT__) || defined(_UCRT)
#  ifndef getpid
#    define getpid _getpid
#  endif
#  ifndef execl
#    define execl _execl
#  endif
#  ifndef execle
#    define execle _execle
#  endif
#  ifndef execlp
#    define execlp _execlp
#  endif
#  ifndef execlpe
#    define execlpe _execlpe
#  endif
#  ifndef execv
#    define execv _execv
#  endif
#  ifndef execve
#    define execve _execve
#  endif
#  ifndef execvp
#    define execvp _execvp
#  endif
#  ifndef execvpe
#    define execvpe _execvpe
#  endif
#  ifndef spawnl
#    define spawnl _spawnl
#  endif
#  ifndef spawnle
#    define spawnle _spawnle
#  endif
#  ifndef spawnlp
#    define spawnlp _spawnlp
#  endif
#  ifndef spawnlpe
#    define spawnlpe _spawnlpe
#  endif
#  ifndef spawnv
#    define spawnv _spawnv
#  endif
#  ifndef spawnve
#    define spawnve _spawnve
#  endif
#  ifndef spawnvp
#    define spawnvp _spawnvp
#  endif
#  ifndef spawnvpe
#    define spawnvpe _spawnvpe
#  endif
#  ifndef cwait
#    define cwait _cwait
#  endif
#endif

#endif /* __CLANG_PROCESS_H */
