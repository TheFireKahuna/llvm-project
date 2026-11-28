//===-- exec.cpp - The exec and spawn functions ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The exec and spawn functions, which every image's entry object wraps with
// these definitions. In overlay mode, which every exec function uses, the
// Universal CRT starts the new process as it does with _P_NOWAIT and then
// ends this one through its own _exit, which the wrap of _exit cannot reach.
// These spawn the overlay with _P_NOWAIT and end the process through _exit,
// which the wrap makes wincrt's; every other mode is the Universal CRT's.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <process.h>
#include <stdarg.h>

// The environment that follows the null ending a variadic form's arguments.
template <typename Char>
static const Char *const *environmentAfter(const Char *const *Argv) {
  while (*Argv++)
    ;
  return *reinterpret_cast<const Char *const *const *>(Argv);
}

// A variadic form's arguments, from the first after the program name to the
// null that ends them, are the array the vector forms take: va_list is a
// pointer to consecutive argument slots on these targets.
#define WINCRT_SPAWN_VARIADIC(Spawn, Mode, Envp)                               \
  va_list Arguments;                                                           \
  va_start(Arguments, File);                                                   \
  auto Argv = reinterpret_cast<decltype(File) const *>(Arguments);             \
  intptr_t Result = Spawn(Mode, File, Argv, Envp);                             \
  va_end(Arguments);                                                           \
  return Result;

// Defines the sixteen functions with the name prefix W (_ or _w): for each of
// the two rules of finding the program, P being nothing or p, the spawn and
// exec functions that take an array or a list of arguments, with and without
// an environment. A form without one is the Universal CRT's with a null one.
#define WINCRT_SPAWN(W, P, Char)                                               \
  extern "C" intptr_t __cdecl __real_##W##spawnv##P##e(                        \
      int, const Char *, const Char *const *, const Char *const *);            \
  static intptr_t spawn##W##P(int Mode, const Char *File,                      \
                              const Char *const *Argv,                         \
                              const Char *const *Envp) {                       \
    if (Mode != _P_OVERLAY)                                                    \
      return __real_##W##spawnv##P##e(Mode, File, Argv, Envp);                 \
    if (__real_##W##spawnv##P##e(_P_NOWAIT, File, Argv, Envp) == -1)           \
      return -1;                                                               \
    _exit(0);                                                                  \
  }                                                                            \
  extern "C" {                                                                 \
  WINCRT_API intptr_t __cdecl __wrap_##W##spawnv##P(int Mode,                  \
                                                    const Char *File,          \
                                                    const Char *const *Argv) { \
    return spawn##W##P(Mode, File, Argv, nullptr);                             \
  }                                                                            \
  WINCRT_API                                                                   \
  intptr_t __cdecl __wrap_##W##spawnv##P##e(int Mode, const Char *File,        \
                                            const Char *const *Argv,           \
                                            const Char *const *Envp) {         \
    return spawn##W##P(Mode, File, Argv, Envp);                                \
  }                                                                            \
  WINCRT_API intptr_t __cdecl __wrap_##W##execv##P(const Char *File,           \
                                                   const Char *const *Argv) {  \
    return spawn##W##P(_P_OVERLAY, File, Argv, nullptr);                       \
  }                                                                            \
  WINCRT_API intptr_t __cdecl __wrap_##W##execv##P##e(                         \
      const Char *File, const Char *const *Argv, const Char *const *Envp) {    \
    return spawn##W##P(_P_OVERLAY, File, Argv, Envp);                          \
  }                                                                            \
  WINCRT_API intptr_t __cdecl __wrap_##W##spawnl##P(int Mode,                  \
                                                    const Char *File, ...){    \
      WINCRT_SPAWN_VARIADIC(spawn##W##P, Mode, nullptr)} WINCRT_API intptr_t   \
      __cdecl __wrap_##W##spawnl##P##e(int Mode, const Char *File, ...){       \
          WINCRT_SPAWN_VARIADIC(spawn##W##P, Mode,                             \
                                environmentAfter(Argv))} WINCRT_API intptr_t   \
      __cdecl __wrap_##W##execl##P(const Char *File, ...){                     \
          WINCRT_SPAWN_VARIADIC(spawn##W##P, _P_OVERLAY,                       \
                                nullptr)} WINCRT_API intptr_t __cdecl __wrap_  \
      ##W##execl##P##e(const Char *File, ...) {                                \
    WINCRT_SPAWN_VARIADIC(spawn##W##P, _P_OVERLAY, environmentAfter(Argv))     \
  }                                                                            \
  }

WINCRT_SPAWN(_, , char)
WINCRT_SPAWN(_, p, char)
WINCRT_SPAWN(_w, , wchar_t)
WINCRT_SPAWN(_w, p, wchar_t)

// The traditional names of the narrow functions, whose own wraps reach the
// same definitions.
#define WINCRT_SPAWN_ALIAS(Name)                                               \
  extern "C" WINCRT_API __typeof__(__wrap__##Name) __wrap_##Name               \
      __attribute__((alias("__wrap__" #Name)));
WINCRT_SPAWN_FORMS(WINCRT_SPAWN_ALIAS, )
