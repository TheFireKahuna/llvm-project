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
#if defined(__MSVCRT__)
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
#elif defined(_WIN32_ITANIUM)
#include <unistd.h>
#include <pthread.h>

#define _getpid getpid
#define _execl  execl
#define _execle execle
#define _execlp execlp
#define _execv  execv
#define _execve execve
#define _execvp execvp

/* _beginthreadex/_endthreadex: MSVC CRT thread creation wrappers.
   With LLVM libc, pthread_create is the native API. Provide compat shims
   for third-party code (zstd, etc.) that uses the MSVC threading API. */
typedef unsigned (__attribute__((__stdcall__)) *_beginthreadex_proc_type)(void *);

static __inline unsigned long _beginthreadex(
    void *security, unsigned stack_size,
    _beginthreadex_proc_type start_address,
    void *arglist, unsigned initflag, unsigned *thrdaddr) {
  (void)security; (void)initflag;
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  if (stack_size)
    pthread_attr_setstacksize(&attr, stack_size);
  /* Adapt calling conventions: _beginthreadex uses __stdcall unsigned(void*),
     pthread uses __cdecl void*(void*). Cast through void* — both are
     pointer-to-function with one void* arg, only return type differs. */
  typedef void *(*pthread_start_t)(void *);
  int rc = pthread_create(&tid, &attr, (pthread_start_t)(void *)start_address, arglist);
  pthread_attr_destroy(&attr);
  if (rc != 0) return 0;
  if (thrdaddr)
    *thrdaddr = (unsigned)(unsigned long long)tid;
  /* Return a pseudo-handle. Caller expects a HANDLE (closeable via CloseHandle).
     pthread_t on Windows Itanium is the thread HANDLE. */
  return (unsigned long)tid;
}

static __inline void _endthreadex(unsigned retval) {
  pthread_exit((void *)(unsigned long long)retval);
}

#endif /* __MSVCRT__ / _WIN32_ITANIUM */

#endif /* __CLANG_PROCESS_H */
