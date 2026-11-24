//===-- security.cpp - The security cookie --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The load configuration names __security_cookie (loadconfig.S), a read-only
// word that the loader writes with a random 48-bit value before any code of
// the image runs, so __security_init_cookie, which objects that MSVC built
// call, has nothing to do.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

extern "C" {

[[noreturn]] void __cdecl __report_gsfailure(uintptr_t) {
  __fastfail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
}

void __cdecl __security_check_cookie(uintptr_t Value) {
  if (Value != __security_cookie)
    __report_gsfailure(Value);
}

void __cdecl __security_init_cookie(void) {}

} // extern "C"
