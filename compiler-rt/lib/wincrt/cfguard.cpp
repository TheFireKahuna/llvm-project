//===-- cfguard.cpp - _guard_icall_checks_enforced ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

// Defined with the load configuration, which names it.
extern "C" void *volatile __guard_check_icall_fptr;

// Whether the loader has installed its validator, which is in ntdll.dll,
// where the stub is in this image. Comparing with the stub would take its
// address and make it a valid call target.
extern "C" int __cdecl _guard_icall_checks_enforced(void) {
  return reinterpret_cast<uintptr_t>(__guard_check_icall_fptr) -
             reinterpret_cast<uintptr_t>(&__ImageBase) >=
         wincrt::imageSize(reinterpret_cast<HMODULE>(&__ImageBase));
}
