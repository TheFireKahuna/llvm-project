//===-- cfguard.cpp - Control Flow Guard dispatch pointers ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Instrumented code calls indirect targets through these pointers. The loader
// replaces them with ntdll's validators when it enforces CFG for the image;
// otherwise the checks return and the dispatchers tail-call the target. The
// .00cfg section is read-only after load, so the pointers cannot be redirected
// by memory corruption. They are volatile so that enforcement queries observe
// the loader's writes.
//
// guard(nocf) only suppresses checks inside a function; it does not remove the
// function from the valid-target table. The x86_64 dispatcher must not be a
// valid target, so cfguard_dispatch.S defines it as data.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

namespace {

#if defined(__x86_64__)
__declspec(guard(nocf)) void __fastcall checkNop(uintptr_t) {}
#elif defined(__aarch64__)
// The target arrives in x15, outside the parameter registers.
__declspec(guard(nocf)) void checkNop(void) {}
__declspec(guard(nocf)) void dispatchNop(void) {}
#endif

} // namespace

#if defined(__x86_64__)
extern "C" char __wincrt_guard_dispatch_icall_nop[];
#endif

#pragma section(".00cfg", read)

extern "C" {

#define WINCRT_CFG_POINTER                                                     \
  __declspec(allocate(".00cfg")) __declspec(selectany) void *volatile

WINCRT_CFG_POINTER __guard_check_icall_fptr =
    reinterpret_cast<void *>(checkNop);

#if defined(__x86_64__)
WINCRT_CFG_POINTER __guard_dispatch_icall_fptr =
    __wincrt_guard_dispatch_icall_nop;
// XFG shares the CFG stubs until the loader installs its validators.
WINCRT_CFG_POINTER __guard_xfg_check_icall_fptr =
    reinterpret_cast<void *>(checkNop);
WINCRT_CFG_POINTER __guard_xfg_dispatch_icall_fptr =
    __wincrt_guard_dispatch_icall_nop;
WINCRT_CFG_POINTER __guard_xfg_table_dispatch_icall_fptr =
    __wincrt_guard_dispatch_icall_nop;
#else
WINCRT_CFG_POINTER __guard_dispatch_icall_fptr =
    reinterpret_cast<void *>(dispatchNop);
WINCRT_CFG_POINTER __guard_xfg_check_icall_fptr = nullptr;
WINCRT_CFG_POINTER __guard_xfg_dispatch_icall_fptr = nullptr;
WINCRT_CFG_POINTER __guard_xfg_table_dispatch_icall_fptr = nullptr;
#endif

// CastGuard failure handler, installed by the loader when enabled.
WINCRT_CFG_POINTER __castguard_check_failure_os_handled_fptr = nullptr;

#undef WINCRT_CFG_POINTER

// Non-zero when the loader has installed its validator for this image.
int __cdecl _guard_icall_checks_enforced(void) {
  return __guard_check_icall_fptr != reinterpret_cast<void *>(checkNop);
}

} // extern "C"
