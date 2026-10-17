//===-- purecall.cpp - Pure virtual call entry point ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A pure virtual slot of a Windows Itanium vtable names _purecall, as an MSVC
// vtable does, so that the handler a program sets with the Universal CRT's
// _set_purecall_handler applies. Every image that needs it links its own.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

extern "C" [[noreturn]] void __cxa_pure_virtual(void);
// libc++abi reports the call; an image without it aborts.
WINCRT_ALTERNATENAME(__cxa_pure_virtual, abort)

extern "C" int __cdecl _purecall(void) {
  if (_purecall_handler Handler = _get_purecall_handler()) {
    Handler();
    // A handler that returns still ends the program.
    abort();
  }
  __cxa_pure_virtual();
}
