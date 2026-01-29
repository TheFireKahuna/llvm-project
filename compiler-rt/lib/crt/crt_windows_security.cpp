//===-- crt_windows_security.cpp - Security cookie support ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Security cookie support for /GS stack buffer overrun detection.
//
// The /GS compiler flag uses __security_cookie for stack buffer overrun
// detection. We follow MSVC's proven entropy gathering approach, which
// combines multiple system values without requiring any DLL loads during
// CRT initialization (avoiding loader lock issues).
//
// Sources of entropy (matches MSVC vcruntime):
// - GetSystemTimeAsFileTime(): current time with 100ns resolution
// - GetCurrentProcessId(): process ID
// - GetCurrentThreadId(): thread ID
// - GetTickCount64(): system uptime in milliseconds
// - QueryPerformanceCounter(): high-resolution timer
// - Stack address: ASLR-derived randomness
//
// Reference: MSVC vcruntime source (gs_cookie.c, gs_support.c, gs_report.c)
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "crt_windows_internal.h"

//===----------------------------------------------------------------------===//
// Security cookie storage
//
// The cookie is read on every function return with /GS, so we don't use
// cache-line alignment here to keep the ABI simple. MSVC's actual ABI uses
// a plain uintptr_t, not a padded union.
//===----------------------------------------------------------------------===//

extern "C" {

__declspec(selectany) volatile crt::CookieType __security_cookie =
    crt::DefaultSecurityCookie;
__declspec(selectany) volatile crt::CookieType __security_cookie_complement =
    ~crt::DefaultSecurityCookie;

}

//===----------------------------------------------------------------------===//
// __report_gsfailure - called when stack cookie check fails
//
// Uses __fastfail() to terminate immediately with proper crash dump support.
// This is superior to ExitProcess() because:
// - Generates Windows Error Reporting crash dump
// - Cannot be intercepted or blocked by user code
// - Terminates without unwinding (attacker can't exploit unwind handlers)
//===----------------------------------------------------------------------===//

extern "C" {

#if defined(CRT_ARCH_X86)
// On x86, cookie is passed in ECX but we don't declare it as __fastcall param
CRT_NORETURN void __cdecl __report_gsfailure(void) {
  __fastfail(crt::FastFailStackCookieCheckFailure);
}
#else
CRT_NORETURN void __cdecl __report_gsfailure(crt::CookieType stack_cookie) {
  (void)stack_cookie;
  __fastfail(crt::FastFailStackCookieCheckFailure);
}
#endif

}

//===----------------------------------------------------------------------===//
// __security_check_cookie - verify stack cookie matches expected value
//
// The compiler normally generates this check inline, but some scenarios
// (function pointers, certain optimization levels) may require an out-of-line
// version. This implementation matches MSVC's behavior.
//
// On x86, this is __fastcall with the cookie in ECX.
// On x64/ARM64, this is a regular call with the cookie as the first argument.
//
// IMPORTANT: We use an explicit volatile read to prevent the compiler from
// optimizing away the comparison. The __security_cookie is declared volatile,
// but the comparison operand must also be read through a volatile access.
//===----------------------------------------------------------------------===//

extern "C" {

#if defined(CRT_ARCH_X86)
void __fastcall __security_check_cookie(crt::CookieType cookie) {
  // Explicit volatile read to prevent optimization
  volatile crt::CookieType expected = __security_cookie;
  if (cookie != expected)
    __report_gsfailure();
}
#else
void __cdecl __security_check_cookie(crt::CookieType cookie) {
  // Explicit volatile read to prevent optimization
  volatile crt::CookieType expected = __security_cookie;
  if (cookie != expected)
    __report_gsfailure(cookie);
}
#endif

}

//===----------------------------------------------------------------------===//
// __security_init_cookie - initialize the global security cookie
//
// Gathers entropy from multiple system sources and initializes __security_cookie.
// This function is called during CRT startup before any user code runs.
//
// Implementation matches MSVC exactly to ensure equivalent security properties.
//===----------------------------------------------------------------------===//

extern "C" void __cdecl __security_init_cookie(void) {
  crt::CookieType cookie;
  crt_filetime_t systime;
  LARGE_INTEGER perfctr;

  // Check if already initialized (Windows loader may have done it).
  // On x86, also reinitialize if high word is zero (old loader behavior).
  if (__security_cookie != crt::DefaultSecurityCookie
#if defined(CRT_ARCH_X86)
      && (__security_cookie & 0xFFFF0000) != 0
#endif
  ) {
    // Already initialized - just ensure complement is correct
    __security_cookie_complement = ~__security_cookie;
    return;
  }

  // Gather entropy - matches MSVC vcruntime gs_support.c __get_entropy()

  // System time with 100-nanosecond resolution
  GetSystemTimeAsFileTime(&systime);
#if defined(CRT_ARCH_X64) || defined(CRT_ARCH_ARM64)
  cookie = systime.scalar;
#else
  cookie = systime.ft.dwLowDateTime ^ systime.ft.dwHighDateTime;
#endif

  // Process and thread IDs
  cookie ^= GetCurrentThreadId();
  cookie ^= GetCurrentProcessId();

  // System uptime - cache the value to avoid redundant system call
  unsigned __int64 tickCount = GetTickCount64();
#if defined(CRT_ARCH_X64) || defined(CRT_ARCH_ARM64)
  cookie ^= (static_cast<crt::CookieType>(tickCount) << 56);
#endif
  cookie ^= static_cast<crt::CookieType>(tickCount);

  // High-resolution performance counter
  QueryPerformanceCounter(&perfctr);
#if defined(CRT_ARCH_X64) || defined(CRT_ARCH_ARM64)
  cookie ^= ((static_cast<crt::CookieType>(perfctr.LowPart) << 32) ^
             static_cast<crt::CookieType>(perfctr.QuadPart));
#else
  cookie ^= static_cast<crt::CookieType>(perfctr.LowPart);
  cookie ^= static_cast<crt::CookieType>(perfctr.HighPart);
#endif

  // Stack address for ASLR entropy
  cookie ^= reinterpret_cast<crt::CookieType>(&cookie);

#if defined(CRT_ARCH_X64) || defined(CRT_ARCH_ARM64)
  // On 64-bit, mask off the top 16 bits as a defense against buffer overflows
  // involving null-terminated strings.
  cookie &= 0x0000FFFFffffffffULL;
#endif

  // Ensure cookie is valid (not default, zero, or problematic values)
  if (cookie == crt::DefaultSecurityCookie) {
    cookie = crt::DefaultSecurityCookie + 1;
  }
#if defined(CRT_ARCH_X64) || defined(CRT_ARCH_ARM64)
  else if (cookie == 0) {
    cookie = crt::DefaultSecurityCookie + 1;
  }
#else
  else if ((cookie & 0xFFFF0000) == 0) {
    // On 32-bit, ensure high word is non-zero.
    // The value 0x4711 is used as a fallback seed - it's a common German
    // colloquial number meaning "random" or "arbitrary" (from the Eau de
    // Cologne brand "4711"). This matches MSVC's vcruntime implementation.
    // The high word must be non-zero because some older calling conventions
    // zero-extend 16-bit values, which could defeat the cookie protection.
    cookie |= ((cookie | 0x4711) << 16);
  }
#endif

  __security_cookie = cookie;
  __security_cookie_complement = ~cookie;
}

//===----------------------------------------------------------------------===//
// Internal init wrapper
//===----------------------------------------------------------------------===//

namespace crt {

void securityInitCookie() {
  __security_init_cookie();
}

} // namespace crt

#endif // _WIN32
