//===-- formatted_io.c - Universal CRT formatted I/O ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The Universal CRT defines printf, scanf and the rest of its formatted I/O
// inline in its headers, over the __stdio_common_* functions ucrtbase.dll
// exports, and no DLL exports them. Their definitions are emitted here from
// those same headers, so that a program has one of each, which every image
// imports, as a C library's on other targets: code that declares them itself
// links, GetProcAddress and a JIT find them, and &printf is the same in every
// image. The headers declare them imported everywhere else.
//
//===----------------------------------------------------------------------===//

// A definition of the macro keeps the headers' bodies, which then define
// external functions, exported from clang_rt.wincrt_dynamic.dll.
#ifdef COMPILER_RT_SHARED_LIB
#define _CRT_STDIO_INLINE __attribute__((visibility("default")))
#else
#define _CRT_STDIO_INLINE
#endif

#include <conio.h>
#include <stdio.h>
#include <wchar.h>

#include "formatted_io.h"

// __wrap_X, which an entry object's wrap of X makes every reference to X, is X
// under a second name. The import library offers it as an import of X, which
// remains the one name clang_rt.wincrt_dynamic.dll exports. The name is set in
// assembly, since clang drops the export of a function that an alias
// declaration without one names, and it is a function symbol, as the linker
// requires of a symbol after a kcfi type prefix.
#define WINCRT_WRAP_ALIAS(Name)                                                \
  __asm__(".def __wrap_" #Name "\n.scl 2\n.type 32\n.endef\n"                 \
          ".globl __wrap_" #Name "\n.set __wrap_" #Name ", " #Name);
#ifdef COMPILER_RT_SHARED_LIB
#define WINCRT_WRAP(Name)                                                      \
  WINCRT_WRAP_ALIAS(Name)                                                      \
  __pragma(comment(linker, "/EXPORT:__wrap_" #Name ",EXPORTAS," #Name))
#else
#define WINCRT_WRAP(Name) WINCRT_WRAP_ALIAS(Name)
#endif
WINCRT_NTDLL_FORMATTED_IO(WINCRT_WRAP)
