# REQUIRES: x86

## A KCFI type that must be opened statically needs the
## compiled static scanner, which every object with a KCFI thunk defines. An
## input set without it is an error.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: not lld-link %t.obj -entry:main -out:%t.exe 2>&1 \
# RUN:   | FileCheck %s

# CHECK: error: cannot open KCFI type 11111111: __llvm_kcfi_open is not defined

  .linktypeprefixes
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .globl main
        .p2align 4
main:
        callq __llvm_kcfi_dispatch_11111111
        retq

        .data
        .quad plain

## plain has no KCFI prefix.
        .text
        .globl plain
plain:
        retq

        .weak __kcfi_typeid_plain
__kcfi_typeid_plain = 0x11111111

        .weak __llvm_kcfi_mismatch_11111111
__llvm_kcfi_mismatch_11111111 = __llvm_kcfi_trap

        .section .text,"xr",discard,__llvm_kcfi_dispatch_11111111
        .globl __llvm_kcfi_dispatch_11111111
        .p2align 4
__llvm_kcfi_dispatch_11111111:
        cmpl $0x11111111, -4(%rax)
        jne __llvm_kcfi_mismatch_11111111
        jmpq *%rax

        .section .text,"xr",discard,__llvm_kcfi_trap
        .globl __llvm_kcfi_trap
        .p2align 4
__llvm_kcfi_trap:
        movl $64, %ecx
        int $0x29

        .section .text,"xr",discard,__llvm_kcfi_open_dynamic
        .globl __llvm_kcfi_open_dynamic
        .p2align 4
__llvm_kcfi_open_dynamic:
        int3
