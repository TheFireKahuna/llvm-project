//===-- gs_cookie.S - AArch64 /GS cookie push and pop ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An AArch64 function that MSVC compiles with /GS calls __security_push_cookie
// in its prologue and __security_pop_cookie before its epilogue. The push
// allocates 16 bytes of the caller's stack, which the caller's unwind codes
// record, and stores the cookie in the upper 8, where the handler data of
// __GSHandlerCheck locates it. The cookie is stored XORed with the address of
// its slot, as gshandler.cpp expects. The caller keeps its arguments and its
// return value in registers across both calls, so only X16, X17 and the flags
// are changed.
//
//===----------------------------------------------------------------------===//

#if defined(__aarch64__)

        .text

        .p2align 2
        .def __security_push_cookie
        .scl 2
        .type 32
        .endef
        .globl __security_push_cookie
__security_push_cookie:
        sub sp, sp, #16
        adrp x16, __security_cookie
        ldr x16, [x16, :lo12:__security_cookie]
        add x17, sp, #8
        eor x16, x16, x17
        str x16, [sp, #8]
        ret

        .p2align 2
        .def __security_pop_cookie
        .scl 2
        .type 32
        .endef
        .globl __security_pop_cookie
__security_pop_cookie:
        ldr x16, [sp, #8]
        add x17, sp, #8
        eor x16, x16, x17
        adrp x17, __security_cookie
        ldr x17, [x17, :lo12:__security_cookie]
        cmp x16, x17
        b.ne 1f
        add sp, sp, #16
        ret
1:
        mov x0, x16
        b __report_gsfailure

#endif
