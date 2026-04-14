//===-- cfguard.cpp - Control Flow Guard function pointers -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CFG indirect call validation pointers. The Windows loader patches these
// with ntdll's validation routines at module init. Initialized to no-op
// stubs for safety — if somehow called before patching, they return
// harmlessly rather than crashing via nullptr.
//
// The .00cfg section is read-only after load, protecting these pointers
// from memory corruption. These must be volatile — the loader modifies
// them at runtime and the compiler must not cache stale values.
//
//===----------------------------------------------------------------------===//

#include "src/__support/macros/config.h"

#include <stdint.h>

namespace {
constexpr unsigned long kGuardFlags = 0x00000100 | 0x00000400;
} // namespace

//===----------------------------------------------------------------------===//
// No-op fallbacks (safety net before loader patches)
//===----------------------------------------------------------------------===//

#if defined(__i386__)
__declspec(guard(nocf)) static void __fastcall
    guard_check_icall_nop(uintptr_t) {}
#elif defined(__x86_64__) || defined(__arm64ec__)
__declspec(guard(nocf)) static void __fastcall
    guard_check_icall_nop(uintptr_t) {}
__declspec(guard(nocf)) static void __fastcall guard_dispatch_icall_nop() {}
__declspec(guard(nocf)) static void __fastcall
    guard_xfg_dispatch_icall_nop() {}
#elif defined(__aarch64__) && !defined(__arm64ec__)
__declspec(guard(nocf)) static void guard_check_icall_nop() {}
__declspec(guard(nocf)) static void guard_dispatch_icall_nop() {}
#elif defined(__arm__)
__declspec(guard(nocf)) static void guard_check_icall_nop(uintptr_t) {}
#endif

//===----------------------------------------------------------------------===//
// CFG function pointers (placed in .00cfg for loader protection)
//===----------------------------------------------------------------------===//

#pragma section(".00cfg", read)

extern "C" {

#if defined(__i386__)
__declspec(allocate(".00cfg")) __declspec(selectany) volatile void
    *__guard_check_icall_fptr = (void *)guard_check_icall_nop;
__declspec(allocate(".00cfg")) __declspec(selectany) volatile void
    *__guard_dispatch_icall_fptr = nullptr;
#elif defined(__x86_64__) || defined(__arm64ec__)
__declspec(allocate(".00cfg")) __declspec(selectany) volatile void
    *__guard_check_icall_fptr = (void *)guard_check_icall_nop;
__declspec(allocate(".00cfg")) __declspec(selectany) volatile void
    *__guard_dispatch_icall_fptr = (void *)guard_dispatch_icall_nop;
#elif defined(__aarch64__) && !defined(__arm64ec__)
__declspec(allocate(".00cfg")) __declspec(selectany) volatile void
    *__guard_check_icall_fptr = (void *)guard_check_icall_nop;
__declspec(allocate(".00cfg")) __declspec(selectany) volatile void
    *__guard_dispatch_icall_fptr = (void *)guard_dispatch_icall_nop;
#elif defined(__arm__)
__declspec(allocate(".00cfg")) __declspec(selectany) volatile void
    *__guard_check_icall_fptr = (void *)guard_check_icall_nop;
__declspec(allocate(".00cfg")) __declspec(selectany) volatile void
    *__guard_dispatch_icall_fptr = nullptr;
#endif

// CF_INSTRUMENTED | CF_FUNCTION_TABLE_PRESENT
extern __declspec(selectany) const unsigned long __guard_flags = kGuardFlags;

} // extern "C"

//===----------------------------------------------------------------------===//
// XFG (Extended Flow Guard) — Windows 11+
//===----------------------------------------------------------------------===//

extern "C" {

#if defined(__x86_64__) && !defined(__arm64ec__)
__declspec(allocate(".00cfg")) __declspec(selectany) volatile void
    *__guard_xfg_check_icall_fptr = (void *)guard_check_icall_nop;
__declspec(allocate(".00cfg")) __declspec(selectany) volatile void
    *__guard_xfg_dispatch_icall_fptr = (void *)guard_xfg_dispatch_icall_nop;
__declspec(allocate(".00cfg")) __declspec(selectany) volatile void
    *__guard_xfg_table_dispatch_icall_fptr =
        (void *)guard_xfg_dispatch_icall_nop;
#else
__declspec(selectany) volatile void *__guard_xfg_check_icall_fptr = nullptr;
__declspec(selectany) volatile void *__guard_xfg_dispatch_icall_fptr = nullptr;
__declspec(selectany) volatile void
    *__guard_xfg_table_dispatch_icall_fptr = nullptr;
#endif

} // extern "C"

//===----------------------------------------------------------------------===//
// CastGuard (Windows 11+) — vftable pointer validation
//===----------------------------------------------------------------------===//

using CastGuardHandler = void(__cdecl *)(void *);

extern "C" {

__declspec(allocate(".00cfg")) __declspec(selectany) volatile CastGuardHandler
    __castguard_check_failure_os_handled_fptr = nullptr;

} // extern "C"

//===----------------------------------------------------------------------===//
// ARM64EC interop pointers
//===----------------------------------------------------------------------===//

#if defined(__arm64ec__)
extern "C" {
__declspec(selectany) void *__os_arm64x_check_icall = nullptr;
__declspec(selectany) void *__os_arm64x_check_icall_cfg = nullptr;
__declspec(selectany) void *__os_arm64x_dispatch_call = nullptr;
__declspec(selectany) void *__os_arm64x_dispatch_call_no_redirect = nullptr;
}
#endif
