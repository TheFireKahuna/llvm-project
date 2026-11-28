//===-- loadconfig.S - The load configuration -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// _load_config_used, an IMAGE_LOAD_CONFIG_DIRECTORY64, tells the loader where
// the security cookie, the Control Flow Guard pointers and the guard and EH
// continuation tables are. The linker defines the tables, their counts and the
// guard flags as absolute symbols, zero when a feature is off. A constant
// initializer could give a symbol's value only to a pointer-sized field, and
// GuardFlags has 32 bits, so the directory is assembled.
//
// The Control Flow Guard pointers are here too. Code built with the guard
// checks the target of an indirect call through __guard_check_icall_fptr,
// target in X15 on AArch64, or makes the call through
// __guard_dispatch_icall_fptr, target in RAX on x86-64. The loader replaces
// both with its validators in an image it guards; in any other they keep
// their stubs, which check nothing. A stub that was a valid call target would
// undo the guard, and an object the compiler builds with the guard lists every
// function its data names as one, where from an assembled object the linker
// takes no address out of the slots the load configuration names.
//
//===----------------------------------------------------------------------===//

        .section .rdata,"dr"
        .globl _load_config_used
        .p2align 3
_load_config_used:
        .long 1f - _load_config_used    // Size
        .zero 0x54
        .quad __security_cookie         // 0x58 SecurityCookie
        .zero 0x10
        .quad __guard_check_icall_fptr  // 0x70 GuardCFCheckFunctionPointer
        .quad __guard_dispatch_icall_fptr // 0x78 GuardCFDispatchFunctionPointer
        .quad __guard_fids_table        // 0x80 GuardCFFunctionTable
        .quad __guard_fids_count        // 0x88 GuardCFFunctionCount
        .long __guard_flags             // 0x90 GuardFlags
        .zero 0xc
        .quad __guard_iat_table         // 0xa0 GuardAddressTakenIatEntryTable
        .quad __guard_iat_count         // 0xa8 GuardAddressTakenIatEntryCount
        .quad __guard_longjmp_table     // 0xb0 GuardLongJumpTargetTable
        .quad __guard_longjmp_count     // 0xb8 GuardLongJumpTargetCount
        .zero 0x38
        .quad __enclave_config          // 0xf8 EnclaveConfigurationPointer
        .zero 0x8
        .quad __guard_eh_cont_table     // 0x108 GuardEHContinuationTable
        .quad __guard_eh_cont_count     // 0x110 GuardEHContinuationCount
        .zero 0x30                      // through UmaFunctionPointers
1:

// The linker merges .00cfg into .rdata, so the pointers are read-only.
        .section .00cfg,"dr"
        .globl __guard_check_icall_fptr, __guard_dispatch_icall_fptr
        .p2align 3
__guard_check_icall_fptr:
        .quad __wincrt_guard_check_nop
__guard_dispatch_icall_fptr:
        .quad __wincrt_guard_dispatch_nop

// Each stub has a 16-byte unit of the guard's bitmap to itself. The dispatch
// jumps to the target with the arguments untouched.
        .text
        .p2align 4
__wincrt_guard_check_nop:
        ret
        .p2align 4
__wincrt_guard_dispatch_nop:
#if defined(__x86_64__)
        jmp *%rax
#else
        br x15
#endif
        .p2align 4

// Every table of .CRT is read-only, like .rdata, so it needs no section of its
// own.
        .section .drectve,"yni"
        .ascii " /merge:.CRT=.rdata"
