//===-- purecall.cpp - MSVC pure virtual call bridge ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Itanium vtables call __cxa_pure_virtual directly. _purecall exists for code
// that names the MSVC entry point; the handler it consults is UCRT's, shared
// with every runtime in the process.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

extern "C" [[noreturn]] void __cxa_pure_virtual(void);
// libc++abi provides the diagnostic; C-only images abort.
WINCRT_ALTERNATENAME(__cxa_pure_virtual, abort)

extern "C" int __cdecl _purecall(void) {
  if (_purecall_handler Handler = _get_purecall_handler()) {
    Handler();
    abort();
  }
  __cxa_pure_virtual();
}
