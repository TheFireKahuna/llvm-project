//===-- formatted_io.c - The Universal CRT's formatted I/O ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The Universal CRT defines printf, scanf and the rest of its formatted I/O
// inline in its headers, over the __stdio_common_ and __conio_common_
// functions that ucrtbase.dll exports, and no DLL exports them. They are
// defined here, from those headers, so that a program has one of each, which
// every image imports, as with a C library elsewhere: a unit that declares
// one itself links, GetProcAddress and a JIT find it, and its address is the
// same in every image. Everywhere else the headers declare them imported.
//
//===----------------------------------------------------------------------===//

// A unit that defines the macro keeps the headers' bodies, which then define
// external functions.
#ifdef COMPILER_RT_SHARED_LIB
#define _CRT_STDIO_INLINE __declspec(dllexport)
#else
#define _CRT_STDIO_INLINE
#endif

#include <conio.h>
#include <stdio.h>
#include <wchar.h>

#include "ucrt_names.h"

// __wrap_X, which the entry objects' wraps make of every reference to X, is X
// under a second name, which the DLL's import library offers as an import of
// X: X stays the function's one exported name. The name is set in assembly,
// as a function's, since an alias declaration would take the export from X.
#define WINCRT_WRAP_ALIAS(Name)                                                \
  __asm__(".def __wrap_" #Name "; .scl 2; .type 32; .endef\n"                  \
          ".globl __wrap_" #Name "\n.set __wrap_" #Name ", " #Name);
#ifdef COMPILER_RT_SHARED_LIB
#define WINCRT_WRAP(Name)                                                      \
  WINCRT_WRAP_ALIAS(Name)                                                      \
  __pragma(comment(linker, "/export:__wrap_" #Name ",EXPORTAS," #Name))
#else
#define WINCRT_WRAP(Name) WINCRT_WRAP_ALIAS(Name)
#endif
WINCRT_NTDLL_FORMATTED_IO(WINCRT_WRAP)
