//===-- security.cpp - /GS security cookie ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The cookie comes from the system RNG rather than vcruntime's time and
// counter mix. Values are then constrained the same way vcruntime constrains
// them, so compiler-generated checks and the load configuration see the
// layout they expect.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace {

using Cookie = uint64_t;
constexpr Cookie DefaultCookie = 0x00002B992DDFA232;
static_assert(sizeof(Cookie) == sizeof(uintptr_t), "cookie width");

} // namespace

// Global variables are not mangled, so C++ linkage matches the SDK's
// declaration in vcruntime.h.
__declspec(selectany) uintptr_t __security_cookie = DefaultCookie;
__declspec(selectany) uintptr_t __security_cookie_complement = ~DefaultCookie;

extern "C" {

// Declared by the SDK; the definitions match its signature.
void __cdecl __report_gsfailure(uintptr_t) {
  __fastfail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
}

void __cdecl __security_check_cookie(uintptr_t Value) {
  if (Value != __security_cookie)
    __report_gsfailure(Value);
}

// Called at every image entry. A cookie that is already set is kept, so a
// nested entry cannot invalidate frames that are already protected.
void __cdecl __security_init_cookie(void) {
  if (__security_cookie != DefaultCookie) {
    __security_cookie_complement = ~__security_cookie;
    return;
  }

  Cookie Value;
  if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&Value), sizeof(Value),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    __fastfail(FAST_FAIL_GS_COOKIE_INIT);

  // Only the low 48 bits participate, per the x64 /GS ABI.
  Value &= 0x0000FFFFFFFFFFFF;
  if (Value == DefaultCookie)
    ++Value;

  __security_cookie = Value;
  __security_cookie_complement = ~Value;
}

} // extern "C"
