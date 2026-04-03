//===-- cfguard.cpp - Control Flow Guard support -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CFG function pointers patched by Windows loader at module init.
//
// SAFETY: Function pointers are initialized to no-op stubs rather than nullptr.
// If somehow called before the loader patches them (shouldn't happen in normal
// operation), they safely return instead of crashing. The loader always patches
// these with ntdll's validation routines when CFG is enforced.
//
// MUTUAL EXCLUSIVITY: Defines symbols that conflict with vcruntime:
//   __guard_check_icall_fptr       - CFG indirect call check
//   __guard_dispatch_icall_fptr    - CFG dispatch (x64/ARM64)
//   __guard_flags                  - CFG configuration flags
//   __guard_xfg_*                  - XFG (Extended Flow Guard) pointers
//   __os_arm64x_*                  - ARM64EC interop pointers
//
// See init.cpp for mutual exclusivity enforcement mechanism.
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "internal.h"

//===----------------------------------------------------------------------===//
// No-op fallback functions
//===----------------------------------------------------------------------===//
//
// These are called if CFG check is invoked before loader patches the pointers.
// Should never happen in practice, but provides safety over nullptr crash.
//
// Calling conventions per architecture:
//   x86:   __fastcall, target in ECX
//   x64:   __fastcall, target in RCX
//   ARM:   target in R0
//   ARM64: target in X15
//
// SECURITY NOTE: The check nop functions are declared __declspec(guard(nocf))
// to prevent them from being considered valid CFG targets. This matches
// vcruntime's approach of declaring dispatch stubs as data rather than code.
//

#if defined(__i386__)

// x86: __fastcall puts first arg in ECX, which is where target lives.
__declspec(guard(nocf))
static void __fastcall _guard_check_icall_nop(uintptr_t) {}

#elif defined(__x86_64__) || defined(__arm64ec__)

// x64/ARM64EC: target in RCX (first arg).
__declspec(guard(nocf))
static void __fastcall _guard_check_icall_nop(uintptr_t) {}

// Dispatch variant validates and tail-calls target (in RAX for x64).
// As a nop, we just return - caller handles the actual call.
// Note: This is suboptimal but safe. Loader always patches before use.
__declspec(guard(nocf))
static void __fastcall _guard_dispatch_icall_nop(void) {}

// XFG dispatch nop (XFG check uses regular _guard_check_icall_nop).
__declspec(guard(nocf))
static void __fastcall _guard_xfg_dispatch_icall_nop(void) {}

#elif defined(__aarch64__) && !defined(__arm64ec__)

// ARM64: target passed in X15 (not a normal parameter register).
// The nop just returns without validation.
__declspec(guard(nocf))
static void _guard_check_icall_nop(void) {}

__declspec(guard(nocf))
static void _guard_dispatch_icall_nop(void) {}

#elif defined(__arm__)

// ARM32: target in R0.
__declspec(guard(nocf))
static void _guard_check_icall_nop(uintptr_t) {}

#endif

//===----------------------------------------------------------------------===//
// CFG function pointers - placed in .00cfg for loader protection
//===----------------------------------------------------------------------===//
//
// The .00cfg section is read-only after load, protecting these pointers from
// memory corruption attacks. The loader patches them during module init.
//
// IMPORTANT: These must be volatile. The loader modifies them at runtime, but
// the compiler sees them as read-only during compilation. Without volatile,
// the compiler might optimize away reads or cache stale values.
//

#pragma section(".00cfg", read)

extern "C" {

#if defined(__i386__)
__declspec(allocate(".00cfg")) __declspec(selectany)
volatile void* __guard_check_icall_fptr = (void*)_guard_check_icall_nop;

__declspec(allocate(".00cfg")) __declspec(selectany)
volatile void* __guard_dispatch_icall_fptr = nullptr;
#elif defined(__x86_64__) || defined(__arm64ec__)
__declspec(allocate(".00cfg")) __declspec(selectany)
volatile void* __guard_check_icall_fptr = (void*)_guard_check_icall_nop;

__declspec(allocate(".00cfg")) __declspec(selectany)
volatile void* __guard_dispatch_icall_fptr = (void*)_guard_dispatch_icall_nop;
#elif defined(__aarch64__) && !defined(__arm64ec__)
__declspec(allocate(".00cfg")) __declspec(selectany)
volatile void* __guard_check_icall_fptr = (void*)_guard_check_icall_nop;

__declspec(allocate(".00cfg")) __declspec(selectany)
volatile void* __guard_dispatch_icall_fptr = (void*)_guard_dispatch_icall_nop;
#elif defined(__arm__)
__declspec(allocate(".00cfg")) __declspec(selectany)
volatile void* __guard_check_icall_fptr = (void*)_guard_check_icall_nop;

__declspec(allocate(".00cfg")) __declspec(selectany)
volatile void* __guard_dispatch_icall_fptr = nullptr;
#endif

__declspec(selectany) DWORD __guard_flags = wincrt::GuardFlags::DEFAULT_CFG;

}

//===----------------------------------------------------------------------===//
// XFG (Extended Flow Guard) - Windows 11+
//===----------------------------------------------------------------------===//

extern "C" {

#if defined(__x86_64__) && !defined(__arm64ec__)

// XFG check uses regular CFG nop as fallback (matches vcruntime behavior).
__declspec(allocate(".00cfg")) __declspec(selectany)
volatile void* __guard_xfg_check_icall_fptr = (void*)_guard_check_icall_nop;

__declspec(allocate(".00cfg")) __declspec(selectany)
volatile void* __guard_xfg_dispatch_icall_fptr = (void*)_guard_xfg_dispatch_icall_nop;

__declspec(allocate(".00cfg")) __declspec(selectany)
volatile void* __guard_xfg_table_dispatch_icall_fptr = (void*)_guard_xfg_dispatch_icall_nop;

#else

// XFG only supported on x64 currently.
__declspec(selectany) volatile void* __guard_xfg_check_icall_fptr = nullptr;
__declspec(selectany) volatile void* __guard_xfg_dispatch_icall_fptr = nullptr;
__declspec(selectany) volatile void* __guard_xfg_table_dispatch_icall_fptr = nullptr;

#endif

}

//===----------------------------------------------------------------------===//
// CastGuard (Windows 11+) - Type-safe casting via vftable validation
//===----------------------------------------------------------------------===//
//
// CastGuard validates dynamic_cast operations at runtime by checking that
// vftable pointers are within the expected range. The OS handler is called
// when validation fails; nullptr means CastGuard is disabled.
//

using _castguard_handler = void(__cdecl*)(void*);

extern "C" {

// OS-determined failure handler, set by loader if CastGuard is enabled.
__declspec(allocate(".00cfg")) __declspec(selectany)
volatile _castguard_handler __castguard_check_failure_os_handled_fptr = nullptr;

}

//===----------------------------------------------------------------------===//
// CFG enforcement detection
//===----------------------------------------------------------------------===//

namespace {

// Cache the nop function address for comparison.
inline void* getNopAddress() {
  return reinterpret_cast<void*>(_guard_check_icall_nop);
}

} // namespace

extern "C" {

/// Returns nonzero if CFG is enforced for the current module.
/// When CFG is enforced, __guard_check_icall_fptr points to ntdll's validator
/// instead of the nop stub.
int __cdecl _guard_icall_checks_enforced(void) {
  // Use volatile read to prevent caching.
  void* current = const_cast<void*>(__guard_check_icall_fptr);
  return current != getNopAddress();
}

}

//===----------------------------------------------------------------------===//
// ARM64EC interop
//===----------------------------------------------------------------------===//
//
// ARM64EC call checkers detect target architecture (ARM64EC vs x64 emulated)
// for proper exit thunk handling. Patched by loader.
//

#if defined(__arm64ec__)

extern "C" {

__declspec(selectany) void* __os_arm64x_check_icall = nullptr;
__declspec(selectany) void* __os_arm64x_check_icall_cfg = nullptr;
__declspec(selectany) void* __os_arm64x_dispatch_call = nullptr;
__declspec(selectany) void* __os_arm64x_dispatch_call_no_redirect = nullptr;

}

#endif // __arm64ec__

#endif // _WIN32
