//===-- cfguard_dispatch.S - Unchecked CFG dispatch ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#if defined(__x86_64__) && !defined(__arm64ec__)
        .text
        .p2align 4
        // Deliberately a data symbol: this unchecked dispatcher must not become
        // a valid CFG call target. It preserves all arguments and tail-calls RAX.
        .def __wincrt_guard_dispatch_icall_nop; .scl 2; .type 0; .endef
        .globl __wincrt_guard_dispatch_icall_nop
__wincrt_guard_dispatch_icall_nop:
        jmp *%rax
#endif
