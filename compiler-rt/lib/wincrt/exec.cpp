//===-- exec.cpp - Spawning a process in place of this one ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The exec and spawn functions, which every image's entry object wraps with
// these definitions, with their traditional names. In overlay mode, which
// every exec function uses, the Universal CRT starts the new process as it
// does with _P_NOWAIT and then ends this one through its own _exit, which the
// wrap of _exit cannot reach, and which loads kernel.appcore.dll and
// msvcrt.dll to read the app model's termination policy. These spawn the
// overlay with _P_NOWAIT and end the process through _exit, which the wrap
// makes wincrt's; every other mode is the Universal CRT's.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <process.h>
#include <stdarg.h>

namespace {

[[noreturn]] void endOverlaid() { _exit(0); }

// A variadic form's arguments, from the first after the program name to the
// null that ends them, are the array the corresponding vector form takes:
// va_list is a pointer to consecutive argument slots on these targets.
template <typename Char> const Char *const *argumentArray(va_list Arguments) {
  return reinterpret_cast<const Char *const *>(Arguments);
}

// The environment that follows the null ending a variadic form's arguments.
template <typename Char>
const Char *const *environmentAfter(const Char *const *Argv) {
  while (*Argv)
    ++Argv;
  return reinterpret_cast<const Char *const *const *>(Argv + 1)[0];
}

} // namespace

// Defines the wrappers of one vector spawn function, its exec function and
// their variadic forms, for the name prefix W (_ or _w) and the suffix S (none,
// p, e or pe). E is either nothing or the environment parameter, with Env the
// expression that forwards it.
#define WINCRT_SPAWN(W, S, Char, E, Env, VariadicEnv)                          \
  __attribute__((visibility("hidden"))) intptr_t __cdecl                       \
      __real_##W##spawnv##S(int, const Char *, const Char *const * E);         \
  WINCRT_ATEXIT_API intptr_t __cdecl __wrap_##W##spawnv##S(                    \
      int Mode, const Char *File, const Char *const *Argv E) {                 \
    if (Mode != _P_OVERLAY)                                                    \
      return __real_##W##spawnv##S(Mode, File, Argv Env);                      \
    if (__real_##W##spawnv##S(_P_NOWAIT, File, Argv Env) == -1)                \
      return -1;                                                               \
    endOverlaid();                                                             \
  }                                                                            \
  WINCRT_ATEXIT_API intptr_t __cdecl __wrap_##W##execv##S(                     \
      const Char *File, const Char *const *Argv E) {                           \
    return __wrap_##W##spawnv##S(_P_OVERLAY, File, Argv Env);                  \
  }                                                                            \
  WINCRT_ATEXIT_API intptr_t __cdecl __wrap_##W##spawnl##S(                    \
      int Mode, const Char *File, ...) {                                       \
    va_list Arguments;                                                         \
    va_start(Arguments, File);                                                 \
    const Char *const *Argv = argumentArray<Char>(Arguments);                  \
    VariadicEnv;                                                               \
    intptr_t Result = __wrap_##W##spawnv##S(Mode, File, Argv Env);             \
    va_end(Arguments);                                                         \
    return Result;                                                             \
  }                                                                            \
  WINCRT_ATEXIT_API intptr_t __cdecl __wrap_##W##execl##S(const Char *File,    \
                                                          ...) {               \
    va_list Arguments;                                                         \
    va_start(Arguments, File);                                                 \
    const Char *const *Argv = argumentArray<Char>(Arguments);                  \
    VariadicEnv;                                                               \
    intptr_t Result = __wrap_##W##spawnv##S(_P_OVERLAY, File, Argv Env);       \
    va_end(Arguments);                                                         \
    return Result;                                                             \
  }

#define WINCRT_SPAWN_ENV(Char) , const Char *const *Envp
#define WINCRT_SPAWN_ENV_ARG , Envp
#define WINCRT_SPAWN_ENV_FROM_ARGS(Char)                                       \
  const Char *const *Envp = environmentAfter<Char>(Argv)

// The traditional names of the narrow functions, which reach the same
// definitions through their own wraps.
#define WINCRT_SPAWN_ALIASES(S, E)                                             \
  WINCRT_ATEXIT_API intptr_t __cdecl __wrap_spawnv##S(                         \
      int, const char *, const char *const * E)                                \
      __attribute__((alias("__wrap__spawnv" #S)));                             \
  WINCRT_ATEXIT_API intptr_t __cdecl __wrap_execv##S(const char *,             \
                                                     const char *const * E)    \
      __attribute__((alias("__wrap__execv" #S)));                              \
  WINCRT_ATEXIT_API intptr_t __cdecl __wrap_spawnl##S(int, const char *, ...)  \
      __attribute__((alias("__wrap__spawnl" #S)));                             \
  WINCRT_ATEXIT_API intptr_t __cdecl __wrap_execl##S(const char *, ...)        \
      __attribute__((alias("__wrap__execl" #S)));

extern "C" {

WINCRT_SPAWN(_, , char, , , )
WINCRT_SPAWN(_, p, char, , , )
WINCRT_SPAWN(_, e, char, WINCRT_SPAWN_ENV(char), WINCRT_SPAWN_ENV_ARG,
             WINCRT_SPAWN_ENV_FROM_ARGS(char))
WINCRT_SPAWN(_, pe, char, WINCRT_SPAWN_ENV(char), WINCRT_SPAWN_ENV_ARG,
             WINCRT_SPAWN_ENV_FROM_ARGS(char))
WINCRT_SPAWN(_w, , wchar_t, , , )
WINCRT_SPAWN(_w, p, wchar_t, , , )
WINCRT_SPAWN(_w, e, wchar_t, WINCRT_SPAWN_ENV(wchar_t), WINCRT_SPAWN_ENV_ARG,
             WINCRT_SPAWN_ENV_FROM_ARGS(wchar_t))
WINCRT_SPAWN(_w, pe, wchar_t, WINCRT_SPAWN_ENV(wchar_t), WINCRT_SPAWN_ENV_ARG,
             WINCRT_SPAWN_ENV_FROM_ARGS(wchar_t))

WINCRT_SPAWN_ALIASES(, )
WINCRT_SPAWN_ALIASES(p, )
WINCRT_SPAWN_ALIASES(e, WINCRT_SPAWN_ENV(char))
WINCRT_SPAWN_ALIASES(pe, WINCRT_SPAWN_ENV(char))

} // extern "C"
