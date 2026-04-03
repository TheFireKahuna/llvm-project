//===-- security.cpp - /GS security cookie support ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// /GS security cookie via BCryptGenRandom (vs vcruntime's kernel32 mix).
// Adds bcrypt.dll dependency but provides cryptographic entropy. Values not
// meeting ABI constraints are fixed up deterministically per MSVC gs_cookie.c.
//
// MUTUAL EXCLUSIVITY: Defines __security_cookie, __security_cookie_complement,
// __security_check_cookie, __security_check_cookie_arm64ec (ARM64EC only),
// __security_init_cookie, __report_gsfailure which conflict with vcruntime.
// See init.cpp for enforcement mechanism.
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "internal.h"

#pragma comment(lib, "bcrypt.lib")

// selectany required: multiple CRT objects may define this symbol.
// These have C++ linkage to match SDK vcruntime.h declaration.
__declspec(selectany) uintptr_t __security_cookie = wincrt::DefaultSecurityCookie;
__declspec(selectany) uintptr_t __security_cookie_complement =
    ~wincrt::DefaultSecurityCookie;

extern "C" {

#if defined(__i386__)
// x86: cookie in ECX, not as parameter.
WINCRT_NORETURN void __cdecl __report_gsfailure(void) {
  __fastfail(static_cast<unsigned>(wincrt::FastFail::StackCookieCheckFailure));
}
#else
WINCRT_NORETURN void __cdecl __report_gsfailure(wincrt::CookieType) {
  __fastfail(static_cast<unsigned>(wincrt::FastFail::StackCookieCheckFailure));
}
#endif

}

extern "C" {

#if defined(__i386__)
void __fastcall __security_check_cookie(wincrt::CookieType cookie) {
  volatile wincrt::CookieType expected = __security_cookie;
  if (cookie != expected)
    __report_gsfailure();
}
#elif defined(__arm64ec__)
// ARM64EC uses a decorated name to prevent accidentally linking ARM64EC code
// against x64 vcruntime. The compiler emits calls to the _arm64ec variant.
void __cdecl __security_check_cookie_arm64ec(wincrt::CookieType cookie) {
  volatile wincrt::CookieType expected = __security_cookie;
  if (cookie != expected)
    __report_gsfailure(cookie);
}
// Provide undecorated name as alias for compatibility.
#pragma comment(linker, "/alternatename:__security_check_cookie=__security_check_cookie_arm64ec")
#else
void __cdecl __security_check_cookie(wincrt::CookieType cookie) {
  volatile wincrt::CookieType expected = __security_cookie;
  if (cookie != expected)
    __report_gsfailure(cookie);
}
#endif

}

extern "C" void __cdecl __security_init_cookie(void) {
  if (__security_cookie != wincrt::DefaultSecurityCookie
#if defined(__i386__)
      && (__security_cookie & wincrt::CookieHighWordMask) != 0
#endif
  ) {
    __security_cookie_complement = ~__security_cookie;
    return;
  }

  wincrt::CookieType Cookie;

  NTSTATUS status = BCryptGenRandom(nullptr,
                                    reinterpret_cast<PUCHAR>(&Cookie),
                                    sizeof(Cookie),
                                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status != 0) {
    wincrt::securityFailure("BCryptGenRandom failed", wincrt::FastFail::GsCookieInit);
  }

#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
  // 64-bit: mask to 48 bits per MSVC ABI.
  Cookie &= 0x0000FFFFFFFFFFFFULL;
#else
  // 32-bit: high word must be non-zero.
  if ((Cookie & wincrt::CookieHighWordMask) == 0) {
    Cookie |= ((Cookie | wincrt::CookieHighWordFixup) << 16);
  }
#endif

  if (Cookie == wincrt::DefaultSecurityCookie)
    Cookie += 1;

  __security_cookie = Cookie;
  __security_cookie_complement = ~Cookie;
  WINCRT_TRACE("security cookie initialized");
}

namespace {

INIT_ONCE g_securityInitOnce = INIT_ONCE_STATIC_INIT;

BOOL __stdcall securityInitCallback(PINIT_ONCE, void*, void**) {
  __security_init_cookie();
  return TRUE;
}

} // namespace

namespace wincrt {

void securityInitCookie() {
  InitOnceExecuteOnce(&g_securityInitOnce, securityInitCallback,
                      nullptr, nullptr);
}

} // namespace wincrt

#endif // _WIN32
