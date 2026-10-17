//===-- cfguard_dispatch.S - Control Flow Guard stubs ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The values of the Control Flow Guard pointers until the loader installs its
// validators, and in an image it does not guard. The check returns, and the
// dispatch jumps to the target with the arguments untouched.
//
//===----------------------------------------------------------------------===//

        .text

        .p2align 4
        .def __wincrt_guard_check_icall_nop
        .scl 2
        .type 32
        .endef
        .globl __wincrt_guard_check_icall_nop
__wincrt_guard_check_icall_nop:
        ret

        .p2align 4
        .def __wincrt_guard_dispatch_icall_nop
        .scl 2
        .type 32
        .endef
        .globl __wincrt_guard_dispatch_icall_nop
__wincrt_guard_dispatch_icall_nop:
#if defined(__x86_64__)
        jmp *%rax
#elif defined(__aarch64__)
        br x15
#endif
