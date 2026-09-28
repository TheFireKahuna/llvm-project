//===-- security.cpp - Stack protector cookie -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The load configuration names __security_cookie, so the loader replaces its
// default value with a random one before any code of the image runs. Only an
// image the loader did not initialize this way needs a value made here.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <stdint.h>

// The value the loader recognizes as not yet initialized. Only the low 48
// bits of a 64-bit cookie are used, so that a string copied over the stack
// cannot reproduce a valid cookie.
static constexpr uintptr_t DefaultCookie = 0x00002B992DDFA232;
static constexpr uintptr_t CookieMask = 0x0000FFFFFFFFFFFF;

extern "C" {

uintptr_t __security_cookie = DefaultCookie;

[[noreturn]] void __cdecl __report_gsfailure(uintptr_t) {
  __fastfail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
}

void __cdecl __security_check_cookie(uintptr_t Value) {
  if (Value != __security_cookie)
    __report_gsfailure(Value);
}

// Called by every entry point before any protected frame of the image. It
// changes the cookie its own epilogue would check, so it has no protector.
__attribute__((no_stack_protector)) void __cdecl __security_init_cookie(void) {
  if (__security_cookie != DefaultCookie)
    return;
  LARGE_INTEGER Counter;
  QueryPerformanceCounter(&Counter);
  FILETIME Time;
  GetSystemTimePreciseAsFileTime(&Time);
  uintptr_t Value =
      static_cast<uintptr_t>(Counter.QuadPart) ^
      (static_cast<uintptr_t>(Time.dwHighDateTime) << 32 | Time.dwLowDateTime) ^
      static_cast<uintptr_t>(GetCurrentThreadId()) << 32 ^
      GetCurrentProcessId() ^ reinterpret_cast<uintptr_t>(&Counter);
  Value &= CookieMask;
  __security_cookie = Value == DefaultCookie ? Value + 1 : Value;
}

} // extern "C"
