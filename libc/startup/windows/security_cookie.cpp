//===-- security_cookie.cpp - /GS stack protector cookie -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Security cookie for stack buffer overrun detection (/GS). The compiler
// inserts canary checks into functions with vulnerable buffers; this file
// provides the cookie value, validation, and initialization.
//
// Uses ProcessPrng (bcryptprimitives.dll) for entropy — always succeeds,
// no error handling needed. Fixes up values that violate ABI constraints
// per MSVC gs_cookie.c conventions.
//
//===----------------------------------------------------------------------===//

// Single-purpose import header only — keep early bootstrap lightweight.
#include "include/__llvm-libc-common.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"

#include <stdint.h>

#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
using CookieType = uint64_t;
static constexpr CookieType DEFAULT_COOKIE = 0x00002B992DDFA232ULL;
#else
using CookieType = uint32_t;
static constexpr CookieType DEFAULT_COOKIE = 0xBB40E64EUL;
static constexpr CookieType HIGH_WORD_MASK = 0xFFFF0000UL;
static constexpr CookieType HIGH_WORD_FIXUP = 0x4711UL;
#endif

static_assert(sizeof(CookieType) == sizeof(void *), "cookie size mismatch");

// selectany: multiple CRT objects may define this symbol. C++ linkage to
// match SDK vcruntime.h declaration.
__LIBC_SELECTANY_ATTR uintptr_t __security_cookie = DEFAULT_COOKIE;
__LIBC_SELECTANY_ATTR uintptr_t __security_cookie_complement = ~DEFAULT_COOKIE;

extern "C" {

// GNU-form `__attribute__((noreturn))` matches Clang's implicit builtin
// declaration of this /GS runtime; the C++11 `[[noreturn]]` form lives in
// a different attribute namespace and produces a first-declaration
// mismatch diagnostic against the builtin.
__attribute__((noreturn)) void __report_gsfailure(CookieType) {
    __asm__ volatile("int $0x29" : : "c"(7) : "memory"); // fastfail
    __builtin_unreachable();
}

void __security_check_cookie(CookieType cookie) {
  if (cookie != static_cast<CookieType>(__security_cookie))
    __report_gsfailure(cookie);
}

[[gnu::retain]] void __security_init_cookie(void) {
  if (__security_cookie != DEFAULT_COOKIE) {
    __security_cookie_complement = ~__security_cookie;
    return;
  }

  CookieType cookie;
  ::ProcessPrng(reinterpret_cast<unsigned char *>(&cookie), sizeof(cookie));

#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
  // 64-bit: mask to 48 bits per MSVC ABI.
  cookie &= 0x0000FFFFFFFFFFFFULL;
#else
  // 32-bit: high word must be non-zero.
  if ((cookie & HIGH_WORD_MASK) == 0)
    cookie |= ((cookie | HIGH_WORD_FIXUP) << 16);
#endif

  if (cookie == DEFAULT_COOKIE)
    cookie += 1;

  __security_cookie = cookie;
  __security_cookie_complement = ~cookie;
}

} // extern "C"

// __security_init_cookie is called explicitly from startup code
// (do_start.cpp / dll_startup.cpp) as the first initialization step.
// `[[gnu::retain]]` on the definition emits /INCLUDE: into `.drectve`
// so the cookie symbol is always available.
