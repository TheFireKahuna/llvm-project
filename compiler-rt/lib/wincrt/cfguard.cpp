//===-- cfguard.cpp - Control Flow Guard pointers -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Code built with Control Flow Guard makes each indirect call through
// __guard_dispatch_icall_fptr, target in RAX, on x86-64, and checks the target
// through __guard_check_icall_fptr, target in X15, on AArch64. The load
// configuration names both pointers, and the loader replaces them with its
// validators when it enforces the guard for the image. Until then, and in an
// image it does not guard, they hold the stubs of cfguard_dispatch.S, which
// check nothing. The linker merges .00cfg into .rdata, so the pointers are
// read-only once the loader has written them. They are volatile so that
// _guard_icall_checks_enforced sees the loader's value.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

extern "C" {

// cfguard_dispatch.S.
extern char __wincrt_guard_check_icall_nop[];
extern char __wincrt_guard_dispatch_icall_nop[];

#pragma section(".00cfg", read)

__declspec(allocate(".00cfg")) void *volatile __guard_check_icall_fptr =
    __wincrt_guard_check_icall_nop;
__declspec(allocate(".00cfg")) void *volatile __guard_dispatch_icall_fptr =
    __wincrt_guard_dispatch_icall_nop;

// Whether the loader has installed its validator for this image.
int __cdecl _guard_icall_checks_enforced(void) {
  return __guard_check_icall_fptr != __wincrt_guard_check_icall_nop;
}

} // extern "C"
